#ifdef __ANDROID__
#include "Embed.h"
#include "../Compositor.hpp"
#include "../managers/SeatManager.hpp"
#include <aquamarine/backend/Android.hpp>
#include <android/native_window.h>
#include <android/log.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <cerrno>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>

struct SEmbedState {
    std::mutex                        mutex;
    std::deque<std::function<void()>> commands;
    int                               eventFd = -1;
    bool                              used    = false;
};

static SEmbedState                     embed;

static SP<Aquamarine::CAndroidBackend> backend() {
    if (!g_pCompositor || !g_pCompositor->m_aqBackend)
        return nullptr;
    for (const auto& impl : g_pCompositor->m_aqBackend->getImplementations()) {
        if (impl->type() == Aquamarine::AQ_BACKEND_ANDROID)
            return Hyprutils::Memory::dynamicPointerCast<Aquamarine::CAndroidBackend>(impl);
    }
    return nullptr;
}

static int enqueue(std::function<void()> command) {
    std::lock_guard lock(embed.mutex);
    if (embed.eventFd < 0)
        return -ENOTCONN;
    embed.commands.emplace_back(std::move(command));
    const uint64_t one = 1;
    ssize_t        result;
    do {
        result = write(embed.eventFd, &one, sizeof(one));
    } while (result < 0 && errno == EINTR);
    if (result < 0 && errno != EAGAIN) {
        embed.commands.pop_back();
        return -errno;
    }
    return 0;
}

static int dispatchCommands(int fd, uint32_t, void*) {
    uint64_t count;
    while (read(fd, &count, sizeof(count)) < 0 && errno == EINTR) {
        ;
    }
    std::deque<std::function<void()>> commands;
    {
        std::lock_guard lock(embed.mutex);
        commands.swap(embed.commands);
    }
    for (auto& command : commands) {
        if (g_pCompositor->m_isShuttingDown)
            break;
        command();
    }
    return 0;
}

class CEnvironmentRestore {
  public:
    CEnvironmentRestore() {
        for (const auto name : {"HYPRLAND_NO_RT", "WAYLAND_DISPLAY", "DISPLAY", "HYPRLAND_INSTANCE_SIGNATURE", "XDG_CURRENT_DESKTOP"}) {
            const auto value = getenv(name);
            values.emplace_back(name, value ? std::optional<std::string>(value) : std::nullopt);
        }
    }
    ~CEnvironmentRestore() {
        for (const auto& [name, value] : values) {
            if (value)
                setenv(name.c_str(), value->c_str(), 1);
            else
                unsetenv(name.c_str());
        }
    }

  private:
    std::vector<std::pair<std::string, std::optional<std::string>>> values;
};

extern "C" int anhyprland_run(ANativeWindow* window, int width, int height, const char* config, void (*ready)(void*), void* userdata) {
    if (!window || width <= 0 || height <= 0 || !config || !*config)
        return -EINVAL;
    const auto runtime = getenv("XDG_RUNTIME_DIR");
    if (!runtime || runtime[0] != '/')
        return -EINVAL;
    {
        std::lock_guard lock(embed.mutex);
        if (embed.used)
            return -EALREADY;
        embed.eventFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (embed.eventFd < 0)
            return -errno;
        embed.used = true;
    }
    ANativeWindow_acquire(window);
    CEnvironmentRestore environment;
    setenv("HYPRLAND_NO_RT", "1", 1);
    wl_event_source* commands = nullptr;
    int              result   = 0;
    try {
        g_pCompositor                       = makeUnique<CCompositor>();
        g_pCompositor->m_explicitConfigPath = config;
        g_pCompositor->m_androidWindow      = window;
        g_pCompositor->m_androidWidth       = width;
        g_pCompositor->m_androidHeight      = height;
        g_pCompositor->initServer("wayland-0", -1);
        commands = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, embed.eventFd, WL_EVENT_READABLE, dispatchCommands, nullptr);
        if (!commands)
            throw std::runtime_error("Cannot register Android command queue");
        if (ready)
            ready(userdata);
        g_pCompositor->startCompositor();
    } catch (const std::exception& error) {
        __android_log_print(ANDROID_LOG_ERROR, "anhyprland", "Compositor failed: %s", error.what());
        result = -EIO;
    }
    {
        std::lock_guard lock(embed.mutex);
        close(embed.eventFd);
        embed.eventFd = -1;
        embed.commands.clear();
    }
    if (commands)
        wl_event_source_remove(commands);
    if (g_pCompositor) {
        g_pCompositor->cleanup();
        g_pCompositor.reset();
    }
    ANativeWindow_release(window);
    return result;
}

