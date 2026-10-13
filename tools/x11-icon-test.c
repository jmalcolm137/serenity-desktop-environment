// A minimal X11 client that maps a window with _NET_WM_ICON and WM_CLASS set, so
// the XWayland test can assert the compositor fetches the icon
// (wlr_xwayland_surface_fetch_icon + xcb_ewmh) and reports WM_CLASS.
//
// Usage: x11-icon-test   (uses DISPLAY; keeps the window mapped for ~6s)
#define _GNU_SOURCE

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xcb/xcb.h>
#include <xcb/xcb_ewmh.h>

int main(void)
{
    xcb_connection_t* connection = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(connection)) {
        fprintf(stderr, "x11-icon-test: cannot connect to the X server\n");
        return 1;
    }
    xcb_screen_t* screen = xcb_setup_roots_iterator(xcb_get_setup(connection)).data;
    xcb_window_t window = xcb_generate_id(connection);

    uint32_t const value_mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
    uint32_t const values[] = { screen->black_pixel, XCB_EVENT_MASK_EXPOSURE };
    xcb_create_window(connection, XCB_COPY_FROM_PARENT, window, screen->root, 80, 80, 160, 120, 0,
        XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, value_mask, values);

    xcb_ewmh_connection_t ewmh;
    xcb_intern_atom_cookie_t* atom_cookie = xcb_ewmh_init_atoms(connection, &ewmh);
    if (!xcb_ewmh_init_atoms_replies(&ewmh, atom_cookie, NULL)) {
        fprintf(stderr, "x11-icon-test: cannot init EWMH atoms\n");
        return 1;
    }

    // WM_CLASS = "<instance>\0<class>\0".
    char wm_class[] = "xicon\0XIconTest\0";
    xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8, sizeof(wm_class) - 1, wm_class);

    // _NET_WM_ICON: CARDINAL[2 + w*h] = [width, height, ARGB pixels...]. A 16x16
    // frame is the size the Taskbar/switcher prefer.
    enum { size = 16 };
    uint32_t icon[2 + size * size];
    icon[0] = size;
    icon[1] = size;
    for (int i = 0; i < size * size; ++i)
        icon[2 + i] = 0xFFFF0000u; // opaque red
    xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window, ewmh._NET_WM_ICON, XCB_ATOM_CARDINAL, 32, 2 + size * size, icon);

    xcb_map_window(connection, window);
    xcb_flush(connection);

    // Keep the window mapped so the compositor fetches the properties.
    for (int i = 0; i < 60; ++i) {
        xcb_flush(connection);
        usleep(100 * 1000);
    }

    xcb_destroy_window(connection, window);
    xcb_ewmh_connection_wipe(&ewmh);
    xcb_disconnect(connection);
    return 0;
}
