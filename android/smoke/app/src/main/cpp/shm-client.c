// Small xdg-shell client used to verify the compositor's wl_shm render path.
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include "wayland-android-client-protocol.h"
struct wl_buffer*            make_ahb_buffer(struct wl_display*, struct android_wlegl*, int);
static struct android_wlegl* android;
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct wl_compositor* compositor;
static struct wl_shm*        shm;
static struct xdg_wm_base*   wm;
static struct wl_surface*    surface;
static struct wl_buffer*     buffer;
static struct wl_seat*       seat;
static int                   running = 1;
static void                  ping(void* data, struct xdg_wm_base* base, uint32_t serial) {
    xdg_wm_base_pong(base, serial);
}
static const struct xdg_wm_base_listener wm_listener = {.ping = ping};
static void                              keymap(void* data, struct wl_keyboard* keyboard, uint32_t format, int32_t fd, uint32_t size) {
    fprintf(stderr, "KEYMAP format=%u size=%u fd=%d\n", format, size, fd);
    close(fd);
}
static void key_enter(void* data, struct wl_keyboard* keyboard, uint32_t serial, struct wl_surface* surface, struct wl_array* keys) {}
static void key_leave(void* data, struct wl_keyboard* keyboard, uint32_t serial, struct wl_surface* surface) {}
static void key_event(void* data, struct wl_keyboard* keyboard, uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
    fprintf(stderr, "KEY code=%u state=%u\n", key, state);
}
static void modifiers(void* data, struct wl_keyboard* keyboard, uint32_t serial, uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group) {}
static const struct wl_keyboard_listener keyboard_listener = {.keymap = keymap, .enter = key_enter, .leave = key_leave, .key = key_event, .modifiers = modifiers};
static void                              pointer_enter(void* data, struct wl_pointer* pointer, uint32_t serial, struct wl_surface* surface, wl_fixed_t x, wl_fixed_t y) {
    fprintf(stderr, "POINTER enter %.1f,%.1f\n", wl_fixed_to_double(x), wl_fixed_to_double(y));
}
static void pointer_leave(void* data, struct wl_pointer* pointer, uint32_t serial, struct wl_surface* surface) {}
static void motion(void* data, struct wl_pointer* pointer, uint32_t time, wl_fixed_t x, wl_fixed_t y) {}
static void button(void* data, struct wl_pointer* pointer, uint32_t serial, uint32_t time, uint32_t button, uint32_t state) {
    fprintf(stderr, "BUTTON code=%u state=%u\n", button, state);
}
static void                             axis(void* data, struct wl_pointer* pointer, uint32_t time, uint32_t axis, wl_fixed_t value) {}
static const struct wl_pointer_listener pointer_listener = {.enter = pointer_enter, .leave = pointer_leave, .motion = motion, .button = button, .axis = axis};
static void                             capabilities(void* data, struct wl_seat* seat, uint32_t caps) {
    static struct wl_keyboard* keyboard;
    static struct wl_pointer*  pointer;
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !keyboard) {
        keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(keyboard, &keyboard_listener, NULL);
    }
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !pointer) {
        pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(pointer, &pointer_listener, NULL);
    }
}
static const struct wl_seat_listener seat_listener = {.capabilities = capabilities};
static void                          global(void* data, struct wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
    if (!strcmp(interface, "wl_compositor"))
        compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
    else if (!strcmp(interface, "wl_shm"))
        shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    else if (!strcmp(interface, "android_wlegl"))
        android = wl_registry_bind(registry, name, &android_wlegl_interface, version < 2 ? version : 2);
    else if (!strcmp(interface, "wl_seat")) {
        seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
        wl_seat_add_listener(seat, &seat_listener, NULL);
    } else if (!strcmp(interface, "xdg_wm_base")) {
        wm = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(wm, &wm_listener, NULL);
    }
}
static void                              removed(void* data, struct wl_registry* registry, uint32_t name) {}
static const struct wl_registry_listener registry_listener = {global, removed};
static void                              configure(void* data, struct xdg_surface* xdg, uint32_t serial) {
    xdg_surface_ack_configure(xdg, serial);
    wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_damage(surface, 0, 0, 640, 480);
    wl_surface_commit(surface);
    fprintf(stderr, "BUFFER committed 640x480 serial=%u\n", serial);
}
static const struct xdg_surface_listener surface_listener = {configure};
static void                              size(void* data, struct xdg_toplevel* top, int32_t width, int32_t height, struct wl_array* states) {}
static void                              closed(void* data, struct xdg_toplevel* top) {
    running = 0;
}
static const struct xdg_toplevel_listener top_listener = {.configure = size, .close = closed};
int                                       main(int argc, char** argv) {
    struct wl_display* display = wl_display_connect(NULL);
    if (!display)
        return 1;
    struct wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    if (wl_display_roundtrip(display) < 0 || !compositor || !shm || !wm)
        return 2;
    int gpu_version = argc > 1 && !strcmp(argv[1], "--ahb-v1") ? 1 : argc > 1 && !strcmp(argv[1], "--ahb-v2") ? 2 : 0;
    if (gpu_version) {
        buffer = make_ahb_buffer(display, android, gpu_version);
        if (!buffer) {
            fprintf(stderr, "AHB v%d failed\n", gpu_version);
            return 5;
        }
    } else {
        int fd = syscall(SYS_memfd_create, "anhyprland-shm-smoke", 1);
        if (fd < 0 || ftruncate(fd, 640 * 480 * 4))
            return 3;
        uint32_t* pixels = mmap(NULL, 640 * 480 * 4, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (pixels == MAP_FAILED)
            return 4;
        for (int y = 0; y < 480; ++y)
            for (int x = 0; x < 640; ++x)
                pixels[y * 640 + x] = 0xff000000u | ((x * 255 / 639) << 16) | ((y * 255 / 479) << 8) | (((x / 40 + y / 40) % 2) ? 0xe0 : 0x40);
        struct wl_shm_pool* pool = wl_shm_create_pool(shm, fd, 640 * 480 * 4);
        buffer                   = wl_shm_pool_create_buffer(pool, 0, 640, 480, 640 * 4, WL_SHM_FORMAT_XRGB8888);
        wl_shm_pool_destroy(pool);
        close(fd);
    }
    surface                 = wl_compositor_create_surface(compositor);
    struct xdg_surface* xdg = xdg_wm_base_get_xdg_surface(wm, surface);
    xdg_surface_add_listener(xdg, &surface_listener, NULL);
    struct xdg_toplevel* top = xdg_surface_get_toplevel(xdg);
    xdg_toplevel_add_listener(top, &top_listener, NULL);
    xdg_toplevel_set_title(top, gpu_version ? "anhyprland GPU AHB smoke" : "anhyprland SHM smoke");
    xdg_toplevel_set_app_id(top, "anhyprland-smoke");
    wl_surface_commit(surface);
    while (running && wl_display_dispatch(display) >= 0) {}
    wl_display_disconnect(display);
    return 0;
}
