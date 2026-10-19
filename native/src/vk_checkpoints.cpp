// REFRACT_VK_CHECKPOINTS=1: finds the GPU work that hangs or faults when a game loses the device.
//
// With VK_NV_device_diagnostic_checkpoints the app's command buffers get a checkpoint before every render pass,
// draw, dispatch and copy. The first call that reports VK_ERROR_DEVICE_LOST logs, per queue, the last checkpoints
// the GPU reached at the top and bottom of the pipe, with what each one marks (render pass size, pipeline, draw
// parameters) and the commands recorded just before it.
#include <windows.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <string>
#include <mutex>
#include <set>
#include <unordered_map>
#include <vector>

#include "thunks.h"
#include "vulkan_hle.h"

namespace rn {

namespace {

template <typename T>
T Fn(const char* name) {
    return reinterpret_cast<T>(VulkanHostProc(name));
}

enum Kind : uint32_t { kBeginPass, kEndPass, kDraw, kDrawIndexed, kDrawIndirect, kDispatch, kCopy, kSubmit };
const char* const kKindNames[] = {"begin render pass", "end render pass", "draw", "draw indexed",
                                  "draw indirect", "dispatch", "copy/upload", "submit"};

struct Mark {
    Kind kind;
    VkCommandBuffer cb;
    uint64_t object;  // render pass / pipeline / destination image
    uint32_t a, b, c, d;
};

std::mutex g_mu;
std::vector<Mark> g_marks;  // marker id - 1 -> what it marks (a ring once full)
constexpr size_t kMaxMarks = 1 << 22;
uint64_t g_next = 0;
std::unordered_map<VkCommandBuffer, uint64_t> g_pipeline;  // last bound graphics/compute pipeline
std::set<VkQueue> g_queues;
// Shader modules of each pipeline (vertex/fragment or compute); modules are dumped as <handle>.spv.
std::unordered_map<uint64_t, std::vector<uint64_t>> g_pipeline_modules;

std::string SpirvDir() {
    char tmp[MAX_PATH] = {};
    GetTempPathA(sizeof(tmp), tmp);
    std::string dir = std::string(tmp) + "refract_spirv";
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir;
}
std::atomic<bool> g_dumped{false};

void Checkpoint(VkCommandBuffer cb, Kind kind, uint64_t object, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0,
                uint32_t d = 0) {
    static auto set = Fn<PFN_vkCmdSetCheckpointNV>("vkCmdSetCheckpointNV");
    if (!set)
        return;
    uint64_t id;
    {
        std::lock_guard lock(g_mu);
        if (g_marks.size() < kMaxMarks)
            g_marks.push_back({});
        id = ++g_next;
        g_marks[(id - 1) % kMaxMarks] = {kind, cb, object, a, b, c, d};
    }
    set(cb, reinterpret_cast<void*>(static_cast<uintptr_t>(id)));
}

uint64_t PipelineOf(VkCommandBuffer cb) {
    std::lock_guard lock(g_mu);
    auto it = g_pipeline.find(cb);
    return it == g_pipeline.end() ? 0 : it->second;
}

void Describe(const char* prefix, uint64_t id) {
    if (!id || id > g_next || g_next - id >= kMaxMarks) {
        Log("%s marker %" PRIu64 " (unknown)", prefix, id);
        return;
    }
    const Mark& m = g_marks[(id - 1) % kMaxMarks];
    std::string modules;
    if (m.kind == kDraw || m.kind == kDrawIndexed || m.kind == kDrawIndirect || m.kind == kDispatch) {
        auto it = g_pipeline_modules.find(m.object);
        if (it != g_pipeline_modules.end())
            for (uint64_t mod : it->second) {
                char hex[24];
                snprintf(hex, sizeof(hex), " %" PRIx64, mod);
                modules += hex;
            }
    }
    Log("%s #%" PRIu64 " %s cb=%p obj=0x%" PRIx64 " [%u %u %u %u]%s%s", prefix, id, kKindNames[m.kind],
        static_cast<void*>(m.cb), m.object, m.a, m.b, m.c, m.d, modules.empty() ? "" : " modules", modules.c_str());
}

void Dump(const char* what) {
    if (g_dumped.exchange(true))
        return;
    auto get = Fn<PFN_vkGetQueueCheckpointDataNV>("vkGetQueueCheckpointDataNV");
    Log("==== %s lost the device; checkpoints (%" PRIu64 " recorded) ====", what, g_next);
    if (!get)
        return;
    std::lock_guard lock(g_mu);
    for (VkQueue q : g_queues) {
        uint32_t n = 0;
        get(q, &n, nullptr);
        std::vector<VkCheckpointDataNV> data(n, {VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV});
        get(q, &n, data.data());
        Log("queue %p: %u checkpoint(s)", static_cast<void*>(q), n);
        // Everything between the last completed (bottom of pipe) and the last started (top of pipe) checkpoint:
        // the work in flight when the GPU stopped.
        uint64_t lo = UINT64_MAX, hi = 0;
        for (const auto& d : data) {
            const uint64_t id = reinterpret_cast<uintptr_t>(d.pCheckpointMarker);
            lo = std::min(lo, id), hi = std::max(hi, id);
        }
        if (n >= 2 && hi > lo && hi - lo < 4096 && g_next - lo < kMaxMarks) {
            Log("in flight (#%" PRIu64 " .. #%" PRIu64 "):", lo + 1, hi);
            const VkCommandBuffer cb = g_marks[(hi - 1) % kMaxMarks].cb;
            for (uint64_t j = lo + 1; j <= hi; ++j)
                if (g_marks[(j - 1) % kMaxMarks].cb == cb)
                    Describe("   ", j);
        }
        for (const auto& d : data) {
            const uint64_t id = reinterpret_cast<uintptr_t>(d.pCheckpointMarker);
            char prefix[64];
            snprintf(prefix, sizeof(prefix), "  stage 0x%x:", d.stage);
            Describe(prefix, id);
            // What the same command buffer recorded just before and after it.
            if (id && id <= g_next) {
                const VkCommandBuffer cb = g_marks[(id - 1) % kMaxMarks].cb;
                int shown = 0;
                for (uint64_t j = id - 1; j > 0 && g_next - j < kMaxMarks && shown < 8; --j)
                    if (g_marks[(j - 1) % kMaxMarks].cb == cb)
                        Describe("      before:", j), ++shown;
                shown = 0;
                for (uint64_t j = id + 1; j <= g_next && shown < 4; ++j)
                    if (g_marks[(j - 1) % kMaxMarks].cb == cb)
                        Describe("      after: ", j), ++shown;
            }
        }
    }
}

VkResult Check(VkResult r, const char* what) {
    if (r == VK_ERROR_DEVICE_LOST)
        Dump(what);
    return r;
}

// --- hooks ---------------------------------------------------------------------------------------------------

void VKAPI_CALL C_vkGetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue* queue) {
    Fn<PFN_vkGetDeviceQueue>("vkGetDeviceQueue")(device, family, index, queue);
    std::lock_guard lock(g_mu);
    g_queues.insert(*queue);
}
void VKAPI_CALL C_vkGetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2* info, VkQueue* queue) {
    Fn<PFN_vkGetDeviceQueue2>("vkGetDeviceQueue2")(device, info, queue);
    std::lock_guard lock(g_mu);
    g_queues.insert(*queue);
}
VkResult VKAPI_CALL C_vkWaitForFences(VkDevice device, uint32_t n, const VkFence* fences, VkBool32 all, uint64_t timeout) {
    return Check(Fn<PFN_vkWaitForFences>("vkWaitForFences")(device, n, fences, all, timeout), "vkWaitForFences");
}
VkResult VKAPI_CALL C_vkGetFenceStatus(VkDevice device, VkFence fence) {
    return Check(Fn<PFN_vkGetFenceStatus>("vkGetFenceStatus")(device, fence), "vkGetFenceStatus");
}
VkResult VKAPI_CALL C_vkQueueWaitIdle(VkQueue queue) {
    return Check(Fn<PFN_vkQueueWaitIdle>("vkQueueWaitIdle")(queue), "vkQueueWaitIdle");
}
VkResult VKAPI_CALL C_vkDeviceWaitIdle(VkDevice device) {
    return Check(Fn<PFN_vkDeviceWaitIdle>("vkDeviceWaitIdle")(device), "vkDeviceWaitIdle");
}
VkResult VKAPI_CALL C_vkWaitSemaphores(VkDevice device, const VkSemaphoreWaitInfo* info, uint64_t timeout) {
    return Check(Fn<PFN_vkWaitSemaphores>("vkWaitSemaphores")(device, info, timeout), "vkWaitSemaphores");
}