extern "C" int anhyprland_window(ANativeWindow* window, int width, int height) {
    if (window && (width <= 0 || height <= 0))
        return -EINVAL;
    if (window)
        ANativeWindow_acquire(window);
    auto retained = std::shared_ptr<ANativeWindow>(window, [](ANativeWindow* value) {
        if (value)
            ANativeWindow_release(value);
    });
    return enqueue([retained, width, height] {
        if (auto output = backend(); output && !output->setWindow(retained.get(), width, height))
            __android_log_print(ANDROID_LOG_ERROR, "anhyprland", "Cannot attach replacement Surface");
    });
}

extern "C" int anhyprland_pointer(float x, float y, uint32_t button, int pressed) {
    return enqueue([=] {
        if (auto output = backend())
            output->pointer(x, y, button, pressed);
    });
}
extern "C" int anhyprland_axis(float dx, float dy) {
    return enqueue([=] {
        if (auto output = backend())
            output->axis(dx, dy);
    });
}
extern "C" int anhyprland_key(uint32_t evdev, int pressed) {
    return enqueue([=] {
        if (auto output = backend())
            output->key(evdev, pressed);
    });
}
extern "C" int anhyprland_unicode(uint32_t codepoint) {
    if (codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint <= 0xdfff))
        return -EINVAL;
    return enqueue([=] {
        auto output   = backend();
        auto keyboard = g_pSeatManager ? g_pSeatManager->m_keyboard.lock() : nullptr;
        if (!output || !keyboard || !keyboard->m_xkbKeymap || !keyboard->m_xkbState)
            return;
        auto       map    = keyboard->m_xkbKeymap;
        const auto layout = keyboard->getActiveLayoutIndex().value_or(0);
        const auto wanted = xkb_utf32_to_keysym(codepoint);
        if (wanted == XKB_KEY_NoSymbol)
            return;
        for (auto code = xkb_keymap_min_keycode(map); code <= xkb_keymap_max_keycode(map); ++code) {
            if (code < 8)
                continue;
            for (xkb_level_index_t level = 0; level < std::min<xkb_level_index_t>(2, xkb_keymap_num_levels_for_key(map, code, layout)); ++level) {
                const xkb_keysym_t* symbols = nullptr;
                const int           count   = xkb_keymap_key_get_syms_by_level(map, code, layout, level, &symbols);
                bool                found   = false;
                for (int i = 0; i < count; ++i)
                    found = found || symbols[i] == wanted;
                if (!found)
                    continue;
                const bool leftShift  = keyboard->getPressed(42);
                const bool rightShift = keyboard->getPressed(54);
                const bool shifted    = leftShift || rightShift;
                if (level && !shifted)
                    output->key(42, true);
                if (!level && leftShift)
                    output->key(42, false);
                if (!level && rightShift)
                    output->key(54, false);
                output->key(code - 8, true);
                output->key(code - 8, false);
                if (level && !shifted)
                    output->key(42, false);
                if (!level && leftShift)
                    output->key(42, true);
                if (!level && rightShift)
                    output->key(54, true);
                return;
            }
        }
    });
}

extern "C" int anhyprland_stop() {
    return enqueue([] { g_pCompositor->stopCompositor(); });
}
#endif
