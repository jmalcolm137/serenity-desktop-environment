/*
 * Minimal native xdg-shell client used to exercise the compositor with a
 * non-Serenity ("native") Wayland application. Not part of the desktop: a test
 * tool.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include "xdg-shell-client-protocol.h"
#include "serenity-window-client-protocol.h"

static struct wl_display* display;
static struct wl_compositor* compositor;
static struct wl_shm* shm;
static struct xdg_wm_base* wm_base;
static struct wl_surface* surface;
static struct xdg_surface* xdg_surface;
static struct xdg_toplevel* toplevel;
static int width = 320;
static int height = 200;
static int configured;

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
static void keyboard_key(void* d, struct wl_keyboard* k, uint32_t serial, uint32_t time, uint32_t key, uint32_t state) { (void)d; (void)k; (void)serial; (void)time; (void)key; (void)state; }
static void keyboard_modifiers(void* d, struct wl_keyboard* k, uint32_t serial, uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group) { (void)d; (void)k; (void)serial; (void)depressed; (void)latched; (void)locked; (void)group; }
static void keyboard_repeat_info(void* d, struct wl_keyboard* k, int32_t rate, int32_t delay) { (void)d; (void)k; (void)rate; (void)delay; }
static struct wl_keyboard_listener const keyboard_listener = { keyboard_keymap, keyboard_enter, keyboard_leave, keyboard_key, keyboard_modifiers, keyboard_repeat_info };

static void seat_capabilities(void* d, struct wl_seat* s, uint32_t capabilities)
{
    (void)d;
    if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && !keyboard) {
        keyboard = wl_seat_get_keyboard(s);
        wl_keyboard_add_listener(keyboard, &keyboard_listener, NULL);
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
    }
}

static void registry_remove(void* data, struct wl_registry* registry, uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}

static struct wl_registry_listener const registry_listener = { registry_global, registry_remove };

int main(void)
{
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
    xdg_toplevel_set_title(toplevel, "SDE Native Test");
    xdg_toplevel_set_app_id(toplevel, "sde-native-test");
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

    for (int i = 0; i < 200; i++)
        wl_display_dispatch(display);
    return 0;
}
