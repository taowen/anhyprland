#ifdef __ANDROID__
#include "AndroidWlegl.hpp"
#include "wayland-android.hpp"
#include "types/Buffer.hpp"
#include "../render/OpenGL.hpp"
#include "../render/gl/GLTexture.hpp"
#include <android/hardware_buffer.h>
#include <dlfcn.h>
#include <cstring>

// Native handle ABI used by Android's gralloc transport. The platform clones
// imported descriptors; the Wayland handle retains ownership of received FDs.
struct SNativeHandle {
    int version, numFds, numInts;
};
using FImportHandle = int (*)(const AHardwareBuffer_Desc*, const SNativeHandle*, int32_t, AHardwareBuffer**);
using FExportHandle = const SNativeHandle* (*)(const AHardwareBuffer*);

struct SAndroidBufferDeleter {
    void operator()(AHardwareBuffer* buffer) const {
        if (buffer)
            AHardwareBuffer_release(buffer);
    }
};
using AHB = std::unique_ptr<AHardwareBuffer, SAndroidBufferDeleter>;

static uint32_t drmFormat(uint32_t format) {
    switch (format) {
        case AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM: return DRM_FORMAT_ABGR8888;
        case AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM: return DRM_FORMAT_XBGR8888;
        case AHARDWAREBUFFER_FORMAT_R5G6B5_UNORM: return DRM_FORMAT_RGB565;
        case 5: return DRM_FORMAT_ARGB8888; // Android HAL_PIXEL_FORMAT_BGRA_8888
        default: return DRM_FORMAT_INVALID;
    }
}

class CAndroidClientBuffer : public IHLBuffer {
  public:
    CAndroidClientBuffer(wl_client* client, uint32_t id, AHB buffer) : m_buffer(std::move(buffer)) {
        AHardwareBuffer_Desc desc{};
        AHardwareBuffer_describe(m_buffer.get(), &desc);
        size              = Vector2D{sc<double>(desc.width), sc<double>(desc.height)};
        const auto format = drmFormat(desc.format);
        m_opaque          = format == DRM_FORMAT_XBGR8888 || format == DRM_FORMAT_RGB565;
        Render::GL::g_pHyprOpenGL->makeEGLCurrent();
        const auto image = Render::GL::g_pHyprOpenGL->createAndroidImage(m_buffer.get());
        if (image == EGL_NO_IMAGE_KHR)
            return;
        Aquamarine::SDMABUFAttrs metadata{.success = false, .size = size, .format = format};
        m_texture  = makeShared<Render::GL::CGLTexture>(metadata, image, m_opaque);
        m_resource = CWLBufferResource::create(makeShared<CWlBuffer>(client, 1, id));
    }
    ~CAndroidClientBuffer() override {
        // EGLImage/texture references must go before the allocation reference.
        m_texture.reset();
    }
    Aquamarine::eBufferCapability caps() override {
        return Aquamarine::BUFFER_CAPABILITY_NONE;
    }
    Aquamarine::eBufferType type() override {
        return Aquamarine::BUFFER_TYPE_MISC;
    }
    bool isSynchronous() override {
        return false;
    }
    void update(const CRegion&) override {
        ;
    }
    bool good() override {
        return m_resource && m_resource->good() && m_texture && m_texture->ok();
    }
    AHardwareBuffer* androidBuffer() override {
        return m_buffer.get();
    }
    void sendRelease() override {
        // android_wlegl carries no release fence. Complete sampling before the
        // producer receives wl_buffer.release and writes this allocation again.
        Render::GL::g_pHyprOpenGL->makeEGLCurrent();
        glFinish();
        if (m_resource && m_resource->good())
            IHLBuffer::sendRelease();
    }

  private:
    AHB m_buffer;
};

