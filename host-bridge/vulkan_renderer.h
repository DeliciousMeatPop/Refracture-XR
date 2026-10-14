#pragma once
#if !defined(_WIN32)
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES  // libvulkan.so.1 is opened at run time, like the OpenXR loader.
#endif
#include <vulkan/vulkan.h>
#ifndef XR_USE_GRAPHICS_API_VULKAN
#define XR_USE_GRAPHICS_API_VULKAN
#endif
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <vector>

namespace refract::host {

// The Linux host's GPU side: a Vulkan device made through the PC runtime (XR_KHR_vulkan_enable2) and the
// copies that put Android frames into its swapchain images. Only the frame loop's thread uses it, and that
// thread is also the only one calling the runtime, which shares the queue.
class VulkanRenderer {
public:
    VulkanRenderer() = default;
    VulkanRenderer(const VulkanRenderer&) = delete;
    VulkanRenderer& operator=(const VulkanRenderer&) = delete;
    ~VulkanRenderer();

    bool create(XrInstance instance, XrSystemId system, PFN_xrGetInstanceProcAddr getInstanceProcAddr);
    const XrGraphicsBindingVulkan2KHR& binding() const { return binding_; }

    // Copies `layers` tightly packed RGBA8 images of width x height into the top-left of the first layers of
    // a swapchain image the caller has acquired and waited for (so it is in COLOR_ATTACHMENT_OPTIMAL).
    bool upload(VkImage image, const uint8_t* pixels, uint32_t width, uint32_t height, uint32_t layers);
    // Copies the top-left width x height of one layer of `source` into layer 0 of `dest`; both are acquired
    // swapchain images.
    bool copy(VkImage source, uint32_t sourceLayer, VkImage dest, uint32_t width, uint32_t height);
    // Before destroying swapchains whose images a copy may still read.
    void wait_idle();

    // GPU sharing: Android's eye images arrive as the opaque-FD Vulkan memory Refract's GPU layer exports (as the PC
    // viewer imports them), so no pixels cross TCP. The image thread copies each frame's pair into a cache slot
    // (receive_gpu_pair) and the game may then reuse its textures; the frame loop blits from the slot into swapchain
    // images (blit_cached). Needs a second queue for the image thread, and the external-memory extensions.
    static constexpr int kSlots = 4;
    bool gpu_share_ready() const { return receiveQueue_ != VK_NULL_HANDLE && externalMemoryFd_; }
    // Image thread. Copies the width x height pair `session` shares into slot `slot`, held in `targetFormat`'s
    // sRGB-ness so the later blit keeps the bytes. Returns when the copy has finished.
    bool receive_gpu_pair(uint64_t session, uint32_t width, uint32_t height, VkFormat targetFormat, int slot);
    struct BlitRegion {
        uint32_t srcLayer = 0;
        int32_t srcX = 0, srcY = 0;
        uint32_t width = 0, height = 0;
        uint32_t dstLayer = 0;
        int32_t dstX = 0, dstY = 0;
        bool flip = false;  // The region's rows are stored bottom-up.
    };
    // Frame loop thread. Blits regions of a cache slot into a swapchain image the caller has acquired and waited for
    // (layers [0, dstLayers) of it). A BGRA swapchain gets its channels swapped.
    bool blit_cached(int slot, VkImage dest, uint32_t dstLayers, const BlitRegion* regions, size_t count);
    // The preview window, as MirrorWindow's DXGI swapchain on Windows: a Vulkan swapchain on the window's surface that the
    // frame loop copies the headset's left eye into on the GPU. Nothing waits for the window: a frame it cannot take
    // yet is skipped. Takes the surface (made from this instance); preview_detach destroys it before the window goes.
    bool preview_attach(VkSurfaceKHR surface);
    void preview_detach();
    // Frame loop thread. The top-left width x height of one layer of an acquired swapchain image (in
    // COLOR_ATTACHMENT_OPTIMAL, of `format`), or a region of a cache slot, scaled to fit a window of windowWidth x
    // windowHeight pixels and presented. False: skipped (the window had no image free); try again next frame.
    bool preview_present_image(VkImage image, VkFormat format, uint32_t layer, uint32_t width, uint32_t height,
                               uint32_t windowWidth, uint32_t windowHeight);
    bool preview_present_cached(int slot, const BlitRegion& region, uint32_t windowWidth, uint32_t windowHeight);

private:
    bool load_device_functions();
    bool ensure_staging(VkDeviceSize size);
    bool begin();
    bool submit();

