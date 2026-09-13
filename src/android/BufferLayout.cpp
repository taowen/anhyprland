#ifdef __ANDROID__
#include "BufferLayout.hpp"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnullability-extension"
#include "../../android/deps/imapper/IMapper.h"
#pragma clang diagnostic pop

#include <android/hardware_buffer.h>
#include <android/log.h>
#include <dlfcn.h>
#include <link.h>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct SDlCloser {
    void operator()(void* library) const {
        if (library)
            dlclose(library);
    }
};

// AHardwareBuffer_allocate has already loaded the device's configured mapper.
// Reuse that exact HAL through Android's stable C interface, without probing
// vendor handle fields or loading a guessed, device-specific mapper library.
class CBufferMapper {
  public:
    CBufferMapper() : m_nativeWindow(dlopen("libnativewindow.so", RTLD_NOW | RTLD_LOCAL)) {
        if (!m_nativeWindow)
            return;
        // libnativewindow's dependency graph already owns the platform mapper
        // loader. Directly opening its private libvndksupport dependency is not
        // permitted from an APK's classloader namespace.
        const auto load = reinterpret_cast<void* (*)(const char*, int)>(dlsym(m_nativeWindow.get(), "android_load_sphal_library"));
        m_exportHandle  = reinterpret_cast<buffer_handle_t (*)(const AHardwareBuffer*)>(dlsym(m_nativeWindow.get(), "AHardwareBuffer_getNativeHandle"));
        if (!load || !m_exportHandle) {
            __android_log_print(ANDROID_LOG_DEBUG, "AndroidBufferLayout", "Stable mapper access unavailable");
            return;
        }

        std::vector<std::string> candidates;
        dl_iterate_phdr(
            [](dl_phdr_info* info, size_t, void* data) {
                const std::string_view path{info->dlpi_name};
                const auto             name = path.substr(path.find_last_of('/') + 1);
                if (name.starts_with("mapper.") && name.ends_with(".so"))
                    static_cast<std::vector<std::string>*>(data)->emplace_back(path);
                return 0;
            },
            &candidates);
        // Multiple providers would make the buffer's owner ambiguous.
        if (candidates.size() != 1) {
            __android_log_print(ANDROID_LOG_DEBUG, "AndroidBufferLayout", "Loaded stable mapper candidates: %zu", candidates.size());
            return;
        }
        m_hal.reset(load(candidates.front().c_str(), RTLD_NOW | RTLD_NOLOAD));
        if (!m_hal) {
            __android_log_print(ANDROID_LOG_DEBUG, "AndroidBufferLayout", "Cannot reopen loaded mapper: %s", dlerror());
            return;
        }
        const auto get = reinterpret_cast<AIMapper_Error (*)(AIMapper**)>(dlsym(m_hal.get(), "AIMapper_loadIMapper"));
        if (!get || get(&m_mapper) != AIMAPPER_ERROR_NONE || !m_mapper || m_mapper->version < AIMAPPER_VERSION_5)
            m_mapper = nullptr;
        __android_log_print(ANDROID_LOG_DEBUG, "AndroidBufferLayout", "Mapper %s version %u", candidates.front().c_str(), m_mapper ? m_mapper->version : 0);
    }

    std::optional<SAndroidLinearLayout> layout(AHardwareBuffer* buffer) const {
        if (!m_mapper)
            return std::nullopt;
        AHardwareBuffer_Desc desc{};
        AHardwareBuffer_describe(buffer, &desc);
        const auto handle = m_exportHandle(buffer);
        if (!handle || handle->numFds < 1 || desc.layers != 1)
            return std::nullopt;
        return decodeAndroidLinearLayout(metadata(handle, 7), metadata(handle, 8), metadata(handle, 10), metadata(handle, 15), desc.width, desc.height);
    }

  private:
    std::vector<uint8_t> metadata(buffer_handle_t handle, int64_t type) const {
        const auto size = m_mapper->v5.getStandardMetadata(handle, type, nullptr, 0);
        if (size <= 0 || size > 4096)
            return {};
        std::vector<uint8_t> bytes(size);
        if (m_mapper->v5.getStandardMetadata(handle, type, bytes.data(), bytes.size()) != size)
            return {};
        return bytes;
    }

    std::unique_ptr<void, SDlCloser> m_nativeWindow;
    std::unique_ptr<void, SDlCloser> m_hal;
    buffer_handle_t (*m_exportHandle)(const AHardwareBuffer*) = nullptr;
    AIMapper* m_mapper                                        = nullptr;
};

std::optional<SAndroidLinearLayout> androidBufferLinearLayout(AHardwareBuffer* buffer) {
    static const CBufferMapper MAPPER;
    return MAPPER.layout(buffer);
}
#endif
