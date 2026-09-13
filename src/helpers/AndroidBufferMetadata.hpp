#pragma once

#include <cstdint>
#include <optional>
#include <span>

struct SAndroidLinearLayout {
    uint32_t drmFormat = 0;
    uint32_t stride    = 0;
};

// Decode IMapper's standard metadata wire format. Only a single, uncompressed
// 32-bit RGB plane at offset zero is eligible for the current client import.
std::optional<SAndroidLinearLayout> decodeAndroidLinearLayout(std::span<const uint8_t> format, std::span<const uint8_t> modifier, std::span<const uint8_t> allocationSize,
                                                              std::span<const uint8_t> planes, uint32_t width, uint32_t height);
