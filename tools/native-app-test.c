/*
 * Minimal native xdg-shell client used to exercise the compositor with a
 * non-Serenity ("native") Wayland application. Not part of the desktop: a test
 * tool.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#define _GNU_SOURCE
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include "xdg-shell-client-protocol.h"
#include "serenity-window-client-protocol.h"
#include "presentation-time-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "xdg-decoration-client-protocol.h"

static struct wl_display* display;
static struct wl_compositor* compositor;
static struct wl_shm* shm;
static struct xdg_wm_base* wm_base;
static struct wl_surface* surface;
static struct xdg_surface* xdg_surface;
static struct xdg_toplevel* toplevel;

// --server-side-decorations: ask the compositor to draw the window frame. Without
// it the client gets client-side decorations (the compositor's default), so there
// is no compositor titlebar to interact with.
static int want_server_side_decorations;
static struct zxdg_decoration_manager_v1* decoration_manager;
static struct zxdg_toplevel_decoration_v1* toplevel_decoration;
// The toplevel title (--title), so a test can tell two instances apart.
static char const* g_title = "SDE Native Test";
static int width = 320;
static int height = 200;
static int configured;

// wp_presentation: with --presentation, request feedback for a committed frame
// and wait for the compositor to report it displayed.
static struct wp_presentation* presentation;
static int presented;
static uint32_t presented_refresh;

static void feedback_presented(void* data, struct wp_presentation_feedback* feedback,
    uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec, uint32_t refresh,
    uint32_t seq_hi, uint32_t seq_lo, uint32_t flags)
{
    (void)data;
    (void)feedback;
    (void)tv_sec_hi;
    (void)tv_sec_lo;
    (void)tv_nsec;
    (void)seq_hi;
    (void)seq_lo;
    (void)flags;
    presented = 1;
    presented_refresh = refresh;
}

static void feedback_sync_output(void* data, struct wp_presentation_feedback* feedback, struct wl_output* output)
{
    (void)data;
    (void)feedback;
    (void)output;
}

static void feedback_discarded(void* data, struct wp_presentation_feedback* feedback)
{
    (void)data;
    (void)feedback;
}

static struct wp_presentation_feedback_listener const feedback_listener = {
    .sync_output = feedback_sync_output,
    .presented = feedback_presented,
    .discarded = feedback_discarded,
};

// wp_fractional_scale_v1: learn the (possibly fractional) preferred scale.
static struct wp_fractional_scale_manager_v1* fractional_scale_manager;
static uint32_t preferred_scale_v120;

static void fractional_preferred_scale(void* data, struct wp_fractional_scale_v1* object, uint32_t scale)
{
    (void)data;
    (void)object;
    preferred_scale_v120 = scale;
}

static struct wp_fractional_scale_v1_listener const fractional_scale_listener = { fractional_preferred_scale };

static struct wl_buffer* create_buffer(int w, int h)
{
    int stride = w * 4;
    int size = stride * h;
    int fd = memfd_create("native-app-test", 0);
    if (fd < 0) {
        perror("memfd_create");
        exit(1);
    }
    if (ftruncate(fd, size) < 0) {
        perror("ftruncate");
        exit(1);
    }
    void* data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }
    for (int i = 0; i < w * h; i++)
        ((uint32_t*)data)[i] = 0xFFC06030;

    struct wl_shm_pool* pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer* buffer = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return buffer;
}

static void wm_base_ping(void* data, struct xdg_wm_base* base, uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(base, serial);
}

static struct xdg_wm_base_listener const wm_base_listener = { wm_base_ping };

static void render(void)
{
    struct wl_buffer* buffer = create_buffer(width, height);
    wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_damage_buffer(surface, 0, 0, width, height);
    wl_surface_commit(surface);
}

static void xdg_surface_configure(void* data, struct xdg_surface* surf, uint32_t serial)
{
    (void)data;
    xdg_surface_ack_configure(surf, serial);
    render();
    configured = 1;
}

static struct xdg_surface_listener const xdg_surface_listener = { xdg_surface_configure };

static void toplevel_configure(void* data, struct xdg_toplevel* tl, int32_t w, int32_t h, struct wl_array* states)
{
    (void)data;
    (void)tl;
    (void)states;
    if (w > 0)
        width = w;
    if (h > 0)
        height = h;
    if (configured)
        render();
}

static void toplevel_close(void* data, struct xdg_toplevel* tl)
{
    (void)data;
    (void)tl;
    exit(0);
}

static struct xdg_toplevel_listener const toplevel_listener = { toplevel_configure, toplevel_close };

// Bind a seat + keyboard so the compositor can focus this window (the tile
// keybindings act on the focused window).
static struct wl_seat* seat;
static struct wl_keyboard* keyboard;

// The serenity-window chrome handle, so we can send the compositor our icon.
static struct serenity_window_manager* window_manager;
static struct serenity_toplevel* serenity_toplevel_handle;

static void keyboard_keymap(void* d, struct wl_keyboard* k, uint32_t format, int32_t fd, uint32_t size) { (void)d; (void)k; (void)format; (void)size; close(fd); }
static void keyboard_enter(void* d, struct wl_keyboard* k, uint32_t serial, struct wl_surface* s, struct wl_array* keys) { (void)d; (void)k; (void)serial; (void)s; (void)keys; }
static void keyboard_leave(void* d, struct wl_keyboard* k, uint32_t serial, struct wl_surface* s) { (void)d; (void)k; (void)serial; (void)s; }
static void keyboard_key(void* d, struct wl_keyboard* k, uint32_t serial, uint32_t time, uint32_t key, uint32_t state) { (void)d; (void)k; (void)serial; (void)time; fprintf(stderr, "native-app-test: key %u %s\n", key, state == WL_KEYBOARD_KEY_STATE_PRESSED ? "press" : "release"); }
static void keyboard_modifiers(void* d, struct wl_keyboard* k, uint32_t serial, uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group) { (void)d; (void)k; (void)serial; (void)depressed; (void)latched; (void)locked; (void)group; }
static void keyboard_repeat_info(void* d, struct wl_keyboard* k, int32_t rate, int32_t delay) { (void)d; (void)k; (void)rate; (void)delay; }
static struct wl_keyboard_listener const keyboard_listener = { keyboard_keymap, keyboard_enter, keyboard_leave, keyboard_key, keyboard_modifiers, keyboard_repeat_info };

static void seat_capabilities(void* d, struct wl_seat* s, uint32_t capabilities)
{
    (void)d;
    if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && !keyboard) {
        keyboard = wl_seat_get_keyboard(s);
        wl_keyboard_add_listener(keyboard, &keyboard_listener, NULL);
    } else if (!(capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && keyboard) {
        // The seat's keyboard went away (e.g. a virtual keyboard disconnected);
        // release ours so we get a fresh, live one when it comes back.
        wl_keyboard_destroy(keyboard);
        keyboard = NULL;
    }
}
static void seat_name(void* d, struct wl_seat* s, char const* name) { (void)d; (void)s; (void)name; }
static struct wl_seat_listener const seat_listener = { seat_capabilities, seat_name };

static void registry_global(void* data, struct wl_registry* registry, uint32_t name, char const* interface, uint32_t version)
{
    (void)data;
    (void)version;
    if (strcmp(interface, wl_compositor_interface.name) == 0)
        compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
    else if (strcmp(interface, wl_shm_interface.name) == 0)
        shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    else if (strcmp(interface, xdg_wm_base_interface.name) == 0)
        wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
    else if (strcmp(interface, wl_seat_interface.name) == 0) {
        seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
        wl_seat_add_listener(seat, &seat_listener, NULL);
    } else if (strcmp(interface, serenity_window_manager_interface.name) == 0) {
        window_manager = wl_registry_bind(registry, name, &serenity_window_manager_interface, 1);
    } else if (strcmp(interface, wp_presentation_interface.name) == 0) {
        presentation = wl_registry_bind(registry, name, &wp_presentation_interface, 1);
    } else if (strcmp(interface, wp_fractional_scale_manager_v1_interface.name) == 0) {
        fractional_scale_manager = wl_registry_bind(registry, name, &wp_fractional_scale_manager_v1_interface, 1);
    } else if (strcmp(interface, zxdg_decoration_manager_v1_interface.name) == 0 && want_server_side_decorations) {
        decoration_manager = wl_registry_bind(registry, name, &zxdg_decoration_manager_v1_interface, 1);
    }
}

static void registry_remove(void* data, struct wl_registry* registry, uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}

static struct wl_registry_listener const registry_listener = { registry_global, registry_remove };

int main(int argc, char** argv)
{
    int const want_presentation = argc > 1 && strcmp(argv[1], "--presentation") == 0;
    int const want_fractional = argc > 1 && strcmp(argv[1], "--fractional") == 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--server-side-decorations") == 0)
            want_server_side_decorations = 1;
    }

    // --title <text>: a distinct title, so a test can tell two instances apart.
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--title") == 0) {
            g_title = argv[i + 1];
            break;
        }
    }
    // --linger <ms>: keep the window mapped for this long before exiting, so a test
    // can map several clients and observe how focus moves between them.
    int linger_ms = 0;
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--linger") == 0) {
            linger_ms = atoi(argv[i + 1]);
            break;
        }
    }

    display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "native-app-test: cannot connect to a compositor\n");
        return 1;
    }

    struct wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);

    if (!compositor || !shm || !wm_base) {
        fprintf(stderr, "native-app-test: missing compositor/shm/xdg_wm_base globals\n");
        return 1;
    }

    xdg_wm_base_add_listener(wm_base, &wm_base_listener, NULL);

    surface = wl_compositor_create_surface(compositor);
    xdg_surface = xdg_wm_base_get_xdg_surface(wm_base, surface);
    xdg_surface_add_listener(xdg_surface, &xdg_surface_listener, NULL);
    toplevel = xdg_surface_get_toplevel(xdg_surface);
    xdg_toplevel_add_listener(toplevel, &toplevel_listener, NULL);
    xdg_toplevel_set_title(toplevel, g_title);
    xdg_toplevel_set_app_id(toplevel, "sde-native-test");
    // --server-side-decorations: ask for a compositor-drawn frame. Without it the
    // default is client-side decorations, so there is no titlebar to click/drag.
    if (want_server_side_decorations && decoration_manager) {
        toplevel_decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(decoration_manager, toplevel);
        zxdg_toplevel_decoration_v1_set_mode(toplevel_decoration, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    }
    wl_surface_commit(surface);

    wl_display_roundtrip(display);
    wl_display_roundtrip(display);

    // Send our window icon over serenity-window so the compositor can show it in
    // the titlebar and the Alt+Tab switcher (a 16x16 ARGB8888 green square).
    if (window_manager) {
        serenity_toplevel_handle = serenity_window_manager_get_toplevel(window_manager, surface);
        int const iw = 16, ih = 16, istride = iw * 4;
        int ifd = memfd_create("native-app-icon", 0);
        if (ifd >= 0 && ftruncate(ifd, istride * ih) == 0) {
            void* pixels = mmap(NULL, istride * ih, PROT_READ | PROT_WRITE, MAP_SHARED, ifd, 0);
            if (pixels != MAP_FAILED) {
                for (int i = 0; i < iw * ih; i++)
                    ((uint32_t*)pixels)[i] = 0xFF00C000; // ARGB: opaque green
                munmap(pixels, istride * ih);
            }
            serenity_toplevel_set_icon(serenity_toplevel_handle, iw, ih, istride, 0, ifd);
            close(ifd);
            wl_display_flush(display);
        }
    }

    fprintf(stderr, "native-app-test: mapped %dx%d\n", width, height);

    if (want_fractional) {
        if (!fractional_scale_manager) {
            fprintf(stderr, "native-app-test: no wp_fractional_scale_manager_v1 global\n");
            return 1;
        }
        // Learn the (possibly fractional) preferred scale the compositor has
        // assigned to our surface (R6 fractional scaling).
        struct wp_fractional_scale_v1* fractional =
            wp_fractional_scale_manager_v1_get_fractional_scale(fractional_scale_manager, surface);
        wp_fractional_scale_v1_add_listener(fractional, &fractional_scale_listener, NULL);
        render();
        for (int i = 0; i < 200 && preferred_scale_v120 == 0; i++) {
            if (wl_display_dispatch(display) < 0)
                break;
        }
        if (preferred_scale_v120 == 0) {
            fprintf(stderr, "native-app-test: no fractional preferred_scale\n");
            return 1;
        }
        fprintf(stderr, "native-app-test: fractional preferred_scale=%u (%.3f)\n",
            preferred_scale_v120, preferred_scale_v120 / 120.0);
        return 0;
    }

    if (want_presentation) {
        if (!presentation) {
            fprintf(stderr, "native-app-test: no wp_presentation global\n");
            return 1;
        }
        // Ask for feedback for the next frame, commit it, and wait for the
        // compositor to report it displayed (R6 presentation-time).
        struct wp_presentation_feedback* feedback = wp_presentation_feedback(presentation, surface);
        wp_presentation_feedback_add_listener(feedback, &feedback_listener, NULL);
        render();
        for (int i = 0; i < 400 && !presented; i++) {
            if (wl_display_dispatch(display) < 0)
                break;
        }
        if (!presented) {
            fprintf(stderr, "native-app-test: no presentation feedback\n");
            return 1;
        }
        fprintf(stderr, "native-app-test: presented refresh=%u\n", presented_refresh);
        return 0;
    }

    if (linger_ms > 0) {
        struct pollfd pfd = { .fd = wl_display_get_fd(display), .events = POLLIN };
        int waited = 0;
        while (waited < linger_ms) {
            wl_display_dispatch_pending(display);
            wl_display_flush(display);
            if (poll(&pfd, 1, 50) > 0)
                wl_display_dispatch(display);
            waited += 50;
        }
        return 0;
    }

    for (int i = 0; i < 200; i++)
        wl_display_dispatch(display);
    return 0;
}
