#include "stats_overlay.h"
#if defined(_WIN32)
#include <windows.h>
#else
#include "mono_font_files.h"
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

#include <fstream>
#include <iterator>
#include <map>
#endif

#include <algorithm>
#include <cmath>

namespace refract::host {
namespace {
constexpr float kCardOpacity = 0.78f;
constexpr float kCardGrey = 18.0f;  // The card's colour, 0-255.
#if !defined(_WIN32)
constexpr int kEncodeSteps = 4096;
#endif
}

#if !defined(_WIN32)
// The desktop's monospace font at one size, with each character rasterized the first time it is drawn. Every
// character takes the width of a '0', so the numbers line up in columns.
struct StatsOverlay::Font {
    struct Glyph { int width = 0, height = 0, left = 0, top = 0; std::vector<uint8_t> coverage; };
    std::vector<unsigned char> data;
    stbtt_fontinfo info{};
    float scale = 0;
    int advance = 0, ascent = 0;
    std::map<uint32_t, Glyph> glyphs;

    bool load()
    {
        for (const std::string& path : mono_font_candidates(true)) {
            std::ifstream file(path, std::ios::binary);
            data.assign(std::istreambuf_iterator<char>(file), {});
            const int offset = data.empty() ? -1 : stbtt_GetFontOffsetForIndex(data.data(), 0);
            if (offset >= 0 && stbtt_InitFont(&info, data.data(), offset)) return true;
        }
        return false;
    }
    // Em size in pixels.
    void set_size(float pixels)
    {
        scale = stbtt_ScaleForMappingEmToPixels(&info, pixels);
        int ascender = 0, descender = 0, gap = 0, width = 0, bearing = 0;
        stbtt_GetFontVMetrics(&info, &ascender, &descender, &gap);
        stbtt_GetCodepointHMetrics(&info, '0', &width, &bearing);
        advance = std::max(1, int(std::lround(width * scale)));
        ascent = int(std::lround(ascender * scale));
        glyphs.clear();
    }
    // The '0' advance as a fraction of the em.
    float advance_em() const
    {
        int width = 0, bearing = 0;
        stbtt_GetCodepointHMetrics(&info, '0', &width, &bearing);
        return width * stbtt_ScaleForMappingEmToPixels(&info, 1.0f);
    }
    const Glyph& glyph(uint32_t codepoint)
    {
        auto found = glyphs.find(codepoint);
        if (found != glyphs.end()) return found->second;
        Glyph& glyph = glyphs[codepoint];
        const int index = stbtt_FindGlyphIndex(&info, int(codepoint));
        unsigned char* bitmap = stbtt_GetGlyphBitmap(&info, scale, scale, index ? index : stbtt_FindGlyphIndex(&info, '?'),
            &glyph.width, &glyph.height, &glyph.left, &glyph.top);
        if (bitmap) glyph.coverage.assign(bitmap, bitmap + size_t(glyph.width) * glyph.height);
        stbtt_FreeBitmap(bitmap, nullptr);
        return glyph;
    }
};
#endif

StatsOverlay::StatsOverlay() = default;

StatsOverlay::~StatsOverlay()
{
#if defined(_WIN32)
    if (dc_) DeleteDC(static_cast<HDC>(dc_));
    if (bitmap_) DeleteObject(static_cast<HBITMAP>(bitmap_));
    if (font_) DeleteObject(static_cast<HFONT>(font_));
#endif
}

bool StatsOverlay::prepare(uint32_t width, uint32_t height, uint32_t rows)
{
    const float pad = 0.06f * height;
#if defined(_WIN32)
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = static_cast<LONG>(width);
    info.bmiHeader.biHeight = -static_cast<LONG>(height);  // Top-down.
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    HDC dc = CreateCompatibleDC(nullptr);
    void* bits = nullptr;
    HBITMAP bitmap = dc ? CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    const int fontHeight = static_cast<int>((height - 2.0f * pad) / rows * 0.74f);
    // Grey antialiasing (not ClearType): the red, green and blue channels then all hold the coverage.
    HFONT font = CreateFontW(-fontHeight, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
        CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    if (!dc || !bitmap || !font) {
        if (dc) DeleteDC(dc);
        if (bitmap) DeleteObject(bitmap);
        if (font) DeleteObject(font);
        return false;
    }
    SelectObject(dc, bitmap);
    SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    dc_ = dc; bitmap_ = bitmap; font_ = font;
    dib_ = static_cast<uint8_t*>(bits);
#else
    // The longest line has 50 columns; the em shrinks from a row's 74% if the font is wider than Consolas.
    auto font = std::make_unique<Font>();
    if (!font->load()) return false;
    constexpr float kColumns = 50.0f;
    font->set_size(std::min((height - 2.0f * pad) / rows * 0.74f, (width - 2.8f * pad) / kColumns / font->advance_em()));
    font_ = std::move(font);
    text_.assign(static_cast<size_t>(width) * height, 0);
#endif
    width_ = width; height_ = height; rows_ = rows;

    // Rounded corners, antialiased.
    const float radius = 0.08f * height;
    card_.resize(static_cast<size_t>(width) * height);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const float dx = (std::max)(std::fabs(x + 0.5f - 0.5f * width) - (0.5f * width - radius), 0.0f);
            const float dy = (std::max)(std::fabs(y + 0.5f - 0.5f * height) - (0.5f * height - radius), 0.0f);
            const float distance = std::sqrt(dx * dx + dy * dy) - radius;
            card_[static_cast<size_t>(y) * width + x] = std::clamp(0.5f - distance, 0.0f, 1.0f) * kCardOpacity;
        }
    }
    pixels_.assign(static_cast<size_t>(width) * height * 4, 0);
    return true;
}

