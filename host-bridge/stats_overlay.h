#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace refract::host {
// The performance panel shown in the headset: lines of text on a rounded, translucent dark card,
// drawn on the CPU into premultiplied RGBA (GDI on Windows, stb_truetype on Linux). It is redrawn a few
// times a second at most.
class StatsOverlay {
public:
    struct Line {
        std::wstring text;
        uint32_t color = 0xEDEDED;  // 0xRRGGBB.
    };

    StatsOverlay();
    ~StatsOverlay();
    // Allocates a width x height card and a monospaced font that fits `rows` lines.
    bool prepare(uint32_t width, uint32_t height, uint32_t rows);
    void draw(const std::vector<Line>& lines);
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    const uint8_t* pixels() const { return pixels_.data(); }  // Premultiplied RGBA, top row first.

private:
    uint32_t width_ = 0, height_ = 0, rows_ = 0;
#if defined(_WIN32)
    void* dc_ = nullptr;      // HDC
    void* bitmap_ = nullptr;  // HBITMAP (32-bit top-down DIB)
    void* font_ = nullptr;    // HFONT
    uint8_t* dib_ = nullptr;  // The DIB's BGRX pixels: white text coverage on black.
#else
    struct Font;
    std::unique_ptr<Font> font_;
    std::vector<uint8_t> text_;  // White text coverage on black, one byte a pixel.
#endif
    std::vector<float> card_;       // Card opacity per pixel (rounded corners).
    std::vector<uint8_t> pixels_;
};
}
