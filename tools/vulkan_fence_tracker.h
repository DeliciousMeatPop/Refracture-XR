// Fence state kept in the guest by Refract's Vulkan layer. Under gfxstream every vkGetFenceStatus, vkResetFences and
// vkWaitForFences is a blocking round trip: the guest pings the host (a VM exit) and waits until the host has decoded
// everything queued before the call. UE4 polls its fences ~900 times a second (Resident Evil 4), and those stalls were
// a quarter of its render thread. A helper thread waits for each submitted fence on its own host connection and resets
// it on the host as soon as it signals, so the application's queries and resets are answered without leaving the guest.
#pragma once
#include <vulkan/vulkan.h>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace refract {

class FenceTracker {
public:
    FenceTracker(VkDevice device,PFN_vkGetDeviceProcAddr gdpa):device_(device),
        wait_(reinterpret_cast<PFN_vkWaitForFences>(gdpa(device,"vkWaitForFences"))),
        reset_(reinterpret_cast<PFN_vkResetFences>(gdpa(device,"vkResetFences"))),
        status_(reinterpret_cast<PFN_vkGetFenceStatus>(gdpa(device,"vkGetFenceStatus"))),
        helper_([this]{run();}) {}
    ~FenceTracker(){{std::lock_guard lock(mutex_);stop_=true;}changed_.notify_all();helper_.join();}

