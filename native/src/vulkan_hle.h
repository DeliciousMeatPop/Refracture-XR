#pragma once

namespace rn {

void RegisterVulkanHle();
// Lets instance-level lookups work for instances created elsewhere (e.g. by OpenXR).
void NoteVulkanInstance(void* instance);
// The host loader's vkGetInstanceProcAddr (for XR_KHR_vulkan_enable2).
void* HostVkGetInstanceProcAddr();
// A host Vulkan function (through the first live instance).
void* VulkanHostProc(const char* name);
// Remembers which physical device a VkDevice was created on (vk_texcomp.cpp).
void NoteVulkanDevice(void* device, void* physical_device);

// Clears emulated features (ASTC, ETC2) in a VkDeviceCreateInfo* for the host's
// vkCreateDevice on physical_device; undone when the object goes out of scope.
class DeviceFeatureFix {
public:
    DeviceFeatureFix(void* create_info, void* physical_device);
    ~DeviceFeatureFix();
    DeviceFeatureFix(const DeviceFeatureFix&) = delete;
    DeviceFeatureFix& operator=(const DeviceFeatureFix&) = delete;

private:
    void* owned_ = nullptr;
    void* patched_ = nullptr;  // VkPhysicalDeviceFeatures in the app's chain
    void* saved_ = nullptr;    // its original contents
};

// REFRACT_VK_CHECKPOINTS=1 (vk_checkpoints.cpp): NV diagnostic checkpoints in the app's command buffers,
// dumped on the first lost device.
bool CheckpointsEnabled();
void CheckpointBeginRenderPass(void* command_buffer, const void* render_pass_begin_info);
void CheckpointUpload(void* command_buffer, unsigned long long image, unsigned regions);
void CheckpointDeviceLost(int result, const char* what);
void CheckpointShaderModule(unsigned long long module, const void* code, size_t bytes);
void CheckpointPipelines(const void* graphics_create_infos, unsigned count, const void* pipelines);

#ifdef VK_VERSION_1_0
// vkCreateInstance/vkCreateDevice with Android -> Windows extension translation.
VkResult VulkanCreateInstanceTranslated(const VkInstanceCreateInfo* ci, VkInstance* out);
VkResult VulkanCreateDeviceTranslated(VkPhysicalDevice pd, const VkDeviceCreateInfo* ci, VkDevice* out);
#endif

}  // namespace rn