struct CAndroidWleglProtocol::SState {
    struct SHandle {
        UP<CAndroidWleglHandle>                     resource;
        int                                         expectedFds = 0;
        std::vector<Hyprutils::OS::CFileDescriptor> fds;
        std::vector<int32_t>                        ints;
    };
    struct SBuffer {
        SP<CAndroidClientBuffer> buffer;
        CHyprSignalListener      destroy;
    };
    std::vector<UP<CAndroidWlegl>> managers;
    std::vector<UP<SHandle>>       handles;
    std::vector<UP<SBuffer>>       buffers;
    void*                          nativeWindow = dlopen("libnativewindow.so", RTLD_NOW | RTLD_LOCAL);
    FImportHandle                  importHandle = nativeWindow ? reinterpret_cast<FImportHandle>(dlsym(nativeWindow, "AHardwareBuffer_createFromHandle")) : nullptr;
    FExportHandle                  exportHandle = nativeWindow ? reinterpret_cast<FExportHandle>(dlsym(nativeWindow, "AHardwareBuffer_getNativeHandle")) : nullptr;

    ~SState() {
        buffers.clear();
        handles.clear();
        managers.clear();
        if (nativeWindow)
            dlclose(nativeWindow);
    }

    SP<CAndroidClientBuffer> addBuffer(wl_client* client, uint32_t id, AHB allocation) {
        auto resource    = makeUnique<SBuffer>();
        resource->buffer = makeShared<CAndroidClientBuffer>(client, id, std::move(allocation));
        auto buffer      = resource->buffer;
        if (!buffer->good())
            return nullptr;
        buffer->m_resource->m_buffer = buffer;
        resource->destroy = buffer->events.destroy.listen([this, ptr = buffer.get()] { std::erase_if(buffers, [ptr](const auto& entry) { return entry->buffer.get() == ptr; }); });
        buffers.emplace_back(std::move(resource));
        return buffer;
    }

    static bool validDimensions(int32_t width, int32_t height, int32_t format) {
        return width > 0 && height > 0 && width <= 16384 && height <= 16384 && drmFormat(format) != DRM_FORMAT_INVALID;
    }

    void createHandle(CAndroidWlegl* manager, uint32_t id, int32_t count, wl_array* data) {
        if (count < 0 || count > 64 || data->size % sizeof(int32_t) || data->size > 4096) {
            manager->error(ANDROID_WLEGL_ERROR_BAD_VALUE, "Invalid native handle size");
            return;
        }
        auto handle      = makeUnique<SHandle>();
        handle->resource = makeUnique<CAndroidWleglHandle>(manager->client(), 1, id);
        if (!handle->resource->resource()) {
            manager->noMemory();
            return;
        }
        handle->expectedFds = count;
        handle->ints.resize(data->size / sizeof(int32_t));
        if (data->size)
            std::memcpy(handle->ints.data(), data->data, data->size);
        auto ptr = handle.get();
        handle->resource->setAddFd([ptr](CAndroidWleglHandle* resource, int32_t fd) {
            Hyprutils::OS::CFileDescriptor received{fd};
            if (ptr->fds.size() >= sc<size_t>(ptr->expectedFds)) {
                resource->error(ANDROID_WLEGL_HANDLE_ERROR_TOO_MANY_FDS, "Too many native handle FDs");
                return;
            }
            ptr->fds.emplace_back(std::move(received));
        });
        handle->resource->setDestroy([this, ptr](CAndroidWleglHandle*) { removeHandle(ptr); });
        handle->resource->setOnDestroy([this, ptr](CAndroidWleglHandle*) { removeHandle(ptr); });
        handles.emplace_back(std::move(handle));
    }

    void removeHandle(SHandle* handle) {
        std::erase_if(handles, [handle](const auto& entry) { return entry.get() == handle; });
    }

    void importBuffer(CAndroidWlegl* manager, uint32_t id, int32_t width, int32_t height, int32_t stride, int32_t format, int32_t usage, wl_resource* resource) {
        const auto found = std::ranges::find_if(handles, [resource](const auto& handle) { return handle->resource->resource() == resource; });
        if (!importHandle || found == handles.end() || !validDimensions(width, height, format) || stride < width || (*found)->fds.size() != sc<size_t>((*found)->expectedFds)) {
            manager->error(ANDROID_WLEGL_ERROR_BAD_HANDLE, "Invalid Android buffer handle or dimensions");
            return;
        }
        const auto&          handle = **found;
        std::vector<int32_t> serialized(3 + handle.fds.size() + handle.ints.size());
        serialized[0] = sizeof(SNativeHandle);
        serialized[1] = handle.fds.size();
        serialized[2] = handle.ints.size();
        for (size_t i = 0; i < handle.fds.size(); ++i)
            serialized[3 + i] = handle.fds[i].get();
        std::ranges::copy(handle.ints, serialized.begin() + 3 + handle.fds.size());
        AHardwareBuffer_Desc desc{.width  = sc<uint32_t>(width),
                                  .height = sc<uint32_t>(height),
                                  .layers = 1,
                                  .format = sc<uint32_t>(format),
                                  .usage  = sc<uint32_t>(usage),
                                  .stride = sc<uint32_t>(stride)};
        AHardwareBuffer*     raw    = nullptr;
        const auto           result = importHandle(&desc, reinterpret_cast<const SNativeHandle*>(serialized.data()), 3 /* CLONE */, &raw);
        AHB                  allocation{raw};
        if (result || !allocation || !addBuffer(manager->client(), id, std::move(allocation)))
            manager->error(ANDROID_WLEGL_ERROR_BAD_HANDLE, "Cannot import Android buffer");
    }

