#include "vulkan_renderer.h"
#if !defined(_WIN32)

#include "linux_gpu_share.h"

#include <dlfcn.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <initializer_list>
#include <type_traits>
#include <vector>

#include <unistd.h>

namespace refract::host {

namespace {

constexpr VkImageLayout kAcquiredLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;  // OpenXR's Vulkan rule.

bool vk_ok(VkResult result, const char* what)
{
    if (result == VK_SUCCESS) return true;
    std::fprintf(stderr, "Refract Vulkan: %s failed: %d\n", what, static_cast<int>(result));
    return false;
}

bool xr_ok(XrResult result, const char* what)
{
    if (result == XR_SUCCESS) return true;
    std::fprintf(stderr, "Refract Vulkan: %s failed: %d\n", what, static_cast<int>(result));
    return false;
}

VkImageMemoryBarrier layout_barrier(VkImage image, uint32_t baseLayer, uint32_t layers, VkImageLayout from, VkImageLayout to,
                                    VkAccessFlags srcAccess, VkAccessFlags dstAccess)
{
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, baseLayer, layers};
    return barrier;
}

} // namespace

VulkanRenderer::~VulkanRenderer()
{
    if (device_ != VK_NULL_HANDLE) {
        deviceWaitIdle_(device_);
        for (auto& [id, pair] : shared_) close_shared(pair);
        for (auto& slot : cache_) destroy_cache(slot);
        preview_detach();
        if (receiveFence_ != VK_NULL_HANDLE) destroyFence_(device_, receiveFence_, nullptr);
        if (receivePool_ != VK_NULL_HANDLE) destroyCommandPool_(device_, receivePool_, nullptr);
        if (staging_ != VK_NULL_HANDLE) destroyBuffer_(device_, staging_, nullptr);
        if (stagingMemory_ != VK_NULL_HANDLE) freeMemory_(device_, stagingMemory_, nullptr);
        if (fence_ != VK_NULL_HANDLE) destroyFence_(device_, fence_, nullptr);
        if (pool_ != VK_NULL_HANDLE) destroyCommandPool_(device_, pool_, nullptr);
        destroyDevice_(device_, nullptr);
    }
    if (instance_ != VK_NULL_HANDLE && destroyInstance_ != nullptr) destroyInstance_(instance_, nullptr);
    if (library_ != nullptr) dlclose(library_);
}

