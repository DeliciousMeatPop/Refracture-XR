#pragma once

#include <cstddef>
#include <cstdint>

namespace rn {

// ETC2/EAC block decoding (OpenGL ES 3.0 spec, appendix C.1) for hosts without ETC2.
enum class EtcCodec {
    Rgb8,    // ETC2 RGB8 (and ETC1)          -> RGBA8
    Rgb8A1,  // ETC2 RGB8 punch-through alpha -> RGBA8
    Rgba8,   // ETC2 RGBA8 (EAC alpha + RGB)  -> RGBA8
    R11,     // EAC R11                       -> R16 (UNORM or SNORM)
    Rg11,    // EAC RG11                      -> R16G16 (UNORM or SNORM)
};

size_t EtcBlockBytes(EtcCodec codec);
size_t EtcTexelBytes(EtcCodec codec);

// Decodes a w x h region whose 4x4 blocks start at src, row_blocks blocks per row in the source, into
// tightly packed texels at dst. sign selects SNORM output for R11/RG11.
void DecodeEtcImage(EtcCodec codec, bool sign, const uint8_t* src, uint32_t row_blocks, uint32_t w, uint32_t h,
                    uint8_t* dst);

// Transcodes the ETC2 colour formats block for block into BC (Rgb8 -> BC1, Rgb8A1 and Rgba8 -> BC3), which
// keeps them as small as the source on GPUs without ETC2. Writes tightly packed blocks ((w + 3) / 4 per row).
size_t BcBlockBytes(EtcCodec codec);
void TranscodeEtcToBc(EtcCodec codec, const uint8_t* src, uint32_t row_blocks, uint32_t w, uint32_t h, uint8_t* dst);

}  // namespace rn
