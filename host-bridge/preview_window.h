#pragma once
#if !defined(_WIN32) && defined(REFRACT_PREVIEW_WINDOW)
// Linux counterpart of MirrorWindow: a window showing the game's left eye while it plays in the headset, so it can be
// seen, and so the menu button (M held) and the performance panel (F1) can be reached from the keyboard. SDL3 runs it
// on a thread of its own. It is a Vulkan surface the bridge's renderer presents to (VulkanRenderer::preview_attach),
// as the Windows mirror is a DXGI swapchain on the bridge's D3D11 device.
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

namespace refract::host {

class PreviewWindow {
public:
    PreviewWindow() = default;
    PreviewWindow(const PreviewWindow&) = delete;
    PreviewWindow& operator=(const PreviewWindow&) = delete;
    // The window goes last: the renderer destroys the surface before this.
    ~PreviewWindow();

    // Starts the window thread and makes the window's surface on `instance`. False when SDL3 cannot open a Vulkan window
    // (no display); the bridge then runs without one.
    bool open(const std::string& gameName, VkInstance instance);
    // The surface open() made, for the renderer to take (once).
    VkSurfaceKHR take_surface() { return surface_.exchange(VK_NULL_HANDLE); }
    bool is_open() const { return thread_.joinable(); }
    // The window's size in pixels; 0 x 0 while it is minimized.
    void size(uint32_t* width, uint32_t* height) const;
    // The window was resized or uncovered since the last call: the frame on show should be presented again.
    bool take_dirty() { return dirty_.exchange(false); }
    void mark_dirty() { dirty_ = true; }
    // M held while the window has focus holds the left menu button, as on Windows.
    bool menu_key_held() const { return menuHeld_.load(); }
    // F1 toggles the headset's performance panel; true once per press.
    bool take_stats_toggle() { return statsToggle_.exchange(false); }
    // The window was closed: the session ends, as on Windows.
    bool closed() const { return closed_.load(); }

private:
    void run(std::string title, VkInstance instance);

    std::thread thread_;
    std::atomic<VkSurfaceKHR> surface_{VK_NULL_HANDLE};
    std::atomic<uint64_t> size_{0};  // Width in the high half, height in the low.
    std::atomic<bool> menuHeld_{false}, statsToggle_{false}, closed_{false}, stop_{false}, failed_{false}, ready_{false}, dirty_{true};
};

}  // namespace refract::host
#endif
