#pragma once

#include <stdint.h>

struct ANativeWindow;

#ifdef __cplusplus
extern "C" {
#endif

// Run on a dedicated host thread. The host configures XDG_RUNTIME_DIR,
// ARLINUX_XWAYLAND, XKB_CONFIG_ROOT and MAGIC before calling this function.
// Currently one compositor lifetime is supported per host process.
// Returns 0 after a requested shutdown, or a negative errno on failure.
int anhyprland_run(struct ANativeWindow* window, int width, int height, const char* config, void (*ready)(void* userdata), void* userdata);
// These calls are thread-safe. Window references are retained until processed.
int anhyprland_window(struct ANativeWindow* window, int width, int height);
int anhyprland_pointer(float x, float y, uint32_t button, int pressed);
int anhyprland_axis(float dx, float dy);
int anhyprland_key(uint32_t evdev, int pressed);
int anhyprland_stop(void);

#ifdef __cplusplus
}
#endif
