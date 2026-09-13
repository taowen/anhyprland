#include "AndroidBufferMetadata.hpp"

#include <bit>
#include <cstring>
#include <limits>
#include <string_view>

// StandardMetadataType.aidl and IMapperMetadataTypes.h define this encoding:
// int64 string length, string bytes, int64 type, then the typed payload.
static constexpr std::string_view METADATA_NAME = "android.hardware.graphics.common.StandardMetadataType";

class CMetadataReader {
  public:
    explicit CMetadataReader(std::span<const uint8_t> bytes) : m_bytes(bytes) {
        ;
    }

    template <typename T>
    std::optional<T> integer() {
        static_assert(std::endian::native == std::endian::little);
        if (m_bytes.size() < sizeof(T))
            return std::nullopt;
        T result = {};
        std::memcpy(&result, m_bytes.data(), sizeof(T));
        m_bytes = m_bytes.subspan(sizeof(T));
        return result;
    }

    std::optional<std::string_view> string() {
        const auto size = integer<int64_t>();
        if (!size || *size < 0 || static_cast<uint64_t>(*size) > m_bytes.size())
            return std::nullopt;
        const auto text = std::string_view{reinterpret_cast<const char*>(m_bytes.data()), static_cast<size_t>(*size)};
        m_bytes         = m_bytes.subspan(*size);
        return text;
    }

    bool header(int64_t type) {
        return string() == METADATA_NAME && integer<int64_t>() == type;
    }

    bool empty() const {
        return m_bytes.empty();
    }

  private:
    std::span<const uint8_t> m_bytes;
};

template <typename T>
static std::optional<T> scalar(std::span<const uint8_t> bytes, int64_t type) {
    CMetadataReader reader{bytes};
    if (!reader.header(type))
        return std::nullopt;
    const auto value = reader.integer<T>();
    return reader.empty() ? value : std::nullopt;
}

std::optional<SAndroidLinearLayout> decodeAndroidLinearLayout(std::span<const uint8_t> formatBytes, std::span<const uint8_t> modifierBytes,
                                                              std::span<const uint8_t> allocationBytes, std::span<const uint8_t> planeBytes, uint32_t width, uint32_t height) {
    const auto format     = scalar<uint32_t>(formatBytes, 7);      // PIXEL_FORMAT_FOURCC
    const auto modifier   = scalar<uint64_t>(modifierBytes, 8);    // PIXEL_FORMAT_MODIFIER
    const auto allocation = scalar<uint64_t>(allocationBytes, 10); // ALLOCATION_SIZE
    // DRM_FORMAT_ARGB8888 / ABGR8888; these are the server's BGRA/RGBA formats.
    if (!width || !height || !format || (*format != 0x34325241 && *format != 0x34324241) || modifier != 0 || !allocation)
        return std::nullopt;

    CMetadataReader reader{planeBytes};
    if (!reader.header(15) || reader.integer<int64_t>() != 1) // PLANE_LAYOUTS
        return std::nullopt;
    const auto components = reader.integer<int64_t>();
    if (!components || *components < 1 || *components > 4)
        return std::nullopt;
    for (int64_t i = 0; i < *components; ++i) {
        if (!reader.string() || !reader.integer<int64_t>())
            return std::nullopt;
        const auto offset = reader.integer<int64_t>();
        const auto size   = reader.integer<int64_t>();
        if (!offset || !size || *offset < 0 || *offset >= 32 || *size < 1 || *size > 32 - *offset)
            return std::nullopt;
    }
    const auto offset     = reader.integer<int64_t>();
    const auto increment  = reader.integer<int64_t>();
    const auto stride     = reader.integer<int64_t>();
    const auto samplesX   = reader.integer<int64_t>();
    const auto samplesY   = reader.integer<int64_t>();
    const auto totalSize  = reader.integer<int64_t>();
    const auto horizontal = reader.integer<int64_t>();
    const auto vertical   = reader.integer<int64_t>();
    if (!reader.empty() || offset != 0 || increment != 32 || horizontal != 1 || vertical != 1 || !stride || !samplesX || !samplesY || !totalSize)
        return std::nullopt;
    if (*stride < static_cast<int64_t>(width) * 4 || *stride > std::numeric_limits<uint32_t>::max() || *stride % 4 || *samplesX < width || *samplesX > *stride / 4 ||
        *samplesY < height || *totalSize < 0 || static_cast<uint64_t>(*totalSize) > *allocation)
        return std::nullopt;
    const uint64_t required = static_cast<uint64_t>(*stride) * (height - 1) + static_cast<uint64_t>(width) * 4;
    if (required > static_cast<uint64_t>(*totalSize))
        return std::nullopt;
    return SAndroidLinearLayout{.drmFormat = *format, .stride = static_cast<uint32_t>(*stride)};
}
