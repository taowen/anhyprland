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
// Follow the Android display's current mode without forcing a device policy.
int anhyprland_refresh_rate(int millihertz);
int anhyprland_pointer(float x, float y, uint32_t button, int pressed);
// Relative pixels; a button event does not warp the pointer. pressed < 0 means motion only.
int  anhyprland_relative_pointer(float dx, float dy, int button, int pressed);
int  anhyprland_open_apps(void);
void anhyprland_configure_input_focus(void (*callback)(int), int (*activation)(int));
int  anhyprland_permit_activation(int pid);
void anhyprland_report_input_focus(int pid);
// Host controls and fullscreen are mutually exclusive. Callback runs on the compositor thread.
void anhyprland_configure_controls(int right_padding, void (*visibility)(int));
int  anhyprland_controls_visible(int visible);
int  anhyprland_fullscreen(void);
int  anhyprland_controls_right_padding(void);
// Output-pixel bounds published by the rendered shell; safe to query from Android.
void anhyprland_report_taskbar(float x, float y, float width, float height);
int  anhyprland_taskbar_contains(float x, float y);
void anhyprland_hide_controls(void);
int  anhyprland_cursor_shape(void);
// Packed signed x/y output coordinates (high/low 32 bits), UINT64_MAX before first publication.
uint64_t anhyprland_cursor_position(void);
void     anhyprland_report_cursor_position(int32_t x, int32_t y);
void     anhyprland_report_cursor_shape(uint32_t shape);
int      anhyprland_pointer_locked(void);
void     anhyprland_report_pointer_lock(int acquired);
int      anhyprland_axis(float dx, float dy);
// Touch contacts in output pixels: 0=down, 1=up, 2=move, 3=cancel.
int anhyprland_touch(int id, int action, float x, float y);
int anhyprland_key(uint32_t evdev, int pressed);
// Type a Unicode character available in the active layout (base or Shift level).
int anhyprland_unicode(uint32_t codepoint);
int anhyprland_stop(void);

#ifdef __cplusplus
}
#endif
