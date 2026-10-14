#include "preview_window.h"
#if !defined(_WIN32) && defined(REFRACT_PREVIEW_WINDOW)

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <chrono>
#include <cstdio>

namespace refract::host {

PreviewWindow::~PreviewWindow()
{
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}

bool PreviewWindow::open(const std::string& gameName, VkInstance instance)
{
    thread_ = std::thread([this, title = gameName + " | Refract", instance] { run(title, instance); });
    // The thread says whether the window came up; SDL's video start can take a moment on Wayland.
    for (int i = 0; i < 200 && !ready_ && !failed_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (failed_ || !ready_) {
        stop_ = true;
        thread_.join();
        return false;
    }
    return true;
}

void PreviewWindow::size(uint32_t* width, uint32_t* height) const
{
    const uint64_t packed = size_.load();
    *width = static_cast<uint32_t>(packed >> 32);
    *height = static_cast<uint32_t>(packed);
}

void PreviewWindow::run(std::string title, VkInstance instance)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "Refract preview: no window (%s)\n", SDL_GetError());
        failed_ = true;
        return;
    }
    SDL_Window* window = SDL_CreateWindow(title.c_str(), 680, 720, SDL_WINDOW_RESIZABLE | SDL_WINDOW_VULKAN);
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (!window || !SDL_Vulkan_CreateSurface(window, instance, nullptr, &surface)) {
        std::fprintf(stderr, "Refract preview: no window (%s)\n", SDL_GetError());
        if (window) SDL_DestroyWindow(window);
        SDL_Quit();
        failed_ = true;
        return;
    }
    const auto update_size = [&] {
        int width = 0, height = 0;
        // Only minimized counts: a Wayland window shows up at its first present, so a covered one still gets frames.
        if (!(SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED)) SDL_GetWindowSizeInPixels(window, &width, &height);
        size_ = (static_cast<uint64_t>(width > 0 ? width : 0) << 32) | static_cast<uint32_t>(height > 0 ? height : 0);
        dirty_ = true;
    };
    update_size();
    surface_ = surface;
    ready_ = true;
    while (!stop_) {
        SDL_Event event;
        if (!SDL_WaitEventTimeout(&event, 50)) continue;
        do {
            switch (event.type) {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                closed_ = true;
                break;
            case SDL_EVENT_KEY_DOWN:
                if (event.key.key == SDLK_F1 && !event.key.repeat) statsToggle_ = true;
                if (event.key.key == SDLK_M) menuHeld_ = true;
                break;
            case SDL_EVENT_KEY_UP:
                if (event.key.key == SDLK_M) menuHeld_ = false;
                break;
            case SDL_EVENT_WINDOW_FOCUS_LOST:
                menuHeld_ = false;  // A key released elsewhere would otherwise stay held.
                break;
            case SDL_EVENT_WINDOW_EXPOSED:
            case SDL_EVENT_WINDOW_RESIZED:
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            case SDL_EVENT_WINDOW_MINIMIZED:
            case SDL_EVENT_WINDOW_RESTORED:
            case SDL_EVENT_WINDOW_SHOWN:
                update_size();
                break;
            default:
                break;
            }
        } while (SDL_PollEvent(&event));
    }
    // The renderer destroyed the surface (the session ended first); the window can go.
    SDL_DestroyWindow(window);
    SDL_Quit();
}

}  // namespace refract::host
#endif
