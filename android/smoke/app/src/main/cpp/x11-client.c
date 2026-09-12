#include <xcb/xcb.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    const char* runtime = getenv("XDG_RUNTIME_DIR");
    if (!runtime)
        return 1;
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    if (snprintf(address.sun_path, sizeof(address.sun_path), "%s/.X11-unix/X0", runtime) >= sizeof(address.sun_path))
        return 1;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0 || connect(fd, (struct sockaddr*)&address, sizeof(address))) {
        perror("X11 connect");
        return 1;
    }
    xcb_connection_t* connection = xcb_connect_to_fd(fd, NULL);
    if (xcb_connection_has_error(connection))
        return 2;
    xcb_screen_t*  screen   = xcb_setup_roots_iterator(xcb_get_setup(connection)).data;
    xcb_window_t   window   = xcb_generate_id(connection);
    const uint32_t values[] = {0xff224488, XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_STRUCTURE_NOTIFY};
    xcb_create_window(connection, screen->root_depth, window, screen->root, 0, 0, 480, 320, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
                      XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, values);
    const char title[] = "anhyprland Xwayland smoke";
    xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8, sizeof(title) - 1, title);
    const char cls[] = "anhyprland-x11-smoke\0anhyprland-x11-smoke\0";
    xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8, sizeof(cls) - 1, cls);
    xcb_gcontext_t gc    = xcb_generate_id(connection);
    uint32_t       color = 0xffff9900;
    xcb_create_gc(connection, gc, window, XCB_GC_FOREGROUND, &color);
    xcb_map_window(connection, window);
    xcb_flush(connection);
    fprintf(stderr, "X11 created window=%u depth=%u\n", window, screen->root_depth);
    xcb_generic_event_t* event;
    while ((event = xcb_wait_for_event(connection))) {
        int type = event->response_type & 0x7f;
        if (type == XCB_EXPOSE) {
            xcb_rectangle_t rectangles[] = {{20, 20, 180, 120}, {220, 160, 180, 120}};
            xcb_poly_fill_rectangle(connection, window, gc, 2, rectangles);
            xcb_flush(connection);
            fprintf(stderr, "X11 exposed and painted\n");
        } else if (type == XCB_KEY_PRESS)
            fprintf(stderr, "X11 KEY %u\n", ((xcb_key_press_event_t*)event)->detail);
        else if (type == XCB_BUTTON_PRESS)
            fprintf(stderr, "X11 BUTTON %u\n", ((xcb_button_press_event_t*)event)->detail);
        else if (type == 0)
            fprintf(stderr, "X11 ERROR %u\n", ((xcb_generic_error_t*)event)->error_code);
        free(event);
    }
    xcb_disconnect(connection);
    return 0;
}
