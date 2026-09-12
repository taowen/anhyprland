#include <aquamarine/backend/Android.hpp>
#include <aquamarine/allocator/Swapchain.hpp>
#include <android/hardware_buffer.h>
#include <algorithm>
#include <cmath>
#include <ctime>
#include <sys/timerfd.h>
#include <unistd.h>

using namespace Aquamarine;
using namespace Hyprutils::Memory;
using namespace Hyprutils::Math;

static uint32_t timeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

AHardwareBuffer* IBuffer::androidBuffer() {
    return nullptr;
}

class CAndroidBuffer : public IBuffer {
  public:
    explicit CAndroidBuffer(const SAllocatorBufferParams& params) {
        size = params.size;
        if (size.x <= 0 || size.y <= 0 || size.x > 16384 || size.y > 16384)
            return;
        if (params.format != DRM_FORMAT_INVALID && params.format != DRM_FORMAT_ABGR8888 && params.format != DRM_FORMAT_XBGR8888)
            return;
        m_format = params.format == DRM_FORMAT_INVALID ? DRM_FORMAT_ABGR8888 : params.format;
        opaque = m_format == DRM_FORMAT_XBGR8888;
        AHardwareBuffer_Desc desc = {};
        desc.width = size.x;
        desc.height = size.y;
        desc.layers = 1;
        desc.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
        desc.usage = AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;
        if (AHardwareBuffer_allocate(&desc, &m_buffer) != 0)
            m_buffer = nullptr;
    }
    ~CAndroidBuffer() override {
        events.destroy.emit();
        if (m_buffer)
            AHardwareBuffer_release(m_buffer);
    }
    eBufferCapability caps() override { return BUFFER_CAPABILITY_NONE; }
    eBufferType type() override { return BUFFER_TYPE_MISC; }
    void update(const CRegion&) override { ; }
    bool isSynchronous() override { return false; }
    bool good() override { return m_buffer != nullptr; }
    // Format metadata is useful to swapchain consumers, but this buffer does
    // not advertise a Linux DMA-BUF: no DRM FD or modifier is fabricated.
    SDMABUFAttrs dmabuf() override { return {.success = false, .size = size, .format = m_format}; }
    AHardwareBuffer* androidBuffer() override { return m_buffer; }
  private:
    AHardwareBuffer* m_buffer = nullptr;
    uint32_t m_format = DRM_FORMAT_INVALID;
};

class CAndroidAllocator : public IAllocator {
  public:
    explicit CAndroidAllocator(CWeakPointer<CBackend> backend) : m_backend(backend) { ; }
    CSharedPointer<IBuffer> acquire(const SAllocatorBufferParams& params, CSharedPointer<CSwapchain>) override {
        auto buffer = makeShared<CAndroidBuffer>(params);
        if (!buffer->good())
            return nullptr;
        return buffer;
    }
    CSharedPointer<CBackend> getBackend() override { return m_backend.lock(); }
    int drmFD() override { return -1; }
    eAllocatorType type() override { return AQ_ALLOCATOR_TYPE_ANDROID; }
  private:
    CWeakPointer<CBackend> m_backend;
};

CAndroidOutput::CAndroidOutput(CWeakPointer<CAndroidBackend> backend) : m_backend(backend) {
    name = "ANDROID-1";
    description = "Android Surface";
    make = "Android";
    model = "Embedded display";
}

bool CAndroidOutput::test() {
    const auto& pending = state->state();
    if (pending.drmFormat != DRM_FORMAT_INVALID && pending.drmFormat != DRM_FORMAT_ABGR8888 && pending.drmFormat != DRM_FORMAT_XBGR8888)
        return false;
    return !pending.buffer || pending.buffer->androidBuffer();
}

bool CAndroidOutput::commit() {
    if (!test())
        return false;
    const auto snapshot = state->snapshot();
    if (snapshot.error())
        return false;
    const auto& pending = snapshot.state();
    bool submitted = false;
    if ((pending.committed & COutputState::AQ_OUTPUT_STATE_BUFFER) && pending.buffer && m_backend->window()) {
        if (!m_backend->present || !m_backend->present(pending.buffer))
            return false;
        submitted = true;
    }
    enabled = pending.enabled;
    events.commit.emit();
    state->consume(snapshot);
    needsFrame = false;
    if (pending.committed & COutputState::AQ_OUTPUT_STATE_BUFFER) {
        timespec now = {};
        clock_gettime(CLOCK_MONOTONIC, &now);
        events.present.emit(SPresentEvent{.presented = submitted, .when = &now, .refresh = 16666667});
    }
    return true;
}

CSharedPointer<IBackendImplementation> CAndroidOutput::getBackend() { return m_backend.lock(); }
std::vector<SDRMFormat> CAndroidOutput::getRenderFormats() { return m_backend->getRenderFormats(); }
void CAndroidOutput::scheduleFrame(scheduleFrameReason) { needsFrame = true; m_backend->scheduleFrame(); }
bool CAndroidOutput::pendingPageFlip() { return false; }
bool CAndroidOutput::pendingIdleFrame() { return m_backend->frameScheduled(); }
const std::string& CAndroidKeyboard::getName() { return m_name; }
const std::string& CAndroidPointer::getName() { return m_name; }

CAndroidBackend::CAndroidBackend(CWeakPointer<CBackend> backend, ANativeWindow* window, int width, int height) :
    m_backend(backend), m_window(window), m_width(width), m_height(height) {
    if (m_window)
        ANativeWindow_acquire(m_window);
    m_timer = Hyprutils::OS::CFileDescriptor{timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK)};
    m_lastFrame = std::chrono::steady_clock::now();
}

