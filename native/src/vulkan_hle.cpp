// libvulkan.so for the guest: guest Vulkan calls go straight to the host driver.
// Vulkan structs have the same layout on Linux/arm64 and Windows/x64 (fixed-size
// types, 8-byte pointers and handles), and guest memory is host memory, so most
// commands are pass-through. This file handles the exceptions: proc-address
// lookups, Android surfaces, extension lists and guest callbacks.
#include <windows.h>

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "android_hle.h"
#include "spirv_patch.h"
#include "thunks.h"
#include "vulkan_hle.h"

namespace rn {

void ForgetVulkanInstance(VkInstance instance);
void AddTextureEmulationThunks(std::map<std::string, ThunkFn>& m);
void AddCheckpointThunks(std::map<std::string, ThunkFn>& m);

namespace {

HMODULE g_vk_dll = nullptr;
PFN_vkGetInstanceProcAddr g_gipa = nullptr;
VkInstance g_instance = VK_NULL_HANDLE;
std::vector<VkInstance> g_instances;
std::mutex g_mu;

bool LoadLoader() {
    if (g_gipa)
        return true;
    g_vk_dll = LoadLibraryW(L"vulkan-1.dll");
    if (!g_vk_dll) {
        Log("vulkan-1.dll not found");
        return false;
    }
    g_gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(g_vk_dll, "vkGetInstanceProcAddr"));
    return g_gipa != nullptr;
}

PFN_vkVoidFunction VkResolveGlobal(const char* name) {
    if (!LoadLoader())
        return nullptr;
    return g_gipa(nullptr, name);
}

PFN_vkVoidFunction VkResolve(const char* name) {
    if (!LoadLoader())
        return nullptr;
    if (auto f = reinterpret_cast<PFN_vkVoidFunction>(GetProcAddress(g_vk_dll, name)))
        return f;
    VkInstance inst;
    {
        std::lock_guard lock(g_mu);
        inst = g_instance;
    }
    if (!inst)
        return nullptr;
    return g_gipa(inst, name);
}

template <typename T>
T Fn(const char* name) {
    return reinterpret_cast<T>(VkResolve(name));
}

// --- extension name translation -------------------------------------------

std::vector<VkExtensionProperties> HostInstanceExtensions() {
    auto enumerate =
        reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(VkResolveGlobal("vkEnumerateInstanceExtensionProperties"));
    std::vector<VkExtensionProperties> v;
    if (!enumerate)
        return v;
    uint32_t n = 0;
    enumerate(nullptr, &n, nullptr);
    v.resize(n);
    enumerate(nullptr, &n, v.data());
    v.resize(n);
    return v;
}

std::vector<VkExtensionProperties> HostDeviceExtensions(VkPhysicalDevice pd) {
    auto enumerate = Fn<PFN_vkEnumerateDeviceExtensionProperties>("vkEnumerateDeviceExtensionProperties");
    std::vector<VkExtensionProperties> v;
    if (!enumerate)
        return v;
    uint32_t n = 0;
    enumerate(pd, nullptr, &n, nullptr);
    v.resize(n);
    enumerate(pd, nullptr, &n, v.data());
    v.resize(n);
    return v;
}

bool HostOnly(const char* name) {
    return strstr(name, "win32") || strstr(name, "WIN32") || strstr(name, "_NV_acquire_winrt") ||
           strstr(name, "full_screen_exclusive");
}

VkResult WriteProperties(const std::vector<VkExtensionProperties>& v, uint32_t* count, VkExtensionProperties* out) {
    if (!out) {
        *count = static_cast<uint32_t>(v.size());
        return VK_SUCCESS;
    }
    uint32_t n = std::min<uint32_t>(*count, static_cast<uint32_t>(v.size()));
    memcpy(out, v.data(), n * sizeof(VkExtensionProperties));
    *count = n;
    return n < v.size() ? VK_INCOMPLETE : VK_SUCCESS;
}

// Temporarily unlinks pNext entries the host must not see.
// A device was created with VK_EXT_fragment_density_map (render passes keep their FDM attachments then).
std::atomic<bool> g_fdm_enabled{false};
// REFRACT_VK_FAULT=1: VK_EXT_device_fault on, and what the driver knows is logged when a submit loses the device.
std::atomic<VkDevice> g_fault_device{VK_NULL_HANDLE};
bool FaultReporting() {
    static const bool on = [] {
        char v[8] = {};
        return GetEnvironmentVariableA("REFRACT_VK_FAULT", v, sizeof(v)) && v[0] == '1';
    }();
    return on;
}

struct ChainSplice {
    struct Saved {
        VkBaseOutStructure* prev;
        VkBaseOutStructure* removed;
    };
    std::vector<Saved> saved;
    void Remove(const void* head, std::initializer_list<VkStructureType> types) {
        auto* prev = reinterpret_cast<VkBaseOutStructure*>(const_cast<void*>(head));
        while (prev && prev->pNext) {
            VkBaseOutStructure* cur = prev->pNext;
            if (std::find(types.begin(), types.end(), cur->sType) != types.end()) {
                saved.push_back({prev, cur});
                prev->pNext = cur->pNext;
                continue;
            }
            prev = cur;
        }
    }
    ~ChainSplice() {
        for (auto it = saved.rbegin(); it != saved.rend(); ++it)
            it->prev->pNext = it->removed;
    }
};

// --- special commands -------------------------------------------------------------

VkResult VKAPI_CALL H_vkEnumerateInstanceExtensionProperties(const char* layer, uint32_t* count,
                                                             VkExtensionProperties* props) {
    if (layer)
        return VK_ERROR_LAYER_NOT_PRESENT;
    std::vector<VkExtensionProperties> out;
    bool win32_surface = false;
    for (const auto& e : HostInstanceExtensions()) {
        if (!strcmp(e.extensionName, VK_KHR_WIN32_SURFACE_EXTENSION_NAME))
            win32_surface = true;
        if (!HostOnly(e.extensionName))
            out.push_back(e);
    }
    if (win32_surface) {
        VkExtensionProperties a{};
        strcpy(a.extensionName, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
        a.specVersion = 6;
        out.push_back(a);
    }
    return WriteProperties(out, count, props);
}

VkResult VKAPI_CALL H_vkEnumerateDeviceExtensionProperties(VkPhysicalDevice pd, const char* layer, uint32_t* count,
                                                           VkExtensionProperties* props) {
    if (layer)
        return VK_ERROR_LAYER_NOT_PRESENT;
    std::vector<VkExtensionProperties> out;
    for (const auto& e : HostDeviceExtensions(pd))
        if (!HostOnly(e.extensionName))
            out.push_back(e);
    return WriteProperties(out, count, props);
}

VkResult VKAPI_CALL H_vkEnumerateInstanceLayerProperties(uint32_t* count, VkLayerProperties*) {
    *count = 0;
    return VK_SUCCESS;
}

VkResult VKAPI_CALL H_vkEnumerateDeviceLayerProperties(VkPhysicalDevice, uint32_t* count, VkLayerProperties*) {
    *count = 0;
    return VK_SUCCESS;
}

VkResult VKAPI_CALL H_vkCreateInstance(const VkInstanceCreateInfo* ci, const VkAllocationCallbacks*,
                                       VkInstance* out) {
    return VulkanCreateInstanceTranslated(ci, out);
}

VkResult VKAPI_CALL H_vkCreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo* ci, const VkAllocationCallbacks*,
                                     VkDevice* out) {
    return VulkanCreateDeviceTranslated(pd, ci, out);
}

}  // namespace