    void created(VkFence fence,bool signaled){
        std::lock_guard lock(mutex_);fences_[fence]=signaled?State::HostSignaled:State::Unsignaled;
    }
    // Waits for the helper to let go of the fence: the application may destroy it once its work has completed,
    // which can be before the helper has seen it signal.
    void destroyed(VkFence fence){
        std::unique_lock lock(mutex_);
        changed_.wait(lock,[&]{auto it=fences_.find(fence);return it==fences_.end()||it->second!=State::Pending;});
        fences_.erase(fence);
    }
    // The fence's payload is no longer the one the helper tracks (acquire, import): leave it to the driver.
    void forget(VkFence fence){if(fence){destroyed(fence);}}
    // After a successful submit that signals the fence.
    void submitted(VkFence fence){
        if(!fence)return;
        {std::lock_guard lock(mutex_);auto it=fences_.find(fence);if(it==fences_.end())return;
         it->second=State::Pending;queue_.push_back(fence);}
        changed_.notify_all();
    }
    VkResult status(VkDevice device,VkFence fence){
        {std::lock_guard lock(mutex_);auto it=fences_.find(fence);
         if(it!=fences_.end())return result(it->second);}
        return status_(device,fence);
    }
    VkResult reset(VkDevice device,uint32_t count,const VkFence* fences){
        thread_local std::vector<VkFence> host;host.clear();
        {std::lock_guard lock(mutex_);
         for(uint32_t i=0;i<count;++i){
             auto it=fences_.find(fences[i]);
             if(it==fences_.end()||it->second==State::HostSignaled||it->second==State::Pending)host.push_back(fences[i]);
             else if(it->second==State::Signaled)it->second=State::Unsignaled;
         }}
        if(host.empty())return VK_SUCCESS;
        auto r=reset_(device,static_cast<uint32_t>(host.size()),host.data());
        if(r==VK_SUCCESS){std::lock_guard lock(mutex_);
            for(auto f:host){auto it=fences_.find(f);if(it!=fences_.end()&&it->second==State::HostSignaled)it->second=State::Unsignaled;}}
        return r;
    }
    VkResult wait(VkDevice device,uint32_t count,const VkFence* fences,VkBool32 all,uint64_t timeout){
        using Clock=std::chrono::steady_clock;
        // Timeouts past ~30 years are "forever" and would overflow the clock.
        const bool forever=timeout>(uint64_t(1)<<60);
        const auto deadline=Clock::now()+std::chrono::nanoseconds(forever?0:timeout);
        std::unique_lock lock(mutex_);
        bool untracked=false;
        for(uint32_t i=0;i<count;++i)untracked|=!fences_.count(fences[i]);
        VkResult r=VK_TIMEOUT;
        auto done=[&]{
            uint32_t signaled=0;
            for(uint32_t i=0;i<count;++i){
                auto it=fences_.find(fences[i]);
                auto s=it!=fences_.end()?result(it->second):status_(device,fences[i]);
                if(s<0){r=s;return true;}
                signaled+=s==VK_SUCCESS;
            }
            if(all?signaled==count:signaled>0){r=VK_SUCCESS;return true;}
            return false;
        };
        if(done()||timeout==0)return r;
        if(!untracked){
            if(forever)changed_.wait(lock,done);else if(!changed_.wait_until(lock,deadline,done))return VK_TIMEOUT;
            return r;
        }
        // A fence the layer does not track (rare): poll the driver for it, a short sleep apart.
        while(!done()){
            if(!forever&&Clock::now()>=deadline)return VK_TIMEOUT;
            changed_.wait_for(lock,std::chrono::microseconds(200));
        }
        return r;
    }
    // Exporting a sync file resets the fence. A fence the helper already reset on the host exports as -1
    // ("already signaled"), which the driver could no longer produce.
    bool exportSignaled(VkFence fence){
        std::unique_lock lock(mutex_);
        changed_.wait(lock,[&]{auto it=fences_.find(fence);return it==fences_.end()||it->second!=State::Pending;});
        auto it=fences_.find(fence);
        if(it==fences_.end()||it->second!=State::Signaled){if(it!=fences_.end())it->second=State::Unsignaled;return false;}
        it->second=State::Unsignaled;return true;
    }

private:
    // Unsignaled and Signaled both mean the host fence is unsignaled; HostSignaled is signaled on the host too
    // (created signaled, or the helper's reset failed).
    enum class State:uint8_t{Unsignaled,Pending,Signaled,HostSignaled,Lost};
    static VkResult result(State s){
        return s==State::Lost?VK_ERROR_DEVICE_LOST:s==State::Signaled||s==State::HostSignaled?VK_SUCCESS:VK_NOT_READY;
    }
    // Each call below is a host round trip, so the helper makes as few as it can: it waits for the oldest fence
    // (fences on one queue signal in submission order), then checks all the later ones with one zero-timeout
    // wait, and resets every fence that finished with one call.
    void run(){
        std::vector<VkFence> batch;
        std::unique_lock lock(mutex_);
        for(;;){
            changed_.wait(lock,[&]{return stop_||!queue_.empty();});
            if(stop_)return;
            batch.assign(queue_.begin(),queue_.end());
            lock.unlock();
            auto r=wait_(device_,1,batch.data(),VK_TRUE,100'000'000);
            size_t done=0;
            State next=State::Pending;
            if(r==VK_SUCCESS){
                done=1;
                if(batch.size()>1&&wait_(device_,static_cast<uint32_t>(batch.size()-1),batch.data()+1,VK_TRUE,0)==VK_SUCCESS)done=batch.size();
                next=reset_(device_,static_cast<uint32_t>(done),batch.data())==VK_SUCCESS?State::Signaled:State::HostSignaled;
            }else if(r!=VK_TIMEOUT){done=1;next=State::Lost;}
            lock.lock();
            for(size_t i=0;i<done;++i){
                queue_.pop_front();
                auto it=fences_.find(batch[i]);if(it!=fences_.end()&&it->second==State::Pending)it->second=next;
            }
            if(done)changed_.notify_all();
        }
    }
    VkDevice device_;
    PFN_vkWaitForFences wait_;
    PFN_vkResetFences reset_;
    PFN_vkGetFenceStatus status_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::unordered_map<VkFence,State> fences_;
    std::deque<VkFence> queue_;
    bool stop_=false;
    std::thread helper_;
};

}  // namespace refract