void StatsOverlay::draw(const std::vector<Line>& lines)
{
    if (!font_) return;
    const float pad = 0.06f * height_;
    const float rowHeight = (height_ - 2.0f * pad) / rows_;
    const size_t count = (std::min<size_t>)(lines.size(), rows_);
#if defined(_WIN32)
    HDC dc = static_cast<HDC>(dc_);
    std::fill(dib_, dib_ + static_cast<size_t>(width_) * height_ * 4, uint8_t(0));
    for (size_t i = 0; i < count; ++i) {
        TextOutW(dc, static_cast<int>(1.4f * pad), static_cast<int>(pad + i * rowHeight),
                 lines[i].text.c_str(), static_cast<int>(lines[i].text.size()));
    }
    GdiFlush();
    const auto coverage = [&](size_t index) { return dib_[index * 4 + 1] / 255.0f; };
#else
    std::fill(text_.begin(), text_.end(), uint8_t(0));
    for (size_t i = 0; i < count; ++i) {
        // The cell's top is where GDI starts a line; the baseline is the font's ascent below it.
        const int baseline = static_cast<int>(pad + i * rowHeight) + font_->ascent;
        int x = static_cast<int>(1.4f * pad);
        for (wchar_t c : lines[i].text) {
            const Font::Glyph& glyph = font_->glyph(static_cast<uint32_t>(c));
            for (int gy = 0; gy < glyph.height; ++gy) {
                const int y = baseline + glyph.top + gy;
                if (y < 0 || y >= int(height_)) continue;
                for (int gx = 0; gx < glyph.width; ++gx) {
                    const int px = x + glyph.left + gx;
                    if (px < 0 || px >= int(width_)) continue;
                    uint8_t& out = text_[static_cast<size_t>(y) * width_ + px];
                    out = static_cast<uint8_t>(std::min(255, out + glyph.coverage[static_cast<size_t>(gy) * glyph.width + gx]));
                }
            }
            x += font_->advance;
        }
    }
    const auto coverage = [&](size_t index) { return text_[index] / 255.0f; };
#endif

    for (uint32_t y = 0; y < height_; ++y) {
        const size_t row = rowHeight > 0 ? static_cast<size_t>((std::max)(0.0f, (y - pad) / rowHeight)) : 0;
        const uint32_t color = row < count ? lines[row].color : 0xEDEDED;
        const float red = float(color >> 16 & 0xff), green = float(color >> 8 & 0xff), blue = float(color & 0xff);
#if !defined(_WIN32)
        // The swapchain is sRGB, so the runtime blends decoded (linear) values: the edges' partial coverage mixes in
        // linear light, or thin strokes come out too dark and look broken. GDI's antialiasing already allows for it.
        const auto linear = [](float value) {
            value /= 255.0f;
            return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
        };
        // Linear [0, 1] in 4096 steps to sRGB bytes (pow per pixel would cost the frame loop milliseconds).
        static const std::vector<uint8_t> encoded = [] {
            std::vector<uint8_t> table(kEncodeSteps + 1);
            for (int i = 0; i <= kEncodeSteps; ++i) {
                const float value = float(i) / kEncodeSteps;
                table[i] = static_cast<uint8_t>(
                    255.0f * (value <= 0.0031308f ? value * 12.92f : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f) + 0.5f);
            }
            return table;
        }();
        const auto encode = [](float value) {
            return encoded[static_cast<size_t>(std::clamp(value, 0.0f, 1.0f) * kEncodeSteps + 0.5f)];
        };
        const float textLinear[3] = {linear(red), linear(green), linear(blue)}, cardLinear = linear(kCardGrey);
#endif
        for (uint32_t x = 0; x < width_; ++x) {
            const size_t index = static_cast<size_t>(y) * width_ + x;
            const float text = coverage(index);
            const float card = card_[index] * (1.0f - text);
            uint8_t* out = &pixels_[index * 4];
#if defined(_WIN32)
            out[0] = static_cast<uint8_t>(kCardGrey * card + red * text + 0.5f);
            out[1] = static_cast<uint8_t>(kCardGrey * card + green * text + 0.5f);
            out[2] = static_cast<uint8_t>(kCardGrey * card + blue * text + 0.5f);
#else
            for (int c = 0; c < 3; ++c) out[c] = encode(cardLinear * card + textLinear[c] * text);
#endif
            out[3] = static_cast<uint8_t>((card + text) * 255.0f + 0.5f);
        }
    }
}
}