    void allocateBuffer(CAndroidWlegl* manager, uint32_t id, int32_t width, int32_t height, int32_t format, int32_t usage) {
        if (!exportHandle || !validDimensions(width, height, format)) {
            manager->error(ANDROID_WLEGL_ERROR_BAD_VALUE, "Unsupported Android allocation");
            return;
        }
        AHardwareBuffer_Desc desc{.width = sc<uint32_t>(width), .height = sc<uint32_t>(height), .layers = 1, .format = sc<uint32_t>(format), .usage = sc<uint32_t>(usage)};
        AHardwareBuffer*     raw    = nullptr;
        const auto           result = AHardwareBuffer_allocate(&desc, &raw);
        AHB                  allocation{raw};
        if (result || !allocation) {
            manager->error(ANDROID_WLEGL_ERROR_BAD_VALUE, "Android allocation failed");
            return;
        }
        const auto handle = exportHandle(raw);
        if (!handle || handle->numFds < 0 || handle->numInts < 0 || handle->numFds > 64 || handle->numInts > 1024) {
            manager->error(ANDROID_WLEGL_ERROR_BAD_HANDLE, "Invalid allocated native handle");
            return;
        }
        auto reply = makeUnique<CAndroidWleglServerBufferHandle>(manager->client(), 1, id);
        if (!reply->resource()) {
            manager->noMemory();
            return;
        }
        auto buffer = addBuffer(manager->client(), 0, std::move(allocation));
        if (!buffer) {
            manager->error(ANDROID_WLEGL_ERROR_BAD_HANDLE, "Cannot import allocated buffer into EGL");
            return;
        }
        AHardwareBuffer_describe(raw, &desc);
        const auto payload = reinterpret_cast<const int32_t*>(handle + 1);
        for (int i = 0; i < handle->numFds; ++i)
            reply->sendBufferFd(payload[i]);
        wl_array ints{.size = sc<size_t>(handle->numInts) * sizeof(int32_t), .alloc = 0, .data = const_cast<int32_t*>(payload + handle->numFds)};
        reply->sendBufferInts(&ints);
        reply->sendBuffer(buffer->m_resource->getResource(), desc.format, desc.stride);
        // This reply object has no requests; all events have been queued.
    }
};

CAndroidWleglProtocol::CAndroidWleglProtocol() : IWaylandProtocol(&android_wlegl_interface, 2, "AndroidWlegl"), m_state(makeUnique<SState>()) {
    ;
}
CAndroidWleglProtocol::~CAndroidWleglProtocol() = default;

void CAndroidWleglProtocol::bindManager(wl_client* client, void*, uint32_t version, uint32_t id) {
    auto manager = makeUnique<CAndroidWlegl>(client, version, id);
    if (!manager->resource()) {
        wl_client_post_no_memory(client);
        return;
    }
    manager->setOnDestroy([this](CAndroidWlegl* resource) { std::erase_if(m_state->managers, [resource](const auto& entry) { return entry.get() == resource; }); });
    manager->setCreateHandle([this](auto... args) { m_state->createHandle(args...); });
    manager->setCreateBuffer([this](auto... args) { m_state->importBuffer(args...); });
    manager->setGetServerBufferHandle([this](auto... args) { m_state->allocateBuffer(args...); });
    m_state->managers.emplace_back(std::move(manager));
}
#endif