CAndroidBackend::~CAndroidBackend() {
    if (m_window)
        ANativeWindow_release(m_window);
}

eBackendType CAndroidBackend::type() { return AQ_BACKEND_ANDROID; }
bool CAndroidBackend::start() {
    if (!m_window || !m_timer.isValid() || m_width <= 0 || m_height <= 0)
        return false;
    m_allocator = makeShared<CAndroidAllocator>(m_backend);
    return true;
}

std::vector<CSharedPointer<SPollFD>> CAndroidBackend::pollFDs() {
    return {makeShared<SPollFD>(m_timer.get(), [this]() { dispatchEvents(); })};
}
int CAndroidBackend::drmFD() { return -1; }
int CAndroidBackend::drmRenderNodeFD() { return -1; }
uint32_t CAndroidBackend::capabilities() { return AQ_BACKEND_CAPABILITY_POINTER; }
bool CAndroidBackend::dispatchEvents() {
    uint64_t count = 0;
    if (read(m_timer.get(), &count, sizeof(count)) != sizeof(count))
        return true;
    m_frameScheduled = false;
    if (m_window && m_output) {
        m_lastFrame = std::chrono::steady_clock::now();
        m_output->events.frame.emit();
    }
    return true;
}

void CAndroidBackend::onReady() {
    createOutput();
    m_keyboard = makeShared<CAndroidKeyboard>();
    m_pointer = makeShared<CAndroidPointer>();
    m_backend->events.newKeyboard.emit(m_keyboard);
    m_backend->events.newPointer.emit(m_pointer);
}

std::vector<SDRMFormat> CAndroidBackend::getRenderFormats() {
    return {{.drmFormat = DRM_FORMAT_ABGR8888, .modifiers = {DRM_FORMAT_MOD_INVALID}},
            {.drmFormat = DRM_FORMAT_XBGR8888, .modifiers = {DRM_FORMAT_MOD_INVALID}}};
}
std::vector<SDRMFormat> CAndroidBackend::getCursorFormats() { return {}; }
bool CAndroidBackend::createOutput(const std::string&) {
    if (m_output)
        return false;
    m_output = makeShared<CAndroidOutput>(self);
    m_output->modes.emplace_back(makeShared<SOutputMode>(Vector2D{m_width, m_height}, 60000, true));
    m_output->swapchain = CSwapchain::create(m_allocator, self.lock());
    m_backend->events.newOutput.emit(m_output);
    return true;
}
CSharedPointer<IAllocator> CAndroidBackend::preferredAllocator() { return m_allocator; }
std::vector<CSharedPointer<IAllocator>> CAndroidBackend::getAllocators() { return {m_allocator}; }
CWeakPointer<IBackendImplementation> CAndroidBackend::getPrimary() { return {}; }
ANativeWindow* CAndroidBackend::window() const { return m_window; }
bool CAndroidBackend::frameScheduled() const { return m_frameScheduled; }

bool CAndroidBackend::setWindow(ANativeWindow* window, int width, int height) {
    if (window && (width <= 0 || height <= 0))
        return false;
    if (attachWindow && !attachWindow(window))
        return false;
    if (window)
        ANativeWindow_acquire(window);
    if (m_window)
        ANativeWindow_release(m_window);
    m_window = window;
    if (!window) {
        itimerspec disarm = {};
        timerfd_settime(m_timer.get(), 0, &disarm, nullptr);
        m_frameScheduled = false;
        return true;
    }
    const bool resized = width != m_width || height != m_height;
    m_width = width;
    m_height = height;
    if (resized && m_output)
        m_output->events.state.emit(IOutput::SStateEvent{.size = {width, height}});
    scheduleFrame();
    return true;
}

void CAndroidBackend::scheduleFrame() {
    if (!m_window || m_frameScheduled)
        return;
    const auto next = std::max(m_lastFrame + std::chrono::nanoseconds(16666667), std::chrono::steady_clock::now() + std::chrono::nanoseconds(1));
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(next.time_since_epoch()).count();
    itimerspec timer = {.it_value = {.tv_sec = ns / 1000000000, .tv_nsec = ns % 1000000000}};
    m_frameScheduled = timerfd_settime(m_timer.get(), TFD_TIMER_ABSTIME, &timer, nullptr) == 0;
}

void CAndroidBackend::pointer(float x, float y, uint32_t button, bool pressed) {
    if (!m_pointer || !std::isfinite(x) || !std::isfinite(y))
        return;
    m_pointer->events.warp.emit(IPointer::SWarpEvent{.timeMs = timeMs(), .absolute = {x / m_width, y / m_height}, .output = m_output});
    if (button)
        m_pointer->events.button.emit(IPointer::SButtonEvent{.timeMs = timeMs(), .button = button, .pressed = pressed});
    m_pointer->events.frame.emit();
}

void CAndroidBackend::axis(float dx, float dy) {
    if (!m_pointer || !std::isfinite(dx) || !std::isfinite(dy))
        return;
    if (dx)
        m_pointer->events.axis.emit(IPointer::SAxisEvent{.timeMs = timeMs(), .axis = IPointer::AQ_POINTER_AXIS_HORIZONTAL, .delta = dx});
    if (dy)
        m_pointer->events.axis.emit(IPointer::SAxisEvent{.timeMs = timeMs(), .axis = IPointer::AQ_POINTER_AXIS_VERTICAL, .delta = dy});
    m_pointer->events.frame.emit();
}

void CAndroidBackend::key(uint32_t evdev, bool pressed) {
    if (m_keyboard)
        m_keyboard->events.key.emit(IKeyboard::SKeyEvent{.timeMs = timeMs(), .key = evdev, .pressed = pressed});
}
