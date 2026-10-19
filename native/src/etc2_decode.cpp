// ETC2/EAC decoding, following the OpenGL ES 3.0 spec (appendix C.1). Blocks are 4x4 texels, stored big-endian;
// texel indices run down columns first (texel k is at x = k / 4, y = k % 4).
#include "etc2_decode.h"

#include "bc_encode.h"

#include <algorithm>
#include <cstring>

namespace rn {

namespace {

constexpr int kEtc1Modifiers[8][2] = {{2, 8},   {5, 17},  {9, 29},  {13, 42},
                                      {18, 60}, {24, 80}, {33, 106}, {47, 183}};
constexpr int kDistances[8] = {3, 6, 11, 16, 23, 32, 41, 64};
constexpr int kEacModifiers[16][8] = {
    {-3, -6, -9, -15, 2, 5, 8, 14}, {-3, -7, -10, -13, 2, 6, 9, 12}, {-2, -5, -8, -13, 1, 4, 7, 12},
    {-2, -4, -6, -13, 1, 3, 5, 12}, {-3, -6, -8, -12, 2, 5, 7, 11}, {-3, -7, -9, -11, 2, 6, 8, 10},
    {-4, -7, -8, -11, 3, 6, 7, 10}, {-3, -5, -8, -11, 2, 4, 7, 10}, {-2, -6, -8, -10, 1, 5, 7, 9},
    {-2, -5, -8, -10, 1, 4, 7, 9},  {-2, -4, -8, -10, 1, 3, 7, 9},  {-2, -5, -7, -10, 1, 4, 6, 9},
    {-3, -4, -7, -10, 2, 3, 6, 9},  {-1, -2, -3, -10, 0, 1, 2, 9},  {-4, -6, -8, -9, 3, 5, 7, 8},
    {-3, -5, -7, -9, 2, 4, 6, 8}};

uint8_t Clamp8(int v) {
    return static_cast<uint8_t>(std::clamp(v, 0, 255));
}
int Extend4(int c) {
    return c << 4 | c;
}
int Extend5(int c) {
    return c << 3 | c >> 2;
}
int Extend6(int c) {
    return c << 2 | c >> 4;
}
int Extend7(int c) {
    return c << 1 | c >> 6;
}
int SignExtend3(int v) {
    return v & 4 ? v - 8 : v;
}

// One ETC2 RGB block into out[y * 4 + x] (RGBA). With punch_through, bit 33 is the "opaque" bit instead of
// the individual/differential switch, and an index of 2 in a non-opaque block is transparent black.
void DecodeColorBlock(const uint8_t* s, bool punch_through, uint8_t out[16][4]) {
    const uint32_t indices = uint32_t(s[4]) << 24 | uint32_t(s[5]) << 16 | uint32_t(s[6]) << 8 | s[7];
    auto index = [&](int x, int y) {
        const int k = x * 4 + y;
        return int((indices >> (k + 16)) & 1) << 1 | int((indices >> k) & 1);
    };
    const bool diff_bit = (s[3] & 2) != 0;
    const bool opaque = !punch_through || diff_bit;
    auto put = [&](int x, int y, int r, int g, int b, int idx) {
        uint8_t* p = out[y * 4 + x];
        if (!opaque && idx == 2) {
            p[0] = p[1] = p[2] = p[3] = 0;
            return;
        }
        p[0] = Clamp8(r), p[1] = Clamp8(g), p[2] = Clamp8(b), p[3] = 255;
    };

    int base[2][3];
    if (!punch_through && !diff_bit) {  // individual mode
        for (int i = 0; i < 3; ++i) {
            base[0][i] = Extend4(s[i] >> 4);
            base[1][i] = Extend4(s[i] & 15);
        }
    } else {
        const int r = s[0] >> 3, g = s[1] >> 3, b = s[2] >> 3;
        const int r2 = r + SignExtend3(s[0] & 7), g2 = g + SignExtend3(s[1] & 7), b2 = b + SignExtend3(s[2] & 7);
        if (r2 < 0 || r2 > 31) {  // T mode
            const int c1[3] = {Extend4((s[0] & 0x18) >> 1 | (s[0] & 3)), Extend4(s[1] >> 4), Extend4(s[1] & 15)};
            const int c2[3] = {Extend4(s[2] >> 4), Extend4(s[2] & 15), Extend4(s[3] >> 4)};
            const int d = kDistances[(s[3] & 0x0c) >> 1 | (s[3] & 1)];
            const int paint[4][3] = {{c1[0], c1[1], c1[2]},
                                     {c2[0] + d, c2[1] + d, c2[2] + d},
                                     {c2[0], c2[1], c2[2]},
                                     {c2[0] - d, c2[1] - d, c2[2] - d}};
            for (int x = 0; x < 4; ++x)
                for (int y = 0; y < 4; ++y) {
                    const int i = index(x, y);
                    put(x, y, paint[i][0], paint[i][1], paint[i][2], i);
                }
            return;
        }
        if (g2 < 0 || g2 > 31) {  // H mode
            const int c1[3] = {Extend4((s[0] & 0x78) >> 3), Extend4((s[0] & 7) << 1 | (s[1] & 0x10) >> 4),
                               Extend4((s[1] & 8) | (s[1] & 3) << 1 | s[2] >> 7)};
            const int c2[3] = {Extend4((s[2] & 0x78) >> 3), Extend4((s[2] & 7) << 1 | s[3] >> 7),
                               Extend4((s[3] & 0x78) >> 3)};
            const int v1 = c1[0] << 16 | c1[1] << 8 | c1[2], v2 = c2[0] << 16 | c2[1] << 8 | c2[2];
            const int d = kDistances[(s[3] & 4) | (s[3] & 1) << 1 | (v1 >= v2 ? 1 : 0)];
            const int paint[4][3] = {{c1[0] + d, c1[1] + d, c1[2] + d},
                                     {c1[0] - d, c1[1] - d, c1[2] - d},
                                     {c2[0] + d, c2[1] + d, c2[2] + d},
                                     {c2[0] - d, c2[1] - d, c2[2] - d}};
            for (int x = 0; x < 4; ++x)
                for (int y = 0; y < 4; ++y) {
                    const int i = index(x, y);
                    put(x, y, paint[i][0], paint[i][1], paint[i][2], i);
                }
            return;
        }
        if (b2 < 0 || b2 > 31) {  // planar mode (always opaque)
            const int o[3] = {Extend6((s[0] & 0x7e) >> 1), Extend7((s[0] & 1) << 6 | (s[1] & 0x7e) >> 1),
                              Extend6((s[1] & 1) << 5 | (s[2] & 0x18) | (s[2] & 3) << 1 | s[3] >> 7)};
            const int h[3] = {Extend6((s[3] & 0x7c) >> 1 | (s[3] & 1)), Extend7(s[4] >> 1),
                              Extend6((s[4] & 1) << 5 | s[5] >> 3)};
            const int v[3] = {Extend6((s[5] & 7) << 3 | s[6] >> 5), Extend7((s[6] & 0x1f) << 2 | s[7] >> 6),
                              Extend6(s[7] & 0x3f)};
            for (int x = 0; x < 4; ++x)
                for (int y = 0; y < 4; ++y) {
                    uint8_t* p = out[y * 4 + x];
                    for (int c = 0; c < 3; ++c)
                        p[c] = Clamp8((x * (h[c] - o[c]) + y * (v[c] - o[c]) + 4 * o[c] + 2) >> 2);
                    p[3] = 255;
                }
            return;
        }
        base[0][0] = Extend5(r), base[0][1] = Extend5(g), base[0][2] = Extend5(b);  // differential mode
        base[1][0] = Extend5(r2), base[1][1] = Extend5(g2), base[1][2] = Extend5(b2);
    }
    const int tables[2] = {s[3] >> 5, (s[3] >> 2) & 7};
    const bool flip = (s[3] & 1) != 0;
    for (int x = 0; x < 4; ++x)
        for (int y = 0; y < 4; ++y) {
            const int sub = flip ? (y >= 2) : (x >= 2);
            const int i = index(x, y);
            const int a = kEtc1Modifiers[tables[sub]][0], b = kEtc1Modifiers[tables[sub]][1];
            int m;
            switch (i) {
            case 0: m = opaque ? a : 0; break;
            case 1: m = b; break;
            case 2: m = -a; break;
            default: m = -b; break;
            }
            put(x, y, base[sub][0] + m, base[sub][1] + m, base[sub][2] + m, i);
        }
}

// The 48 EAC index bits of a block; texel (x, y) has 3 of them, from bit 47 down.
uint64_t EacBits(const uint8_t* s) {
    uint64_t bits = 0;
    for (int i = 2; i < 8; ++i)
        bits = bits << 8 | s[i];
    return bits;
}
int EacIndex(uint64_t bits, int x, int y) {
    return int((bits >> (45 - 3 * (x * 4 + y))) & 7);
}

// The EAC alpha of an ETC2 RGBA8 block into out[...][3].
void DecodeAlphaBlock(const uint8_t* s, uint8_t out[16][4]) {
    const int base = s[0], mult = s[1] >> 4;
    const int* mods = kEacModifiers[s[1] & 15];
    const uint64_t bits = EacBits(s);
    for (int x = 0; x < 4; ++x)
        for (int y = 0; y < 4; ++y)
            out[y * 4 + x][3] = Clamp8(base + mods[EacIndex(bits, x, y)] * mult);
}

// One EAC R11 block into 16-bit UNORM/SNORM values at out[(y * 4 + x) * stride].
void DecodeR11Block(const uint8_t* s, bool sign, uint16_t* out, int stride) {
    const int mult = s[1] >> 4;
    const int* mods = kEacModifiers[s[1] & 15];
    const uint64_t bits = EacBits(s);
    for (int x = 0; x < 4; ++x)
        for (int y = 0; y < 4; ++y) {
            const int m = mods[EacIndex(bits, x, y)];
            uint16_t value;
            if (sign) {
                int base = static_cast<int8_t>(s[0]);
                if (base == -128)
                    base = -127;
                const int v = std::clamp(base * 8 + (mult ? m * mult * 8 : m), -1023, 1023);
                const int mag = v < 0 ? -v : v;
                const int wide = mag << 5 | mag >> 5;
                value = static_cast<uint16_t>(static_cast<int16_t>(v < 0 ? -wide : wide));
            } else {
                const int v = std::clamp(s[0] * 8 + 4 + (mult ? m * mult * 8 : m), 0, 2047);
                value = static_cast<uint16_t>(v << 5 | v >> 6);
            }
            out[(y * 4 + x) * stride] = value;
        }
}

}  // namespace

size_t EtcBlockBytes(EtcCodec codec) {
    return codec == EtcCodec::Rgba8 || codec == EtcCodec::Rg11 ? 16 : 8;
}

size_t EtcTexelBytes(EtcCodec codec) {
    return codec == EtcCodec::R11 ? 2 : 4;
}

void DecodeEtcImage(EtcCodec codec, bool sign, const uint8_t* src, uint32_t row_blocks, uint32_t w, uint32_t h,
                    uint8_t* dst) {
    const size_t block_bytes = EtcBlockBytes(codec), texel_bytes = EtcTexelBytes(codec);
    const uint32_t bx_count = (w + 3) / 4, by_count = (h + 3) / 4;
    alignas(4) uint8_t texels[16][4];  // RGBA8, or 16-bit R/RG values for EAC
    for (uint32_t by = 0; by < by_count; ++by)
        for (uint32_t bx = 0; bx < bx_count; ++bx) {
            const uint8_t* s = src + (size_t(by) * row_blocks + bx) * block_bytes;
            switch (codec) {
            case EtcCodec::Rgb8: DecodeColorBlock(s, false, texels); break;
            case EtcCodec::Rgb8A1: DecodeColorBlock(s, true, texels); break;
            case EtcCodec::Rgba8:
                DecodeColorBlock(s + 8, false, texels);
                DecodeAlphaBlock(s, texels);
                break;
            case EtcCodec::R11: DecodeR11Block(s, sign, reinterpret_cast<uint16_t*>(texels), 1); break;
            case EtcCodec::Rg11:
                DecodeR11Block(s, sign, reinterpret_cast<uint16_t*>(texels), 2);
                DecodeR11Block(s + 8, sign, reinterpret_cast<uint16_t*>(texels) + 1, 2);
                break;
            }
            const uint32_t cols = std::min(4u, w - bx * 4), rows = std::min(4u, h - by * 4);
            for (uint32_t y = 0; y < rows; ++y)
                memcpy(dst + ((size_t(by) * 4 + y) * w + bx * 4) * texel_bytes,
                       reinterpret_cast<const uint8_t*>(texels) + y * 4 * texel_bytes, cols * texel_bytes);
        }
}

size_t BcBlockBytes(EtcCodec codec) {
    return codec == EtcCodec::Rgb8 ? 8 : 16;
}

void TranscodeEtcToBc(EtcCodec codec, const uint8_t* src, uint32_t row_blocks, uint32_t w, uint32_t h, uint8_t* dst) {
    const size_t in_bytes = EtcBlockBytes(codec), out_bytes = BcBlockBytes(codec);
    const uint32_t bx_count = (w + 3) / 4, by_count = (h + 3) / 4;
    alignas(4) uint8_t texels[16][4];
    for (uint32_t by = 0; by < by_count; ++by)
        for (uint32_t bx = 0; bx < bx_count; ++bx) {
            const uint8_t* s = src + (size_t(by) * row_blocks + bx) * in_bytes;
            uint8_t* d = dst + (size_t(by) * bx_count + bx) * out_bytes;
            switch (codec) {
            case EtcCodec::Rgb8:
                DecodeColorBlock(s, false, texels);
                EncodeBc1(texels, d);
                break;
            case EtcCodec::Rgb8A1:
                DecodeColorBlock(s, true, texels);
                EncodeBc3(texels, d);
                break;
            case EtcCodec::Rgba8:
                DecodeColorBlock(s + 8, false, texels);
                DecodeAlphaBlock(s, texels);
                EncodeBc3(texels, d);
                break;
            default: break;  // EAC is decoded, not transcoded
            }
        }
}

}  // namespace rn