VkResult VulkanCreateInstanceTranslated(const VkInstanceCreateInfo* ci, VkInstance* out) {
    auto create = reinterpret_cast<PFN_vkCreateInstance>(VkResolveGlobal("vkCreateInstance"));
    if (!create)
        return VK_ERROR_INITIALIZATION_FAILED;
    std::set<std::string> host;
    for (const auto& e : HostInstanceExtensions())
        host.insert(e.extensionName);
    std::vector<const char*> exts;
    for (uint32_t i = 0; i < ci->enabledExtensionCount; ++i) {
        const char* e = ci->ppEnabledExtensionNames[i];
        if (!strcmp(e, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME))
            e = VK_KHR_WIN32_SURFACE_EXTENSION_NAME;
        if (!host.count(e)) {
            Log("vkCreateInstance: dropping unsupported extension %s", e);
            continue;
        }
        exts.push_back(e);
    }
    VkInstanceCreateInfo c = *ci;
    c.enabledExtensionCount = static_cast<uint32_t>(exts.size());
    c.ppEnabledExtensionNames = exts.data();
    c.enabledLayerCount = 0;
    c.ppEnabledLayerNames = nullptr;
    ChainSplice splice;
    splice.Remove(&c, {VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
                       VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT,
                       VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT, VK_STRUCTURE_TYPE_VALIDATION_FLAGS_EXT,
                       VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT});
    VkResult r = create(&c, nullptr, out);
    if (r == VK_SUCCESS)
        NoteVulkanInstance(*out);
    RN_INFO("vkCreateInstance(%u extensions, api 0x%x) = %d", c.enabledExtensionCount,
            ci->pApplicationInfo ? ci->pApplicationInfo->apiVersion : 0, r);
    return r;
}