    struct SharedPair {
        VkImage image[2]{};
        VkDeviceMemory memory[2]{};
        uint32_t width = 0, height = 0;
        std::chrono::steady_clock::time_point used{};
    };
    struct CacheSlot {
        VkImage image = VK_NULL_HANDLE;  // Two layers: the pair.
        VkDeviceMemory memory = VK_NULL_HANDLE;
        uint32_t width = 0, height = 0;
        VkFormat format = VK_FORMAT_UNDEFINED;
    };
    struct PreviewSource {
        VkImage image = VK_NULL_HANDLE;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;  // Held before and after the copy.
        VkFormat format = VK_FORMAT_UNDEFINED;
        uint32_t layer = 0;
        int32_t x = 0, y = 0;
        uint32_t width = 0, height = 0;
        bool flip = false;
    };
    bool preview_present(const PreviewSource& source, uint32_t windowWidth, uint32_t windowHeight);
    bool preview_create_swapchain(bool srgb, uint32_t windowWidth, uint32_t windowHeight);
    void preview_destroy_swapchain();
    SharedPair* open_shared(uint64_t session, uint32_t width, uint32_t height);
    void close_shared(SharedPair& pair);
    void destroy_cache(CacheSlot& slot);
    bool ensure_cache(CacheSlot& slot, uint32_t width, uint32_t height, VkFormat format);
    uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags wanted) const;
    bool begin_on(VkCommandBuffer commands);
    bool submit_on(VkQueue queue, VkCommandBuffer commands, VkFence fence);

    void* library_ = nullptr;
    PFN_vkGetInstanceProcAddr getInstanceProcAddr_ = nullptr;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = 0;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer commands_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    VkBuffer staging_ = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory_ = VK_NULL_HANDLE;
    VkDeviceSize stagingSize_ = 0;
    void* stagingMapped_ = nullptr;
    // GPU sharing (the image thread's own queue, pool, command buffer and fence; the maps are guarded by receiveMutex_).
    bool externalMemoryFd_ = false;
    VkQueue receiveQueue_ = VK_NULL_HANDLE;
    VkCommandPool receivePool_ = VK_NULL_HANDLE;
    VkCommandBuffer receiveCommands_ = VK_NULL_HANDLE;
    VkFence receiveFence_ = VK_NULL_HANDLE;
    std::mutex receiveMutex_;
    std::map<uint64_t, SharedPair> shared_;
    CacheSlot cache_[kSlots]{};
    // The preview window (frame loop thread). Each in-flight copy has its command buffer, fence and acquire semaphore;
    // each swapchain image the semaphore its copy signals for the present.
    bool swapchainExtension_ = false;
    VkSurfaceKHR previewSurface_ = VK_NULL_HANDLE;
    VkSwapchainKHR previewSwapchain_ = VK_NULL_HANDLE;
    std::vector<VkImage> previewImages_;
    std::vector<VkSemaphore> previewCopied_;
    struct PreviewFrame {
        VkCommandBuffer commands = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkSemaphore acquired = VK_NULL_HANDLE;
    };
    PreviewFrame previewFrames_[2]{};
    uint32_t previewFrame_ = 0;
    VkExtent2D previewExtent_{};
    VkFormat previewFormat_ = VK_FORMAT_UNDEFINED;
    bool previewSrgb_ = false;
    bool previewStale_ = true;  // Out of date or resized: made again before the next copy.
    XrGraphicsBindingVulkan2KHR binding_{XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};