VkResult VKAPI_CALL C_vkCreateComputePipelines(VkDevice device, VkPipelineCache cache, uint32_t n,
                                               const VkComputePipelineCreateInfo* infos,
                                               const VkAllocationCallbacks*, VkPipeline* out) {
    VkResult r = Fn<PFN_vkCreateComputePipelines>("vkCreateComputePipelines")(device, cache, n, infos, nullptr, out);
    std::lock_guard lock(g_mu);
    for (uint32_t i = 0; i < n; ++i)
        if (out[i])
            g_pipeline_modules[reinterpret_cast<uint64_t>(out[i])] = {reinterpret_cast<uint64_t>(infos[i].stage.module)};
    return r;
}

void VKAPI_CALL C_vkCmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint point, VkPipeline pipeline) {
    {
        std::lock_guard lock(g_mu);
        g_pipeline[cb] = reinterpret_cast<uint64_t>(pipeline);
    }
    Fn<PFN_vkCmdBindPipeline>("vkCmdBindPipeline")(cb, point, pipeline);
}
void VKAPI_CALL C_vkCmdDraw(VkCommandBuffer cb, uint32_t vertices, uint32_t instances, uint32_t first_vertex,
                            uint32_t first_instance) {
    Checkpoint(cb, kDraw, PipelineOf(cb), vertices, instances, first_vertex, first_instance);
    Fn<PFN_vkCmdDraw>("vkCmdDraw")(cb, vertices, instances, first_vertex, first_instance);
}
void VKAPI_CALL C_vkCmdDrawIndexed(VkCommandBuffer cb, uint32_t indices, uint32_t instances, uint32_t first_index,
                                   int32_t vertex_offset, uint32_t first_instance) {
    Checkpoint(cb, kDrawIndexed, PipelineOf(cb), indices, instances, first_index, static_cast<uint32_t>(vertex_offset));
    Fn<PFN_vkCmdDrawIndexed>("vkCmdDrawIndexed")(cb, indices, instances, first_index, vertex_offset, first_instance);
}
void VKAPI_CALL C_vkCmdDrawIndirect(VkCommandBuffer cb, VkBuffer buffer, VkDeviceSize offset, uint32_t count,
                                    uint32_t stride) {
    Checkpoint(cb, kDrawIndirect, PipelineOf(cb), count, stride, static_cast<uint32_t>(offset));
    Fn<PFN_vkCmdDrawIndirect>("vkCmdDrawIndirect")(cb, buffer, offset, count, stride);
}
void VKAPI_CALL C_vkCmdDrawIndexedIndirect(VkCommandBuffer cb, VkBuffer buffer, VkDeviceSize offset, uint32_t count,
                                           uint32_t stride) {
    Checkpoint(cb, kDrawIndirect, PipelineOf(cb), count, stride, static_cast<uint32_t>(offset), 1);
    Fn<PFN_vkCmdDrawIndexedIndirect>("vkCmdDrawIndexedIndirect")(cb, buffer, offset, count, stride);
}
void VKAPI_CALL C_vkCmdDispatch(VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z) {
    Checkpoint(cb, kDispatch, PipelineOf(cb), x, y, z);
    Fn<PFN_vkCmdDispatch>("vkCmdDispatch")(cb, x, y, z);
}
void VKAPI_CALL C_vkCmdEndRenderPass(VkCommandBuffer cb) {
    Checkpoint(cb, kEndPass, 0);
    Fn<PFN_vkCmdEndRenderPass>("vkCmdEndRenderPass")(cb);
}
void VKAPI_CALL C_vkCmdCopyBuffer(VkCommandBuffer cb, VkBuffer src, VkBuffer dst, uint32_t n, const VkBufferCopy* r) {
    Checkpoint(cb, kCopy, reinterpret_cast<uint64_t>(dst), n, n ? static_cast<uint32_t>(r[0].size) : 0, 1);
    Fn<PFN_vkCmdCopyBuffer>("vkCmdCopyBuffer")(cb, src, dst, n, r);
}
void VKAPI_CALL C_vkCmdCopyImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl,
                                 uint32_t n, const VkImageCopy* r) {
    Checkpoint(cb, kCopy, reinterpret_cast<uint64_t>(dst), n, n ? r[0].extent.width : 0, 2);
    Fn<PFN_vkCmdCopyImage>("vkCmdCopyImage")(cb, src, sl, dst, dl, n, r);
}
void VKAPI_CALL C_vkCmdBlitImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl,
                                 uint32_t n, const VkImageBlit* r, VkFilter filter) {
    Checkpoint(cb, kCopy, reinterpret_cast<uint64_t>(dst), n, 0, 3);
    Fn<PFN_vkCmdBlitImage>("vkCmdBlitImage")(cb, src, sl, dst, dl, n, r, filter);
}

}  // namespace