void VKAPI_CALL H_vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks*) {
    auto destroy = Fn<PFN_vkDestroyInstance>("vkDestroyInstance");
    if (destroy)
        destroy(instance, nullptr);
    ForgetVulkanInstance(instance);
}

VkResult VulkanCreateDeviceTranslated(VkPhysicalDevice pd, const VkDeviceCreateInfo* ci, VkDevice* out) {
    auto create = Fn<PFN_vkCreateDevice>("vkCreateDevice");
    std::set<std::string> host;
    for (const auto& e : HostDeviceExtensions(pd))
        host.insert(e.extensionName);
    std::vector<const char*> exts;
    for (uint32_t i = 0; i < ci->enabledExtensionCount; ++i) {
        const char* e = ci->ppEnabledExtensionNames[i];
        if (!host.count(e)) {
            Log("vkCreateDevice: dropping unsupported extension %s", e);
            continue;
        }
        exts.push_back(e);
    }
    // Quest drivers always have multiview on: UE4's Oculus renderer uses multiview render passes and shaders
    // without enabling VK_KHR_multiview or the feature (VrApi didn't ask for it), which lost the device on NVIDIA.
    VkPhysicalDeviceMultiviewFeatures multiview{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES};
    multiview.multiview = VK_TRUE;
    bool multiview_set = false;
    if (host.count(VK_KHR_MULTIVIEW_EXTENSION_NAME)) {
        if (std::none_of(exts.begin(), exts.end(), [](const char* e) { return !strcmp(e, VK_KHR_MULTIVIEW_EXTENSION_NAME); }))
            exts.push_back(VK_KHR_MULTIVIEW_EXTENSION_NAME);
        for (auto* n = static_cast<const VkBaseInStructure*>(ci->pNext); n; n = n->pNext) {
            if (n->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES) {
                reinterpret_cast<VkPhysicalDeviceMultiviewFeatures*>(const_cast<VkBaseInStructure*>(n))->multiview = VK_TRUE;
                multiview_set = true;
            } else if (n->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES) {
                reinterpret_cast<VkPhysicalDeviceVulkan11Features*>(const_cast<VkBaseInStructure*>(n))->multiview = VK_TRUE;
                multiview_set = true;
            }
        }
    }
    VkDeviceCreateInfo c = *ci;
    c.enabledExtensionCount = static_cast<uint32_t>(exts.size());
    c.ppEnabledExtensionNames = exts.data();
    c.enabledLayerCount = 0;
    c.ppEnabledLayerNames = nullptr;
    if (host.count(VK_KHR_MULTIVIEW_EXTENSION_NAME) && !multiview_set) {
        multiview.pNext = const_cast<void*>(c.pNext);
        c.pNext = &multiview;
    }
    VkPhysicalDeviceFaultFeaturesEXT fault{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT};
    const bool fault_on = FaultReporting() && host.count(VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
    if (fault_on) {
        exts.push_back(VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
        c.enabledExtensionCount = static_cast<uint32_t>(exts.size());
        c.ppEnabledExtensionNames = exts.data();
        fault.deviceFault = VK_TRUE;
        fault.deviceFaultVendorBinary = VK_FALSE;
        fault.pNext = const_cast<void*>(c.pNext);
        c.pNext = &fault;
    }
    if (CheckpointsEnabled() && host.count(VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME) &&
        std::none_of(exts.begin(), exts.end(),
                     [](const char* e) { return !strcmp(e, VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME); })) {
        exts.push_back(VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME);
        c.enabledExtensionCount = static_cast<uint32_t>(exts.size());
        c.ppEnabledExtensionNames = exts.data();
    }
    DeviceFeatureFix fix(&c, pd);
    VkResult r = create(pd, &c, nullptr, out);
    if (r == VK_SUCCESS) {
        NoteVulkanDevice(*out, pd);
        if (fault_on)
            g_fault_device = *out;
        for (const char* e : exts)
            if (!strcmp(e, VK_EXT_FRAGMENT_DENSITY_MAP_EXTENSION_NAME))
                g_fdm_enabled = true;
    }
    std::string names;
    for (const char* e : exts)
        names += std::string(names.empty() ? "" : " ") + e;
    RN_INFO("vkCreateDevice(%u extensions: %s) = %d", c.enabledExtensionCount, names.c_str(), r);
    return r;
}

namespace {

VkResult VKAPI_CALL H_vkCreateAndroidSurfaceKHR(VkInstance instance, const VkAndroidSurfaceCreateInfoKHR* ci,
                                                const VkAllocationCallbacks*, VkSurfaceKHR* surface) {
    HWND hwnd = static_cast<HWND>(NativeWindowHwnd(ci->window));
    if (!hwnd) {
        Log("vkCreateAndroidSurfaceKHR: ANativeWindow %p has no host window", static_cast<void*>(ci->window));
        return VK_ERROR_SURFACE_LOST_KHR;
    }
    auto create = Fn<PFN_vkCreateWin32SurfaceKHR>("vkCreateWin32SurfaceKHR");
    if (!create)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkWin32SurfaceCreateInfoKHR w{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    w.hinstance = GetModuleHandleW(nullptr);
    w.hwnd = hwnd;
    return create(instance, &w, nullptr, surface);
}

void ReportDeviceFault(const char* what) {
    VkDevice device = g_fault_device;
    auto info_fn = Fn<PFN_vkGetDeviceFaultInfoEXT>("vkGetDeviceFaultInfoEXT");
    if (!device || !info_fn)
        return;
    VkDeviceFaultCountsEXT counts{VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
    if (info_fn(device, &counts, nullptr) != VK_SUCCESS && counts.addressInfoCount == 0 && counts.vendorInfoCount == 0) {
        Log("%s lost the device; no fault info", what);
        return;
    }
    std::vector<VkDeviceFaultAddressInfoEXT> addrs(counts.addressInfoCount);
    std::vector<VkDeviceFaultVendorInfoEXT> vendor(counts.vendorInfoCount);
    counts.vendorBinarySize = 0;
    VkDeviceFaultInfoEXT info{VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT};
    info.pAddressInfos = addrs.data();
    info.pVendorInfos = vendor.data();
    VkResult r = info_fn(device, &counts, &info);
    Log("%s lost the device (fault info %d): %s", what, r, info.description);
    static const char* kinds[] = {"none", "read invalid", "write invalid", "execute invalid",
                                  "instruction pointer unknown", "instruction pointer invalid",
                                  "instruction pointer fault"};
    for (const auto& a : addrs)
        Log("  address 0x%llx (+-0x%llx): %s", static_cast<unsigned long long>(a.reportedAddress),
            static_cast<unsigned long long>(a.addressPrecision), a.addressType < 7 ? kinds[a.addressType] : "?");
    for (const auto& v : vendor)
        Log("  vendor: %s (code 0x%llx, data 0x%llx)", v.description, static_cast<unsigned long long>(v.vendorFaultCode),
            static_cast<unsigned long long>(v.vendorFaultData));
}

VkResult VKAPI_CALL H_vkQueueSubmit(VkQueue queue, uint32_t n, const VkSubmitInfo* submits, VkFence fence) {
    VkResult r = Fn<PFN_vkQueueSubmit>("vkQueueSubmit")(queue, n, submits, fence);
    if (r == VK_ERROR_DEVICE_LOST)
        ReportDeviceFault("vkQueueSubmit");
    CheckpointDeviceLost(r, "vkQueueSubmit");
    return r;
}

VkResult VKAPI_CALL H_vkQueueSubmit2(VkQueue queue, uint32_t n, const VkSubmitInfo2* submits, VkFence fence) {
    VkResult r = Fn<PFN_vkQueueSubmit2>("vkQueueSubmit2")(queue, n, submits, fence);
    if (r == VK_ERROR_DEVICE_LOST)
        ReportDeviceFault("vkQueueSubmit2");
    CheckpointDeviceLost(r, "vkQueueSubmit2");
    return r;
}

// Quest shaders may read function variables before writing them (Adreno zero-initializes them): see spirv_patch.cpp.
VkResult VKAPI_CALL H_vkCreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo* ci,
                                           const VkAllocationCallbacks*, VkShaderModule* out) {
    auto create = Fn<PFN_vkCreateShaderModule>("vkCreateShaderModule");
    std::vector<uint32_t> patched;
    uint32_t count = 0;
    VkShaderModuleCreateInfo c = *ci;
    if (ZeroInitFunctionVariables(ci->pCode, ci->codeSize / 4, &patched, &count)) {
        c.pCode = patched.data();
        c.codeSize = patched.size() * 4;
        static std::atomic<uint32_t> modules{0}, variables{0};
        const uint32_t m = ++modules, v = variables += count;
        if (m == 1 || (m & (m - 1)) == 0)
            RN_INFO("SPIR-V: zero-initialized %u function variable(s) in %u shader module(s) so far", v, m);
    }
    VkResult r = create(device, &c, nullptr, out);
    if (r == VK_SUCCESS)
        CheckpointShaderModule(reinterpret_cast<unsigned long long>(*out), c.pCode, c.codeSize);
    return r;
}

// Pipelines compile when the game creates them (UE4: at first use, mid-frame); slow ones are logged as the
// likely cause of hitches.
VkResult VKAPI_CALL H_vkCreateGraphicsPipelines(VkDevice device, VkPipelineCache cache, uint32_t n,
                                                const VkGraphicsPipelineCreateInfo* infos,
                                                const VkAllocationCallbacks*, VkPipeline* out) {
    const auto start = std::chrono::steady_clock::now();
    VkResult r = Fn<PFN_vkCreateGraphicsPipelines>("vkCreateGraphicsPipelines")(device, cache, n, infos, nullptr, out);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    static std::atomic<uint32_t> created{0}, slow{0};
    created += n;
    if (ms > 8.0) {
        const uint32_t k = ++slow;
        if (k <= 20 || (k & (k - 1)) == 0)
            RN_INFO("Vulkan: vkCreateGraphicsPipelines(%u) took %.1f ms (%u slow of %u pipelines so far)", n, ms, k,
                    created.load());
    }
    if (r == VK_SUCCESS)
        CheckpointPipelines(infos, n, out);
    return r;
}

// UE4's Oculus renderer gives its render passes a fragment density map attachment (Quest foveation) even on
// devices without VK_EXT_fragment_density_map. Without the extension the FDM layout and create info are invalid:
// the attachment becomes an ordinary unused one.

VkImageLayout WithoutFdm(VkImageLayout l) {
    return l == VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : l;
}

VkResult VKAPI_CALL H_vkCreateRenderPass(VkDevice device, const VkRenderPassCreateInfo* ci,
                                         const VkAllocationCallbacks*, VkRenderPass* out) {
    auto create = Fn<PFN_vkCreateRenderPass>("vkCreateRenderPass");
    if (g_fdm_enabled)
        return create(device, ci, nullptr, out);
    VkRenderPassCreateInfo c = *ci;
    std::vector<VkAttachmentDescription> att(ci->pAttachments, ci->pAttachments + ci->attachmentCount);
    for (auto& a : att) {
        a.initialLayout = WithoutFdm(a.initialLayout);
        a.finalLayout = WithoutFdm(a.finalLayout);
    }
    c.pAttachments = att.data();
    ChainSplice splice;
    splice.Remove(&c, {VK_STRUCTURE_TYPE_RENDER_PASS_FRAGMENT_DENSITY_MAP_CREATE_INFO_EXT});
    return create(device, &c, nullptr, out);
}

VkResult RenderPass2WithoutFdm(const char* name, VkDevice device, const VkRenderPassCreateInfo2* ci,
                               VkRenderPass* out) {
    auto create = Fn<PFN_vkCreateRenderPass2>(name);
    if (g_fdm_enabled)
        return create(device, ci, nullptr, out);
    VkRenderPassCreateInfo2 c = *ci;
    std::vector<VkAttachmentDescription2> att(ci->pAttachments, ci->pAttachments + ci->attachmentCount);
    for (auto& a : att) {
        a.initialLayout = WithoutFdm(a.initialLayout);
        a.finalLayout = WithoutFdm(a.finalLayout);
    }
    c.pAttachments = att.data();
    ChainSplice splice;
    splice.Remove(&c, {VK_STRUCTURE_TYPE_RENDER_PASS_FRAGMENT_DENSITY_MAP_CREATE_INFO_EXT});
    return create(device, &c, nullptr, out);
}
VkResult VKAPI_CALL H_vkCreateRenderPass2(VkDevice device, const VkRenderPassCreateInfo2* ci,
                                          const VkAllocationCallbacks*, VkRenderPass* out) {
    return RenderPass2WithoutFdm("vkCreateRenderPass2", device, ci, out);
}
VkResult VKAPI_CALL H_vkCreateRenderPass2KHR(VkDevice device, const VkRenderPassCreateInfo2* ci,
                                             const VkAllocationCallbacks*, VkRenderPass* out) {
    return RenderPass2WithoutFdm("vkCreateRenderPass2KHR", device, ci, out);
}

// Debug messengers would call guest code from driver threads; accept and ignore.
VkResult VKAPI_CALL H_vkCreateDebugUtilsMessengerEXT(VkInstance, const VkDebugUtilsMessengerCreateInfoEXT*,
                                                     const VkAllocationCallbacks*, VkDebugUtilsMessengerEXT* m) {
    static uint64_t next = 0x7e570000;
    *m = reinterpret_cast<VkDebugUtilsMessengerEXT>(++next);
    return VK_SUCCESS;
}
void VKAPI_CALL H_vkDestroyDebugUtilsMessengerEXT(VkInstance, VkDebugUtilsMessengerEXT, const VkAllocationCallbacks*) {}
VkResult VKAPI_CALL H_vkCreateDebugReportCallbackEXT(VkInstance, const VkDebugReportCallbackCreateInfoEXT*,
                                                     const VkAllocationCallbacks*, VkDebugReportCallbackEXT* m) {
    static uint64_t next = 0x7e580000;
    *m = reinterpret_cast<VkDebugReportCallbackEXT>(++next);
    return VK_SUCCESS;
}
void VKAPI_CALL H_vkDestroyDebugReportCallbackEXT(VkInstance, VkDebugReportCallbackEXT, const VkAllocationCallbacks*) {}

VkResult VKAPI_CALL H_vkGetAndroidHardwareBufferPropertiesANDROID(VkDevice, const void*, void*) {
    Log("vkGetAndroidHardwareBufferPropertiesANDROID is not supported");
    return VK_ERROR_INVALID_EXTERNAL_HANDLE;
}
VkResult VKAPI_CALL H_vkGetMemoryAndroidHardwareBufferANDROID(VkDevice, const void*, void**) {
    Log("vkGetMemoryAndroidHardwareBufferANDROID is not supported");
    return VK_ERROR_INVALID_EXTERNAL_HANDLE;
}

void VKAPI_CALL H_vkGetPhysicalDeviceProperties(VkPhysicalDevice pd, VkPhysicalDeviceProperties* p) {
    Fn<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties")(pd, p);
}
void VKAPI_CALL H_vkGetPhysicalDeviceProperties2(VkPhysicalDevice pd, VkPhysicalDeviceProperties2* p) {
    Fn<PFN_vkGetPhysicalDeviceProperties2>("vkGetPhysicalDeviceProperties2")(pd, p);
}

bool IsSpecial(const char* name);

PFN_vkVoidFunction VKAPI_CALL H_vkGetInstanceProcAddr(VkInstance instance, const char* name) {
    if (!name)
        return nullptr;
    u64 stub = StubAddress(kLibVulkan, name);
    if (!stub)
        return nullptr;
    if (!IsSpecial(name) && !(instance ? VkResolve(name) : VkResolveGlobal(name)))
        return nullptr;
    return reinterpret_cast<PFN_vkVoidFunction>(stub);
}

PFN_vkVoidFunction VKAPI_CALL H_vkGetDeviceProcAddr(VkDevice device, const char* name) {
    if (!name)
        return nullptr;
    u64 stub = StubAddress(kLibVulkan, name);
    if (!stub)
        return nullptr;
    if (!IsSpecial(name)) {
        auto gdpa = Fn<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
        if (!gdpa || !gdpa(device, name))
            return nullptr;
    }
    return reinterpret_cast<PFN_vkVoidFunction>(stub);
}

#include "gen/vulkan_thunks.inc"

const std::map<std::string, ThunkFn>& SpecialThunks() {
    static const std::map<std::string, ThunkFn> m = {
        {"vkGetInstanceProcAddr", &Wrap<&H_vkGetInstanceProcAddr>},
        {"vkGetDeviceProcAddr", &Wrap<&H_vkGetDeviceProcAddr>},
        {"vkCreateInstance", &Wrap<&H_vkCreateInstance>},
        {"vkDestroyInstance", &Wrap<&H_vkDestroyInstance>},
        {"vkEnumerateInstanceExtensionProperties", &Wrap<&H_vkEnumerateInstanceExtensionProperties>},
        {"vkEnumerateDeviceExtensionProperties", &Wrap<&H_vkEnumerateDeviceExtensionProperties>},
        {"vkEnumerateInstanceLayerProperties", &Wrap<&H_vkEnumerateInstanceLayerProperties>},
        {"vkEnumerateDeviceLayerProperties", &Wrap<&H_vkEnumerateDeviceLayerProperties>},
        {"vkCreateDevice", &Wrap<&H_vkCreateDevice>},
        {"vkCreateAndroidSurfaceKHR", &Wrap<&H_vkCreateAndroidSurfaceKHR>},
        {"vkCreateRenderPass", &Wrap<&H_vkCreateRenderPass>},
        {"vkCreateShaderModule", &Wrap<&H_vkCreateShaderModule>},
        {"vkCreateGraphicsPipelines", &Wrap<&H_vkCreateGraphicsPipelines>},
        {"vkQueueSubmit", &Wrap<&H_vkQueueSubmit>},
        {"vkQueueSubmit2", &Wrap<&H_vkQueueSubmit2>},
        {"vkCreateRenderPass2", &Wrap<&H_vkCreateRenderPass2>},
        {"vkCreateRenderPass2KHR", &Wrap<&H_vkCreateRenderPass2KHR>},
        {"vkCreateDebugUtilsMessengerEXT", &Wrap<&H_vkCreateDebugUtilsMessengerEXT>},
        {"vkDestroyDebugUtilsMessengerEXT", &Wrap<&H_vkDestroyDebugUtilsMessengerEXT>},
        {"vkCreateDebugReportCallbackEXT", &Wrap<&H_vkCreateDebugReportCallbackEXT>},
        {"vkDestroyDebugReportCallbackEXT", &Wrap<&H_vkDestroyDebugReportCallbackEXT>},
        {"vkGetAndroidHardwareBufferPropertiesANDROID", &Wrap<&H_vkGetAndroidHardwareBufferPropertiesANDROID>},
        {"vkGetMemoryAndroidHardwareBufferANDROID", &Wrap<&H_vkGetMemoryAndroidHardwareBufferANDROID>},
        {"vkGetPhysicalDeviceProperties", &Wrap<&H_vkGetPhysicalDeviceProperties>},
        {"vkGetPhysicalDeviceProperties2", &Wrap<&H_vkGetPhysicalDeviceProperties2>},
        {"vkGetPhysicalDeviceProperties2KHR", &Wrap<&H_vkGetPhysicalDeviceProperties2>},
    };
    return m;
}

bool IsSpecial(const char* name) { return SpecialThunks().count(name) != 0; }

}  // namespace

void NoteVulkanInstance(void* instance) {
    std::lock_guard lock(g_mu);
    g_instances.push_back(static_cast<VkInstance>(instance));
    if (!g_instance)
        g_instance = static_cast<VkInstance>(instance);
}

// Games create throwaway instances (capability probes); never resolve through a destroyed one.
void ForgetVulkanInstance(VkInstance instance) {
    std::lock_guard lock(g_mu);
    g_instances.erase(std::remove(g_instances.begin(), g_instances.end(), instance), g_instances.end());
    if (g_instance == instance)
        g_instance = g_instances.empty() ? VK_NULL_HANDLE : g_instances.back();
}

void* VulkanHostProc(const char* name) { return reinterpret_cast<void*>(VkResolve(name)); }

void* HostVkGetInstanceProcAddr() {
    LoadLoader();
    return reinterpret_cast<void*>(g_gipa);
}

void RegisterVulkanHle() {
    std::map<std::string, ThunkFn> m;
    AddGeneratedVulkanThunks(m);
    AddTextureEmulationThunks(m);
    AddCheckpointThunks(m);
    for (const auto& [k, v] : SpecialThunks())
        m[k] = v;
    RegisterHostLibrary(kLibVulkan, "libvulkan.so", std::move(m));
}

}  // namespace rn
