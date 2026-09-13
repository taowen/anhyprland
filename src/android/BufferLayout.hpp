#pragma once

#include "../helpers/AndroidBufferMetadata.hpp"

struct AHardwareBuffer;

std::optional<SAndroidLinearLayout> androidBufferLinearLayout(AHardwareBuffer* buffer);
