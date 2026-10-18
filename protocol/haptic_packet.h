#pragma once

#include <cstdint>

namespace refract::protocol {

// Sent from the Android runtime to the host over the pose stream's reverse direction.
constexpr uint32_t kHapticPacketMagic = 0x54504852; // RHPT, little-endian.

enum HapticHand : uint8_t { HapticLeft = 0, HapticRight = 1, HapticBoth = 2 };

#pragma pack(push, 1)
struct HapticPacket {
    uint32_t magic = kHapticPacketMagic;
    uint8_t hand = HapticBoth;
    uint8_t stop = 0;       // 1: xrStopHapticFeedback.
    uint16_t reserved = 0;
    int64_t duration_ns = 0; // XrDuration: XR_MIN_HAPTIC_DURATION (-1) is the device's shortest pulse.
    float frequency = 0;     // Hz; XR_FREQUENCY_UNSPECIFIED (0) lets the device choose.
    float amplitude = 0;     // 0..1.
};
#pragma pack(pop)
static_assert(sizeof(HapticPacket) == 24, "HapticPacket is a wire format");

} // namespace refract::protocol