bool VulkanRenderer::create(XrInstance instance, XrSystemId system, PFN_xrGetInstanceProcAddr getInstanceProcAddr)
{
    library_ = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (library_ == nullptr) {
        std::fprintf(stderr, "Refract Vulkan: cannot load libvulkan.so.1\n");
        return false;
    }
    getInstanceProcAddr_ = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(library_, "vkGetInstanceProcAddr"));
    if (getInstanceProcAddr_ == nullptr) return false;

    PFN_xrGetVulkanGraphicsRequirements2KHR getRequirements = nullptr;
    PFN_xrCreateVulkanInstanceKHR createInstance = nullptr;
    PFN_xrGetVulkanGraphicsDevice2KHR getDevice = nullptr;
    PFN_xrCreateVulkanDeviceKHR createDevice = nullptr;
    const auto xr_func = [&](const char* name, auto* out) {
        PFN_xrVoidFunction function = nullptr;
        if (getInstanceProcAddr(instance, name, &function) != XR_SUCCESS || function == nullptr) {
            std::fprintf(stderr, "Refract Vulkan: runtime lacks %s\n", name);
            return false;
        }
        *out = reinterpret_cast<std::remove_pointer_t<decltype(out)>>(function);
        return true;
    };
    if (!xr_func("xrGetVulkanGraphicsRequirements2KHR", &getRequirements) ||
        !xr_func("xrCreateVulkanInstanceKHR", &createInstance) ||
        !xr_func("xrGetVulkanGraphicsDevice2KHR", &getDevice) ||
        !xr_func("xrCreateVulkanDeviceKHR", &createDevice)) return false;

    XrGraphicsRequirementsVulkan2KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
    if (!xr_ok(getRequirements(instance, system, &requirements), "xrGetVulkanGraphicsRequirements2KHR")) return false;
    // Vulkan 1.1 covers everything used here; ask for more only if the runtime insists.
    const XrVersion minimum = requirements.minApiVersionSupported;
    const uint32_t apiVersion = (std::max)(VK_API_VERSION_1_1,
        VK_MAKE_API_VERSION(0, XR_VERSION_MAJOR(minimum), XR_VERSION_MINOR(minimum), 0));

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Refract host bridge";
    app.pEngineName = "Refract";
    app.apiVersion = apiVersion;
    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &app;
    // The preview window's surface: VK_KHR_surface and whichever window-system surfaces this loader offers (SDL picks
    // Wayland or X11 at run time).
    std::vector<const char*> instanceExtensions;
    if (const auto enumerateInstanceExtensions = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
            getInstanceProcAddr_(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties"))) {
        uint32_t count = 0;
        enumerateInstanceExtensions(nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> available(count);
        enumerateInstanceExtensions(nullptr, &count, available.data());
        const auto has = [&](const char* name) {
            return std::any_of(available.begin(), available.end(), [&](const VkExtensionProperties& e) { return !std::strcmp(e.extensionName, name); });
        };
        if (has("VK_KHR_surface")) {
            instanceExtensions.push_back("VK_KHR_surface");
            for (const char* name : {"VK_KHR_wayland_surface", "VK_KHR_xcb_surface", "VK_KHR_xlib_surface"})
                if (has(name)) instanceExtensions.push_back(name);
        }
    }
    instanceInfo.enabledExtensionCount = static_cast<uint32_t>(instanceExtensions.size());
    instanceInfo.ppEnabledExtensionNames = instanceExtensions.data();
    XrVulkanInstanceCreateInfoKHR xrInstanceInfo{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
    xrInstanceInfo.systemId = system;
    xrInstanceInfo.pfnGetInstanceProcAddr = getInstanceProcAddr_;
    xrInstanceInfo.vulkanCreateInfo = &instanceInfo;
    VkResult vkResult = VK_SUCCESS;
    if (!xr_ok(createInstance(instance, &xrInstanceInfo, &instance_, &vkResult), "xrCreateVulkanInstanceKHR") ||
        !vk_ok(vkResult, "vkCreateInstance")) return false;

    const auto vk_instance_func = [&](const char* name, auto* out) {
        *out = reinterpret_cast<std::remove_pointer_t<decltype(out)>>(getInstanceProcAddr_(instance_, name));
        if (*out == nullptr) std::fprintf(stderr, "Refract Vulkan: missing %s\n", name);
        return *out != nullptr;
    };
    if (!vk_instance_func("vkDestroyInstance", &destroyInstance_) ||
        !vk_instance_func("vkGetPhysicalDeviceProperties", &getPhysicalDeviceProperties_) ||
        !vk_instance_func("vkGetPhysicalDeviceMemoryProperties", &getPhysicalDeviceMemoryProperties_) ||
        !vk_instance_func("vkGetPhysicalDeviceQueueFamilyProperties", &getPhysicalDeviceQueueFamilyProperties_) ||
        !vk_instance_func("vkGetDeviceProcAddr", &getDeviceProcAddr_) ||
        !vk_instance_func("vkEnumerateDeviceExtensionProperties", &enumerateDeviceExtensionProperties_)) return false;
    if (!instanceExtensions.empty()) {
        // Optional: without them there is no preview window.
        destroySurface_ = reinterpret_cast<PFN_vkDestroySurfaceKHR>(getInstanceProcAddr_(instance_, "vkDestroySurfaceKHR"));
        getSurfaceSupport_ = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceSupportKHR>(getInstanceProcAddr_(instance_, "vkGetPhysicalDeviceSurfaceSupportKHR"));
        getSurfaceCapabilities_ = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(
            getInstanceProcAddr_(instance_, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"));
        getSurfaceFormats_ = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>(getInstanceProcAddr_(instance_, "vkGetPhysicalDeviceSurfaceFormatsKHR"));
        getSurfacePresentModes_ = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfacePresentModesKHR>(
            getInstanceProcAddr_(instance_, "vkGetPhysicalDeviceSurfacePresentModesKHR"));
    }

    // The runtime picks the GPU the headset is on (with two GPUs, not necessarily the first).
    XrVulkanGraphicsDeviceGetInfoKHR deviceGetInfo{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
    deviceGetInfo.systemId = system;
    deviceGetInfo.vulkanInstance = instance_;
    if (!xr_ok(getDevice(instance, &deviceGetInfo, &physical_), "xrGetVulkanGraphicsDevice2KHR")) return false;

    uint32_t familyCount = 0;
    getPhysicalDeviceQueueFamilyProperties_(physical_, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    getPhysicalDeviceQueueFamilyProperties_(physical_, &familyCount, families.data());
    queueFamily_ = UINT32_MAX;
    for (uint32_t i = 0; i < familyCount && queueFamily_ == UINT32_MAX; ++i)
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) queueFamily_ = i;
    if (queueFamily_ == UINT32_MAX) {
        std::fprintf(stderr, "Refract Vulkan: the runtime's GPU has no graphics queue\n");
        return false;
    }

    // GPU sharing needs the external-memory file descriptor extensions and a second queue for the image thread (the
    // runtime owns queue 0's use on the frame loop's thread). Without either the bridge takes pixels over TCP.
    std::vector<const char*> deviceExtensions;
    {
        uint32_t count = 0;
        enumerateDeviceExtensionProperties_(physical_, nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> available(count);
        enumerateDeviceExtensionProperties_(physical_, nullptr, &count, available.data());
        const auto has = [&](const char* name) {
            return std::any_of(available.begin(), available.end(), [&](const VkExtensionProperties& e) { return !std::strcmp(e.extensionName, name); });
        };
        if (has(VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME) && has(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) && families[queueFamily_].queueCount >= 2) {
            deviceExtensions = {VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME};
            externalMemoryFd_ = true;
        }
        if (destroySurface_ && has(VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
            deviceExtensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
            swapchainExtension_ = true;
        }
    }
    const float priorities[2] = {1.0f, 1.0f};
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = queueFamily_;
    queueInfo.queueCount = externalMemoryFd_ ? 2 : 1;
    queueInfo.pQueuePriorities = priorities;
    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
    deviceInfo.ppEnabledExtensionNames = deviceExtensions.data();
    XrVulkanDeviceCreateInfoKHR xrDeviceInfo{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
    xrDeviceInfo.systemId = system;
    xrDeviceInfo.pfnGetInstanceProcAddr = getInstanceProcAddr_;
    xrDeviceInfo.vulkanPhysicalDevice = physical_;
    xrDeviceInfo.vulkanCreateInfo = &deviceInfo;
    if (!xr_ok(createDevice(instance, &xrDeviceInfo, &device_, &vkResult), "xrCreateVulkanDeviceKHR") ||
        !vk_ok(vkResult, "vkCreateDevice")) {
        device_ = VK_NULL_HANDLE;
        return false;
    }
    if (!load_device_functions()) {
        // Without them the destructor could not release the device either.
        device_ = VK_NULL_HANDLE;
        return false;
    }
    getDeviceQueue_(device_, queueFamily_, 0, &queue_);
    if (swapchainExtension_) {
        const auto optional = [&](const char* name, auto* out) {
            *out = reinterpret_cast<std::remove_pointer_t<decltype(out)>>(getDeviceProcAddr_(device_, name));
            return *out != nullptr;
        };
        swapchainExtension_ = optional("vkCreateSwapchainKHR", &createSwapchain_) && optional("vkDestroySwapchainKHR", &destroySwapchain_) &&
            optional("vkGetSwapchainImagesKHR", &getSwapchainImages_) && optional("vkAcquireNextImageKHR", &acquireNextImage_) &&
            optional("vkQueuePresentKHR", &queuePresent_) && optional("vkCreateSemaphore", &createSemaphore_) &&
            optional("vkDestroySemaphore", &destroySemaphore_) && optional("vkCmdClearColorImage", &cmdClearColorImage_) &&
            optional("vkQueueWaitIdle", &queueWaitIdle_);
    }

    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = queueFamily_;
    if (!vk_ok(createCommandPool_(device_, &poolInfo, nullptr, &pool_), "vkCreateCommandPool")) return false;
    VkCommandBufferAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocateInfo.commandPool = pool_;
    allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocateInfo.commandBufferCount = 1;
    if (!vk_ok(allocateCommandBuffers_(device_, &allocateInfo, &commands_), "vkAllocateCommandBuffers")) return false;
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (!vk_ok(createFence_(device_, &fenceInfo, nullptr, &fence_), "vkCreateFence")) return false;
    if (externalMemoryFd_) {
        getDeviceQueue_(device_, queueFamily_, 1, &receiveQueue_);
        if (!vk_ok(createCommandPool_(device_, &poolInfo, nullptr, &receivePool_), "vkCreateCommandPool") ||
            !vk_ok(createFence_(device_, &fenceInfo, nullptr, &receiveFence_), "vkCreateFence")) return false;
        allocateInfo.commandPool = receivePool_;
        if (!vk_ok(allocateCommandBuffers_(device_, &allocateInfo, &receiveCommands_), "vkAllocateCommandBuffers")) return false;
    }

    binding_.instance = instance_;
    binding_.physicalDevice = physical_;
    binding_.device = device_;
    binding_.queueFamilyIndex = queueFamily_;
    binding_.queueIndex = 0;

    VkPhysicalDeviceProperties properties{};
    getPhysicalDeviceProperties_(physical_, &properties);
    std::fprintf(stderr, "Refract Vulkan: graphics binding ready on %s (Vulkan %u.%u, GPU sharing %s)\n", properties.deviceName,
        VK_API_VERSION_MAJOR(apiVersion), VK_API_VERSION_MINOR(apiVersion), externalMemoryFd_ ? "available" : "unavailable");
    return true;
}

bool VulkanRenderer::load_device_functions()
{
    const auto func = [&](const char* name, auto* out) {
        *out = reinterpret_cast<std::remove_pointer_t<decltype(out)>>(getDeviceProcAddr_(device_, name));
        if (*out == nullptr) std::fprintf(stderr, "Refract Vulkan: missing %s\n", name);
        return *out != nullptr;
    };
    return func("vkDestroyDevice", &destroyDevice_) && func("vkDeviceWaitIdle", &deviceWaitIdle_) &&
        func("vkGetDeviceQueue", &getDeviceQueue_) && func("vkCreateCommandPool", &createCommandPool_) &&
        func("vkDestroyCommandPool", &destroyCommandPool_) && func("vkAllocateCommandBuffers", &allocateCommandBuffers_) &&
        func("vkBeginCommandBuffer", &beginCommandBuffer_) && func("vkEndCommandBuffer", &endCommandBuffer_) &&
        func("vkResetCommandBuffer", &resetCommandBuffer_) && func("vkCmdPipelineBarrier", &cmdPipelineBarrier_) &&
        func("vkCmdCopyBufferToImage", &cmdCopyBufferToImage_) && func("vkCmdCopyImage", &cmdCopyImage_) &&
        func("vkQueueSubmit", &queueSubmit_) && func("vkCreateFence", &createFence_) &&
        func("vkDestroyFence", &destroyFence_) && func("vkWaitForFences", &waitForFences_) &&
        func("vkResetFences", &resetFences_) && func("vkCreateBuffer", &createBuffer_) &&
        func("vkDestroyBuffer", &destroyBuffer_) && func("vkGetBufferMemoryRequirements", &getBufferMemoryRequirements_) &&
        func("vkAllocateMemory", &allocateMemory_) && func("vkFreeMemory", &freeMemory_) &&
        func("vkBindBufferMemory", &bindBufferMemory_) && func("vkMapMemory", &mapMemory_) &&
        func("vkCreateImage", &createImage_) && func("vkDestroyImage", &destroyImage_) &&
        func("vkGetImageMemoryRequirements", &getImageMemoryRequirements_) && func("vkBindImageMemory", &bindImageMemory_) &&
        func("vkCmdBlitImage", &cmdBlitImage_) && func("vkCmdCopyImageToBuffer", &cmdCopyImageToBuffer_);
}

// One host-visible buffer, grown to the largest frame seen. Every copy waits for its fence, so the next
// frame can overwrite it.
bool VulkanRenderer::ensure_staging(VkDeviceSize size)
{
    if (size <= stagingSize_) return true;
    if (staging_ != VK_NULL_HANDLE) destroyBuffer_(device_, staging_, nullptr);
    if (stagingMemory_ != VK_NULL_HANDLE) freeMemory_(device_, stagingMemory_, nullptr);
    staging_ = VK_NULL_HANDLE;
    stagingMemory_ = VK_NULL_HANDLE;
    stagingSize_ = 0;
    stagingMapped_ = nullptr;

    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = size;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!vk_ok(createBuffer_(device_, &bufferInfo, nullptr, &staging_), "vkCreateBuffer")) return false;
    VkMemoryRequirements requirements{};
    getBufferMemoryRequirements_(device_, staging_, &requirements);
    VkPhysicalDeviceMemoryProperties memory{};
    getPhysicalDeviceMemoryProperties_(physical_, &memory);
    constexpr VkMemoryPropertyFlags kWanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < memory.memoryTypeCount && type == UINT32_MAX; ++i)
        if ((requirements.memoryTypeBits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & kWanted) == kWanted) type = i;
    if (type == UINT32_MAX) {
        std::fprintf(stderr, "Refract Vulkan: no host-visible memory for frame uploads\n");
        return false;
    }
    VkMemoryAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocateInfo.allocationSize = requirements.size;
    allocateInfo.memoryTypeIndex = type;
    if (!vk_ok(allocateMemory_(device_, &allocateInfo, nullptr, &stagingMemory_), "vkAllocateMemory") ||
        !vk_ok(bindBufferMemory_(device_, staging_, stagingMemory_, 0), "vkBindBufferMemory") ||
        !vk_ok(mapMemory_(device_, stagingMemory_, 0, VK_WHOLE_SIZE, 0, &stagingMapped_), "vkMapMemory")) return false;
    stagingSize_ = size;
    return true;
}

bool VulkanRenderer::begin()
{
    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    return vk_ok(resetCommandBuffer_(commands_, 0), "vkResetCommandBuffer") &&
        vk_ok(beginCommandBuffer_(commands_, &beginInfo), "vkBeginCommandBuffer");
}

// Waits for the copy: the staging buffer is reused, and the caller releases the image to the runtime next.
bool VulkanRenderer::submit()
{
    if (!vk_ok(endCommandBuffer_(commands_), "vkEndCommandBuffer")) return false;
    VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commands_;
    if (!vk_ok(resetFences_(device_, 1, &fence_), "vkResetFences") ||
        !vk_ok(queueSubmit_(queue_, 1, &submitInfo, fence_), "vkQueueSubmit")) return false;
    return vk_ok(waitForFences_(device_, 1, &fence_, VK_TRUE, UINT64_MAX), "vkWaitForFences");
}

bool VulkanRenderer::upload(VkImage image, const uint8_t* pixels, uint32_t width, uint32_t height, uint32_t layers)
{
    if (device_ == VK_NULL_HANDLE || image == VK_NULL_HANDLE || !width || !height || !layers) return false;
    const VkDeviceSize layerBytes = static_cast<VkDeviceSize>(width) * height * 4;
    if (!ensure_staging(layerBytes * layers)) return false;
    std::memcpy(stagingMapped_, pixels, static_cast<size_t>(layerBytes * layers));
    if (!begin()) return false;

    VkImageMemoryBarrier toTransfer = layout_barrier(image, 0, layers, kAcquiredLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    cmdPipelineBarrier_(commands_, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
        0, nullptr, 0, nullptr, 1, &toTransfer);
    std::vector<VkBufferImageCopy> regions(layers);
    for (uint32_t layer = 0; layer < layers; ++layer) {
        auto& region = regions[layer];
        region.bufferOffset = layerBytes * layer;
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1};
        region.imageExtent = {width, height, 1};
    }
    cmdCopyBufferToImage_(commands_, staging_, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, layers, regions.data());
    VkImageMemoryBarrier back = layout_barrier(image, 0, layers, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, kAcquiredLayout,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    cmdPipelineBarrier_(commands_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
        0, nullptr, 0, nullptr, 1, &back);
    return submit();
}

bool VulkanRenderer::copy(VkImage source, uint32_t sourceLayer, VkImage dest, uint32_t width, uint32_t height)
{
    if (device_ == VK_NULL_HANDLE || source == VK_NULL_HANDLE || dest == VK_NULL_HANDLE || !width || !height) return false;
    if (!begin()) return false;
    const VkImageMemoryBarrier toTransfer[] = {
        layout_barrier(source, sourceLayer, 1, kAcquiredLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT),
        layout_barrier(dest, 0, 1, kAcquiredLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT),
    };
    cmdPipelineBarrier_(commands_, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, toTransfer);
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, sourceLayer, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {width, height, 1};
    cmdCopyImage_(commands_, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dest, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    const VkImageMemoryBarrier back[] = {
        layout_barrier(source, sourceLayer, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, kAcquiredLayout,
            VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT),
        layout_barrier(dest, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, kAcquiredLayout,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT),
    };
    cmdPipelineBarrier_(commands_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
        0, nullptr, 0, nullptr, 2, back);
    return submit();
}

uint32_t VulkanRenderer::memory_type(uint32_t bits, VkMemoryPropertyFlags wanted) const
{
    VkPhysicalDeviceMemoryProperties memory{};
    getPhysicalDeviceMemoryProperties_(physical_, &memory);
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & wanted) == wanted) return i;
    return UINT32_MAX;
}

bool VulkanRenderer::begin_on(VkCommandBuffer commands)
{
    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    return vk_ok(resetCommandBuffer_(commands, 0), "vkResetCommandBuffer") &&
        vk_ok(beginCommandBuffer_(commands, &beginInfo), "vkBeginCommandBuffer");
}

bool VulkanRenderer::submit_on(VkQueue queue, VkCommandBuffer commands, VkFence fence)
{
    if (!vk_ok(endCommandBuffer_(commands), "vkEndCommandBuffer")) return false;
    VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commands;
    if (!vk_ok(resetFences_(device_, 1, &fence), "vkResetFences") ||
        !vk_ok(queueSubmit_(queue, 1, &submitInfo, fence), "vkQueueSubmit")) return false;
    return vk_ok(waitForFences_(device_, 1, &fence, VK_TRUE, 2500000000ull), "vkWaitForFences");
}

void VulkanRenderer::close_shared(SharedPair& pair)
{
    for (int eye = 0; eye < 2; ++eye) {
        if (pair.image[eye] != VK_NULL_HANDLE) destroyImage_(device_, pair.image[eye], nullptr);
        if (pair.memory[eye] != VK_NULL_HANDLE) freeMemory_(device_, pair.memory[eye], nullptr);
        pair.image[eye] = VK_NULL_HANDLE;
        pair.memory[eye] = VK_NULL_HANDLE;
    }
}

void VulkanRenderer::destroy_cache(CacheSlot& slot)
{
    if (slot.image != VK_NULL_HANDLE) destroyImage_(device_, slot.image, nullptr);
    if (slot.memory != VK_NULL_HANDLE) freeMemory_(device_, slot.memory, nullptr);
    slot = {};
}

// Imports both eye images of one session of Refract's GPU layer (see protocol/linux_gpu_share.h). Each id is one
// slot of the Android runtime's export ring; the layer frees a pair after 10 s unused and may later make a new one
// under the same id, so pairs unused for 5 s are dropped here and opened again when needed. Image thread.
VulkanRenderer::SharedPair* VulkanRenderer::open_shared(uint64_t session, uint32_t width, uint32_t height)
{
    const auto now = std::chrono::steady_clock::now();
    for (auto it = shared_.begin(); it != shared_.end();) {
        if (it->first != session && now - it->second.used > std::chrono::seconds(5)) {
            close_shared(it->second);
            it = shared_.erase(it);
        } else ++it;
    }
    auto found = shared_.find(session);
    if (found != shared_.end()) { found->second.used = now; return &found->second; }

    SharedPair pair;
    pair.used = now;
    for (uint32_t eye = 0; eye < 2; ++eye) {
        protocol::LinuxGpuShareReply reply;
        const int fd = protocol::request_linux_gpu_share(session, eye, reply);
        if (fd < 0) {
            std::fprintf(stderr, "Refract Vulkan: the GPU layer did not share texture %016llx/%u (status %d); is the Venus renderer "
                "running with it (scripts/waydroid.sh setup)?\n", static_cast<unsigned long long>(session), eye, reply.status);
            close_shared(pair);
            return nullptr;
        }
        if (reply.width != width || reply.height != height || (reply.format != VK_FORMAT_R8G8B8A8_UNORM && reply.format != VK_FORMAT_R8G8B8A8_SRGB)) {
            std::fprintf(stderr, "Refract Vulkan: shared texture is %ux%u format %u, expected %ux%u RGBA8\n", reply.width, reply.height,
                reply.format, width, height);
            close(fd);
            close_shared(pair);
            return nullptr;
        }
        VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
        external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imageInfo.pNext = &external;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = static_cast<VkFormat>(reply.format);
        imageInfo.extent = {reply.width, reply.height, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (!vk_ok(createImage_(device_, &imageInfo, nullptr, &pair.image[eye]), "vkCreateImage (shared)")) {
            close(fd);
            close_shared(pair);
            return nullptr;
        }
        VkMemoryRequirements requirements{};
        getImageMemoryRequirements_(device_, pair.image[eye], &requirements);
        uint32_t type = memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (type == UINT32_MAX) type = memory_type(requirements.memoryTypeBits, 0);
        VkImportMemoryFdInfoKHR import{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
        import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        import.fd = fd;  // Vulkan owns the descriptor once the import succeeds.
        VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        dedicated.pNext = &import;
        dedicated.image = pair.image[eye];
        VkMemoryAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocateInfo.pNext = &dedicated;
        allocateInfo.allocationSize = (std::max)(static_cast<VkDeviceSize>(reply.size), requirements.size);
        allocateInfo.memoryTypeIndex = type;
        if (type == UINT32_MAX || !vk_ok(allocateMemory_(device_, &allocateInfo, nullptr, &pair.memory[eye]), "vkAllocateMemory (import)") ||
            !vk_ok(bindImageMemory_(device_, pair.image[eye], pair.memory[eye], 0), "vkBindImageMemory (import)")) {
            if (pair.memory[eye] == VK_NULL_HANDLE) close(fd);
            close_shared(pair);
            std::fprintf(stderr, "Refract Vulkan: cannot import the GPU layer's texture (is the Venus renderer on this GPU?)\n");
            return nullptr;
        }
    }
    pair.width = width;
    pair.height = height;
    if (shared_.size() >= 32) {
        for (auto& [id, old] : shared_) close_shared(old);
        shared_.clear();
    }
    std::fprintf(stderr, "Refract Vulkan: imported shared textures for session %016llx (%ux%u)\n",
                 static_cast<unsigned long long>(session), width, height);
    return &shared_.emplace(session, pair).first->second;
}

// A slot is one 2-layer image the size of the pair, in RGBA8 with the swapchain's sRGB-ness.
bool VulkanRenderer::ensure_cache(CacheSlot& slot, uint32_t width, uint32_t height, VkFormat format)
{
    if (slot.image != VK_NULL_HANDLE && slot.width == width && slot.height == height && slot.format == format) return true;
    destroy_cache(slot);
    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = {width, height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 2;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!vk_ok(createImage_(device_, &imageInfo, nullptr, &slot.image), "vkCreateImage (cache)")) { slot = {}; return false; }
    VkMemoryRequirements requirements{};
    getImageMemoryRequirements_(device_, slot.image, &requirements);
    uint32_t type = memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) type = memory_type(requirements.memoryTypeBits, 0);
    VkMemoryAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocateInfo.allocationSize = requirements.size;
    allocateInfo.memoryTypeIndex = type;
    if (type == UINT32_MAX || !vk_ok(allocateMemory_(device_, &allocateInfo, nullptr, &slot.memory), "vkAllocateMemory (cache)") ||
        !vk_ok(bindImageMemory_(device_, slot.image, slot.memory, 0), "vkBindImageMemory (cache)")) {
        destroy_cache(slot);
        return false;
    }
    slot.width = width;
    slot.height = height;
    slot.format = format;
    return true;
}

bool VulkanRenderer::receive_gpu_pair(uint64_t session, uint32_t width, uint32_t height, VkFormat targetFormat, int slotIndex)
{
    if (!gpu_share_ready() || slotIndex < 0 || slotIndex >= kSlots || !width || !height) return false;
    const bool srgb = targetFormat == VK_FORMAT_R8G8B8A8_SRGB || targetFormat == VK_FORMAT_B8G8R8A8_SRGB;
    const VkFormat format = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    std::lock_guard<std::mutex> lock(receiveMutex_);
    SharedPair* pair = open_shared(session, width, height);
    if (!pair) return false;
    // The frame loop reads a slot only while it is on screen, and free_slot never hands out that one, so a slot being
    // resized here is not in use. Its blits also wait for their fence before returning, so none is in flight. (A
    // vkDeviceWaitIdle here would race the frame loop's and the runtime's use of queue 0, which Vulkan forbids.)
    CacheSlot& slot = cache_[slotIndex];
    if (!ensure_cache(slot, width, height, format)) return false;

    if (!begin_on(receiveCommands_)) return false;
    // The shared images keep what the game's frame left in them: UNDEFINED here only gives the layout a name.
    VkImageMemoryBarrier toTransfer[3] = {
        layout_barrier(pair->image[0], 0, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, VK_ACCESS_TRANSFER_READ_BIT),
        layout_barrier(pair->image[1], 0, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, VK_ACCESS_TRANSFER_READ_BIT),
        layout_barrier(slot.image, 0, 2, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT),
    };
    cmdPipelineBarrier_(receiveCommands_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 3, toTransfer);
    for (uint32_t eye = 0; eye < 2; ++eye) {
        VkImageCopy region{};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, eye, 1};
        region.extent = {width, height, 1};
        cmdCopyImage_(receiveCommands_, pair->image[eye], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, slot.image,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    }
    VkImageMemoryBarrier done = layout_barrier(slot.image, 0, 2, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    cmdPipelineBarrier_(receiveCommands_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &done);
    return submit_on(receiveQueue_, receiveCommands_, receiveFence_);
}

bool VulkanRenderer::blit_cached(int slotIndex, VkImage dest, uint32_t dstLayers, const BlitRegion* regions, size_t count)
{
    if (device_ == VK_NULL_HANDLE || dest == VK_NULL_HANDLE || slotIndex < 0 || slotIndex >= kSlots || !dstLayers || !count) return false;
    const CacheSlot& slot = cache_[slotIndex];
    if (slot.image == VK_NULL_HANDLE) return false;
    for (size_t i = 0; i < count; ++i) {
        const BlitRegion& r = regions[i];
        if (!r.width || !r.height || r.srcLayer >= 2 || r.dstLayer >= dstLayers || r.srcX < 0 || r.srcY < 0 ||
            uint64_t(r.srcX) + r.width > slot.width || uint64_t(r.srcY) + r.height > slot.height) return false;
    }
    if (!begin()) return false;
    VkImageMemoryBarrier toTransfer = layout_barrier(dest, 0, dstLayers, kAcquiredLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    cmdPipelineBarrier_(commands_, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
        0, nullptr, 0, nullptr, 1, &toTransfer);
    std::vector<VkImageBlit> blits(count);
    for (size_t i = 0; i < count; ++i) {
        const BlitRegion& r = regions[i];
        VkImageBlit& b = blits[i];
        b.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, r.srcLayer, 1};
        b.srcOffsets[0] = {r.srcX, r.srcY, 0};
        b.srcOffsets[1] = {r.srcX + int32_t(r.width), r.srcY + int32_t(r.height), 1};
        b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, r.dstLayer, 1};
        // Reversed y offsets mirror the rows.
        b.dstOffsets[0] = {r.dstX, r.flip ? r.dstY + int32_t(r.height) : r.dstY, 0};
        b.dstOffsets[1] = {r.dstX + int32_t(r.width), r.flip ? r.dstY : r.dstY + int32_t(r.height), 1};
    }
    cmdBlitImage_(commands_, slot.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dest, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  static_cast<uint32_t>(count), blits.data(), VK_FILTER_NEAREST);
    VkImageMemoryBarrier back = layout_barrier(dest, 0, dstLayers, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, kAcquiredLayout,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    cmdPipelineBarrier_(commands_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
        0, nullptr, 0, nullptr, 1, &back);
    return submit();
}

namespace {
bool is_srgb(VkFormat format)
{
    return format == VK_FORMAT_R8G8B8A8_SRGB || format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_A8B8G8R8_SRGB_PACK32;
}
} // namespace

bool VulkanRenderer::preview_attach(VkSurfaceKHR surface)
{
    if (surface == VK_NULL_HANDLE) return false;
    previewSurface_ = surface;
    VkBool32 supported = VK_FALSE;
    if (!swapchainExtension_ || device_ == VK_NULL_HANDLE ||
        getSurfaceSupport_(physical_, queueFamily_, surface, &supported) != VK_SUCCESS || !supported) {
        std::fprintf(stderr, "Refract Vulkan: the headset's GPU queue cannot present to the preview window\n");
        preview_detach();
        return false;
    }
    VkCommandBuffer commands[2]{};
    VkCommandBufferAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocateInfo.commandPool = pool_;
    allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocateInfo.commandBufferCount = 2;
    if (!vk_ok(allocateCommandBuffers_(device_, &allocateInfo, commands), "vkAllocateCommandBuffers (preview)")) {
        preview_detach();
        return false;
    }
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;  // Free for the first copy.
    VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (int i = 0; i < 2; ++i) {
        previewFrames_[i].commands = commands[i];
        if (!vk_ok(createFence_(device_, &fenceInfo, nullptr, &previewFrames_[i].fence), "vkCreateFence (preview)") ||
            !vk_ok(createSemaphore_(device_, &semaphoreInfo, nullptr, &previewFrames_[i].acquired), "vkCreateSemaphore (preview)")) {
            preview_detach();
            return false;
        }
    }
    previewStale_ = true;
    return true;
}

void VulkanRenderer::preview_detach()
{
    if (device_ == VK_NULL_HANDLE) return;
    preview_destroy_swapchain();
    for (auto& frame : previewFrames_) {
        if (frame.fence != VK_NULL_HANDLE) destroyFence_(device_, frame.fence, nullptr);
        if (frame.acquired != VK_NULL_HANDLE) destroySemaphore_(device_, frame.acquired, nullptr);
        frame = {};  // The command buffers go with the pool.
    }
    if (previewSurface_ != VK_NULL_HANDLE) destroySurface_(instance_, previewSurface_, nullptr);
    previewSurface_ = VK_NULL_HANDLE;
}

// After every copy and present that used them: queue 0 is this thread's, so waiting for it is enough.
void VulkanRenderer::preview_destroy_swapchain()
{
    if (previewSwapchain_ == VK_NULL_HANDLE) return;
    queueWaitIdle_(queue_);
    for (VkSemaphore semaphore : previewCopied_) destroySemaphore_(device_, semaphore, nullptr);
    previewCopied_.clear();
    previewImages_.clear();
    destroySwapchain_(device_, previewSwapchain_, nullptr);
    previewSwapchain_ = VK_NULL_HANDLE;
}

// The window's swapchain, in the copy's sRGB-ness so the blit keeps the bytes the headset gets (as the Windows mirror
// copies them unconverted). Mailbox or immediate presents, so a covered window never holds up the frame loop.
bool VulkanRenderer::preview_create_swapchain(bool srgb, uint32_t windowWidth, uint32_t windowHeight)
{
    VkSurfaceCapabilitiesKHR capabilities{};
    if (!vk_ok(getSurfaceCapabilities_(physical_, previewSurface_, &capabilities), "vkGetPhysicalDeviceSurfaceCapabilitiesKHR")) return false;
    VkExtent2D extent = capabilities.currentExtent;
    if (extent.width == UINT32_MAX) {  // Wayland: the window's size is the swapchain's.
        extent.width = std::clamp(windowWidth, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
        extent.height = std::clamp(windowHeight, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
    }
    if (!extent.width || !extent.height) return false;  // Minimized.
    if (!(capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        std::fprintf(stderr, "Refract Vulkan: the preview window's images cannot be copied into\n");
        return false;
    }

    uint32_t count = 0;
    getSurfaceFormats_(physical_, previewSurface_, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(count);
    getSurfaceFormats_(physical_, previewSurface_, &count, formats.data());
    if (formats.empty()) return false;
    VkSurfaceFormatKHR chosen = formats[0];
    bool found = false;
    for (VkFormat wanted : srgb ? std::initializer_list<VkFormat>{VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB}
                                : std::initializer_list<VkFormat>{VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM}) {
        for (const auto& format : formats)
            if (!found && format.format == wanted && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                chosen = format;
                found = true;
            }
    }
    getSurfacePresentModes_(physical_, previewSurface_, &count, nullptr);
    std::vector<VkPresentModeKHR> modes(count);
    getSurfacePresentModes_(physical_, previewSurface_, &count, modes.data());
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    for (VkPresentModeKHR wanted : {VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_MAILBOX_KHR})
        if (std::find(modes.begin(), modes.end(), wanted) != modes.end()) mode = wanted;

    VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = previewSurface_;
    info.minImageCount = (std::max)(capabilities.minImageCount + 1, 3u);
    if (capabilities.maxImageCount) info.minImageCount = (std::min)(info.minImageCount, capabilities.maxImageCount);
    info.imageFormat = chosen.format;
    info.imageColorSpace = chosen.colorSpace;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = capabilities.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(capabilities.supportedCompositeAlpha & info.compositeAlpha))
        info.compositeAlpha = static_cast<VkCompositeAlphaFlagBitsKHR>(capabilities.supportedCompositeAlpha & ~(capabilities.supportedCompositeAlpha - 1));
    info.presentMode = mode;
    info.clipped = VK_TRUE;
    if (!vk_ok(createSwapchain_(device_, &info, nullptr, &previewSwapchain_), "vkCreateSwapchainKHR (preview)")) {
        previewSwapchain_ = VK_NULL_HANDLE;
        return false;
    }
    getSwapchainImages_(device_, previewSwapchain_, &count, nullptr);
    previewImages_.resize(count);
    getSwapchainImages_(device_, previewSwapchain_, &count, previewImages_.data());
    VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    previewCopied_.assign(count, VK_NULL_HANDLE);
    for (auto& semaphore : previewCopied_)
        if (!vk_ok(createSemaphore_(device_, &semaphoreInfo, nullptr, &semaphore), "vkCreateSemaphore (preview)")) {
            preview_destroy_swapchain();
            return false;
        }
    if (previewFormat_ != chosen.format || previewExtent_.width != extent.width || previewExtent_.height != extent.height)
        std::fprintf(stderr, "Refract Vulkan: preview window %ux%u format=%d %s\n", extent.width, extent.height, static_cast<int>(chosen.format),
            mode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "immediate" : mode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : "fifo");
    previewExtent_ = extent;
    previewFormat_ = chosen.format;
    previewSrgb_ = srgb;
    return true;
}

bool VulkanRenderer::preview_present(const PreviewSource& source, uint32_t windowWidth, uint32_t windowHeight)
{
    if (previewSurface_ == VK_NULL_HANDLE || source.image == VK_NULL_HANDLE || !source.width || !source.height) return false;
    const bool srgb = is_srgb(source.format);
    if (windowWidth && windowHeight && (windowWidth != previewExtent_.width || windowHeight != previewExtent_.height)) previewStale_ = true;
    if (previewStale_ || srgb != previewSrgb_ || previewSwapchain_ == VK_NULL_HANDLE) {
        preview_destroy_swapchain();
        if (!preview_create_swapchain(srgb, windowWidth, windowHeight)) return false;
        previewStale_ = false;
    }
    PreviewFrame& frame = previewFrames_[previewFrame_];
    if (waitForFences_(device_, 1, &frame.fence, VK_TRUE, 0) != VK_SUCCESS) return false;  // Its last copy is still running.
    uint32_t index = 0;
    const VkResult acquired = acquireNextImage_(device_, previewSwapchain_, 0, frame.acquired, VK_NULL_HANDLE, &index);
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) previewStale_ = true;
    if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) return false;  // No free image yet (or out of date).
    if (acquired == VK_SUBOPTIMAL_KHR) previewStale_ = true;
    const VkImage target = previewImages_[index];

    if (!begin_on(frame.commands)) return false;
    VkImageMemoryBarrier before[2] = {
        layout_barrier(target, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT),
        layout_barrier(source.image, source.layer, 1, source.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT)};
    const bool moveSource = source.layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    cmdPipelineBarrier_(frame.commands, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
        0, nullptr, 0, nullptr, moveSource ? 2 : 1, before);
    const VkClearColorValue black{{0.0f, 0.0f, 0.0f, 1.0f}};
    const VkImageSubresourceRange whole{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    cmdClearColorImage_(frame.commands, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &whole);
    VkImageMemoryBarrier cleared = layout_barrier(target, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    cmdPipelineBarrier_(frame.commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &cleared);
    // Fitted to the window, centred, keeping the eye's aspect ratio (DXGI_SCALING_STRETCH into a fitted child on Windows).
    const double scale = (std::min)(double(previewExtent_.width) / source.width, double(previewExtent_.height) / source.height);
    const int32_t width = (std::max)(1, int32_t(source.width * scale)), height = (std::max)(1, int32_t(source.height * scale));
    const int32_t x = (int32_t(previewExtent_.width) - width) / 2, y = (int32_t(previewExtent_.height) - height) / 2;
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, source.layer, 1};
    blit.srcOffsets[0] = {source.x, source.y, 0};
    blit.srcOffsets[1] = {source.x + int32_t(source.width), source.y + int32_t(source.height), 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[0] = {x, source.flip ? y + height : y, 0};
    blit.dstOffsets[1] = {x + width, source.flip ? y : y + height, 1};
    cmdBlitImage_(frame.commands, source.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    VkImageMemoryBarrier after[2] = {
        layout_barrier(target, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0),
        layout_barrier(source.image, source.layer, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, source.layout,
            VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT)};
    cmdPipelineBarrier_(frame.commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, moveSource ? 2 : 1, after);
    if (!vk_ok(endCommandBuffer_(frame.commands), "vkEndCommandBuffer (preview)")) return false;

    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = &frame.acquired;
    submitInfo.pWaitDstStageMask = &waitStage;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &frame.commands;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &previewCopied_[index];
    if (!vk_ok(resetFences_(device_, 1, &frame.fence), "vkResetFences (preview)") ||
        !vk_ok(queueSubmit_(queue_, 1, &submitInfo, frame.fence), "vkQueueSubmit (preview)")) {
        previewStale_ = true;  // The acquire semaphore may still be pending; a new swapchain starts clean.
        return false;
    }
    VkPresentInfoKHR presentInfo{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &previewCopied_[index];
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &previewSwapchain_;
    presentInfo.pImageIndices = &index;
    const VkResult presented = queuePresent_(queue_, &presentInfo);
    previewFrame_ = (previewFrame_ + 1) % 2;
    if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR) previewStale_ = true;
    return presented == VK_SUCCESS || presented == VK_SUBOPTIMAL_KHR;
}

bool VulkanRenderer::preview_present_image(VkImage image, VkFormat format, uint32_t layer, uint32_t width, uint32_t height,
                                           uint32_t windowWidth, uint32_t windowHeight)
{
    PreviewSource source;
    source.image = image;
    source.layout = kAcquiredLayout;
    source.format = format;
    source.layer = layer;
    source.width = width;
    source.height = height;
    return preview_present(source, windowWidth, windowHeight);
}

bool VulkanRenderer::preview_present_cached(int slotIndex, const BlitRegion& region, uint32_t windowWidth, uint32_t windowHeight)
{
    if (slotIndex < 0 || slotIndex >= kSlots || region.srcLayer >= 2) return false;
    const CacheSlot& slot = cache_[slotIndex];
    if (slot.image == VK_NULL_HANDLE || region.srcX < 0 || region.srcY < 0 ||
        uint64_t(region.srcX) + region.width > slot.width || uint64_t(region.srcY) + region.height > slot.height) return false;
    PreviewSource source;
    source.image = slot.image;
    source.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;  // Where cache slots stay.
    source.format = slot.format;
    source.layer = region.srcLayer;
    source.x = region.srcX;
    source.y = region.srcY;
    source.width = region.width;
    source.height = region.height;
    source.flip = region.flip;
    return preview_present(source, windowWidth, windowHeight);
}

void VulkanRenderer::wait_idle()
{
    // Every queue must be quiet: the image thread's submissions hold receiveMutex_.
    std::lock_guard<std::mutex> lock(receiveMutex_);
    if (device_ != VK_NULL_HANDLE) deviceWaitIdle_(device_);
}

} // namespace refract::host
#endif