bool CheckpointsEnabled() {
    static const bool on = [] {
        char v[8] = {};
        return GetEnvironmentVariableA("REFRACT_VK_CHECKPOINTS", v, sizeof(v)) && v[0] == '1';
    }();
    return on;
}

void CheckpointBeginRenderPass(void* cb, const void* begin_info) {
    if (!CheckpointsEnabled())
        return;
    auto* bi = static_cast<const VkRenderPassBeginInfo*>(begin_info);
    Checkpoint(static_cast<VkCommandBuffer>(cb), kBeginPass, reinterpret_cast<uint64_t>(bi->renderPass),
               bi->renderArea.extent.width, bi->renderArea.extent.height, bi->clearValueCount,
               static_cast<uint32_t>(reinterpret_cast<uint64_t>(bi->framebuffer)));
}

void CheckpointUpload(void* cb, uint64_t image, uint32_t regions) {
    if (CheckpointsEnabled())
        Checkpoint(static_cast<VkCommandBuffer>(cb), kCopy, image, regions, 0, 0);
}

void CheckpointShaderModule(unsigned long long module, const void* code, size_t bytes) {
    if (!CheckpointsEnabled())
        return;
    static const std::string dir = SpirvDir();
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s/%llx.spv", dir.c_str(), module);
    if (FILE* f = fopen(path, "wb")) {
        fwrite(code, 1, bytes, f);
        fclose(f);
    }
}