    PFN_vkDestroyInstance destroyInstance_ = nullptr;
    PFN_vkGetPhysicalDeviceProperties getPhysicalDeviceProperties_ = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties getPhysicalDeviceMemoryProperties_ = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties getPhysicalDeviceQueueFamilyProperties_ = nullptr;
    PFN_vkGetDeviceProcAddr getDeviceProcAddr_ = nullptr;
    PFN_vkDestroyDevice destroyDevice_ = nullptr;
    PFN_vkDeviceWaitIdle deviceWaitIdle_ = nullptr;
    PFN_vkGetDeviceQueue getDeviceQueue_ = nullptr;
    PFN_vkCreateCommandPool createCommandPool_ = nullptr;
    PFN_vkDestroyCommandPool destroyCommandPool_ = nullptr;
    PFN_vkAllocateCommandBuffers allocateCommandBuffers_ = nullptr;
    PFN_vkBeginCommandBuffer beginCommandBuffer_ = nullptr;
    PFN_vkEndCommandBuffer endCommandBuffer_ = nullptr;
    PFN_vkResetCommandBuffer resetCommandBuffer_ = nullptr;
    PFN_vkCmdPipelineBarrier cmdPipelineBarrier_ = nullptr;
    PFN_vkCmdCopyBufferToImage cmdCopyBufferToImage_ = nullptr;
    PFN_vkCmdCopyImage cmdCopyImage_ = nullptr;
    PFN_vkQueueSubmit queueSubmit_ = nullptr;
    PFN_vkCreateFence createFence_ = nullptr;
    PFN_vkDestroyFence destroyFence_ = nullptr;
    PFN_vkWaitForFences waitForFences_ = nullptr;
    PFN_vkResetFences resetFences_ = nullptr;
    PFN_vkCreateBuffer createBuffer_ = nullptr;
    PFN_vkDestroyBuffer destroyBuffer_ = nullptr;
    PFN_vkGetBufferMemoryRequirements getBufferMemoryRequirements_ = nullptr;
    PFN_vkAllocateMemory allocateMemory_ = nullptr;
    PFN_vkFreeMemory freeMemory_ = nullptr;
    PFN_vkBindBufferMemory bindBufferMemory_ = nullptr;
    PFN_vkMapMemory mapMemory_ = nullptr;
    PFN_vkCreateImage createImage_ = nullptr;
    PFN_vkDestroyImage destroyImage_ = nullptr;
    PFN_vkGetImageMemoryRequirements getImageMemoryRequirements_ = nullptr;
    PFN_vkBindImageMemory bindImageMemory_ = nullptr;
    PFN_vkCmdBlitImage cmdBlitImage_ = nullptr;
    PFN_vkCmdCopyImageToBuffer cmdCopyImageToBuffer_ = nullptr;
    PFN_vkEnumerateDeviceExtensionProperties enumerateDeviceExtensionProperties_ = nullptr;
    PFN_vkDestroySurfaceKHR destroySurface_ = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR getSurfaceSupport_ = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR getSurfaceCapabilities_ = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR getSurfaceFormats_ = nullptr;
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR getSurfacePresentModes_ = nullptr;
    PFN_vkCreateSwapchainKHR createSwapchain_ = nullptr;
    PFN_vkDestroySwapchainKHR destroySwapchain_ = nullptr;
    PFN_vkGetSwapchainImagesKHR getSwapchainImages_ = nullptr;
    PFN_vkAcquireNextImageKHR acquireNextImage_ = nullptr;
    PFN_vkQueuePresentKHR queuePresent_ = nullptr;
    PFN_vkCreateSemaphore createSemaphore_ = nullptr;
    PFN_vkDestroySemaphore destroySemaphore_ = nullptr;
    PFN_vkCmdClearColorImage cmdClearColorImage_ = nullptr;
    PFN_vkQueueWaitIdle queueWaitIdle_ = nullptr;
};

} // namespace refract::host
#endif
