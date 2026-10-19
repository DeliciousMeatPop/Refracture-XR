#pragma once

#include <cstdint>

namespace rn {

// BC1/BC3 block encoding for transcoding Android texture formats to ones desktop GPUs sample natively.
// Texels are RGBA8, row-major (texel y * 4 + x).
void EncodeBc1(const uint8_t texels[16][4], uint8_t out[8]);   // opaque colour (4-colour mode)
void EncodeBc3(const uint8_t texels[16][4], uint8_t out[16]);  // BC4-style alpha block + BC1 colour block

}  // namespace rn