void CheckpointPipelines(const void* create_infos, unsigned count, const void* pipelines) {
    if (!CheckpointsEnabled())
        return;
    auto* infos = static_cast<const VkGraphicsPipelineCreateInfo*>(create_infos);
    auto* out = static_cast<const VkPipeline*>(pipelines);
    std::lock_guard lock(g_mu);
    for (unsigned i = 0; i < count; ++i) {
        if (!out[i])
            continue;
        auto& mods = g_pipeline_modules[reinterpret_cast<uint64_t>(out[i])];
        mods.clear();
        for (uint32_t s = 0; s < infos[i].stageCount; ++s)
            mods.push_back(reinterpret_cast<uint64_t>(infos[i].pStages[s].module));
    }
}

void CheckpointDeviceLost(int result, const char* what) {
    if (CheckpointsEnabled() && result == VK_ERROR_DEVICE_LOST)
        Dump(what);
}

void AddCheckpointThunks(std::map<std::string, ThunkFn>& m) {
    if (!CheckpointsEnabled())
        return;
    RN_INFO("Vulkan: REFRACT_VK_CHECKPOINTS=1, recording diagnostic checkpoints");
    m["vkGetDeviceQueue"] = &Wrap<&C_vkGetDeviceQueue>;
    m["vkGetDeviceQueue2"] = &Wrap<&C_vkGetDeviceQueue2>;
    m["vkWaitForFences"] = &Wrap<&C_vkWaitForFences>;
    m["vkGetFenceStatus"] = &Wrap<&C_vkGetFenceStatus>;
    m["vkQueueWaitIdle"] = &Wrap<&C_vkQueueWaitIdle>;
    m["vkDeviceWaitIdle"] = &Wrap<&C_vkDeviceWaitIdle>;
    m["vkWaitSemaphores"] = &Wrap<&C_vkWaitSemaphores>;
    m["vkWaitSemaphoresKHR"] = &Wrap<&C_vkWaitSemaphores>;
    m["vkCmdBindPipeline"] = &Wrap<&C_vkCmdBindPipeline>;
    m["vkCreateComputePipelines"] = &Wrap<&C_vkCreateComputePipelines>;
    m["vkCmdDraw"] = &Wrap<&C_vkCmdDraw>;
    m["vkCmdDrawIndexed"] = &Wrap<&C_vkCmdDrawIndexed>;
    m["vkCmdDrawIndirect"] = &Wrap<&C_vkCmdDrawIndirect>;
    m["vkCmdDrawIndexedIndirect"] = &Wrap<&C_vkCmdDrawIndexedIndirect>;
    m["vkCmdDispatch"] = &Wrap<&C_vkCmdDispatch>;
    m["vkCmdEndRenderPass"] = &Wrap<&C_vkCmdEndRenderPass>;
    m["vkCmdCopyBuffer"] = &Wrap<&C_vkCmdCopyBuffer>;
    m["vkCmdCopyImage"] = &Wrap<&C_vkCmdCopyImage>;
    m["vkCmdBlitImage"] = &Wrap<&C_vkCmdBlitImage>;
}

}  // namespace rn
