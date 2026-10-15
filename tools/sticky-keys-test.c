/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// A minimal xdg-shell client that maps a toplevel (to get keyboard focus) and
// records every wl_keyboard key event together with the last modifier mask the
// compositor announced, so a headless test can assert that a latched modifier
// actually reaches the client:
//
//   sticky-keys-test: key=30 press depressed=1 latched=0 locked=0
//   sticky-keys-test: key=30 release depressed=0 latched=0 locked=0
//
// The mask is what the compositor last sent via wl_keyboard.modifiers, which is
// the whole point: clients translate keycodes themselves, so a modifier only has
// to be in the mask they are told about.
//
// Usage: sticky-keys-test [--duration MS]
//
// Not part of the desktop: a test tool.

#define _GNU_SOURCE
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include "xdg-shell-client-protocol.h"

static struct wl_display* display;
static struct wl_compositor* compositor;
static struct wl_shm* shm;
static struct xdg_wm_base* wm_base;
static struct wl_surface* surface;
static struct xdg_surface* xdg_surface;
static struct xdg_toplevel* toplevel;
static struct wl_seat* seat;
static struct wl_keyboard* keyboard;
static int width = 320;
static int height = 200;
static int configured;

static uint32_t mods_depressed;
static uint32_t mods_latched;
static uint32_t mods_locked;
static uint64_t start_ms;

static uint64_t now_ms()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static struct wl_buffer* create_buffer(int w, int h)
{
    int stride = w * 4;
    int size = stride * h;
    int fd = memfd_create("sticky-keys-test", 0);
    if (fd < 0)
        return NULL;
    if (ftruncate(fd, size) < 0) {
        close(fd);
        return NULL;
    }
    void* data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        return NULL;
    }
    for (int i = 0; i < w * h; i++)
        ((uint32_t*)data)[i] = 0xFFB0C070;

    struct wl_shm_pool* pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer* buffer = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return buffer;
}

static void render()
{
    struct wl_buffer* buffer = create_buffer(width, height);
    if (!buffer)
        return;
    wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_damage_buffer(surface, 0, 0, width, height);
    wl_surface_commit(surface);
}

static void wm_base_ping(void* data, struct xdg_wm_base* base, uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(base, serial);
}

static struct xdg_wm_base_listener const wm_base_listener = { wm_base_ping };

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

static void keyboard_keymap(void* d, struct wl_keyboard* k, uint32_t format, int32_t fd, uint32_t size)
{
    (void)d;
    (void)k;
    (void)format;
    (void)size;
    close(fd);
}

static void keyboard_enter(void* d, struct wl_keyboard* k, uint32_t serial, struct wl_surface* s, struct wl_array* keys)
{
    (void)d;
    (void)k;
    (void)serial;
    (void)s;
    (void)keys;
}

static void keyboard_leave(void* d, struct wl_keyboard* k, uint32_t serial, struct wl_surface* s)
{
    (void)d;
    (void)k;
    (void)serial;
    (void)s;
}

static void keyboard_key(void* d, struct wl_keyboard* k, uint32_t serial, uint32_t time, uint32_t key, uint32_t state)
{
    (void)d;
    (void)k;
    (void)serial;
    (void)time;
    fprintf(stderr, "sticky-keys-test: key=%u %s depressed=%u latched=%u locked=%u\n", key,
        state == WL_KEYBOARD_KEY_STATE_PRESSED ? "press" : "release",
        mods_depressed, mods_latched, mods_locked);
    fflush(stderr);
}

static void keyboard_modifiers(void* d, struct wl_keyboard* k, uint32_t serial, uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group)
{
    (void)d;
    (void)k;
    (void)serial;
    (void)group;
    mods_depressed = depressed;
    mods_latched = latched;
    mods_locked = locked;
}

static void keyboard_repeat_info(void* d, struct wl_keyboard* k, int32_t rate, int32_t delay)
{
    (void)d;
    (void)k;
    (void)rate;
    (void)delay;
}

static struct wl_keyboard_listener const keyboard_listener = { keyboard_keymap, keyboard_enter, keyboard_leave, keyboard_key, keyboard_modifiers, keyboard_repeat_info };

static void seat_capabilities(void* d, struct wl_seat* s, uint32_t capabilities)
{
    (void)d;
    if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && !keyboard) {
        keyboard = wl_seat_get_keyboard(s);
        wl_keyboard_add_listener(keyboard, &keyboard_listener, NULL);
    }
}

static void seat_name(void* d, struct wl_seat* s, char const* name)
{
    (void)d;
    (void)s;
    (void)name;
}

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
    uint64_t duration_ms = 4000;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc)
            duration_ms = (uint64_t)strtoull(argv[++i], NULL, 0);
    }

    display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "sticky-keys-test: cannot connect to a compositor\n");
        return 1;
    }

    struct wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);

    if (!compositor || !shm || !wm_base || !seat) {
        fprintf(stderr, "sticky-keys-test: missing compositor/shm/xdg_wm_base/seat globals\n");
        return 1;
    }

    xdg_wm_base_add_listener(wm_base, &wm_base_listener, NULL);

    surface = wl_compositor_create_surface(compositor);
    xdg_surface = xdg_wm_base_get_xdg_surface(wm_base, surface);
    xdg_surface_add_listener(xdg_surface, &xdg_surface_listener, NULL);
    toplevel = xdg_surface_get_toplevel(xdg_surface);
    xdg_toplevel_add_listener(toplevel, &toplevel_listener, NULL);
    xdg_toplevel_set_title(toplevel, "SDE Sticky Keys Test");
    xdg_toplevel_set_app_id(toplevel, "sde-sticky-keys-test");
    wl_surface_commit(surface);

    wl_display_roundtrip(display);
    wl_display_roundtrip(display);

    start_ms = now_ms();
    fprintf(stderr, "sticky-keys-test: mapped %dx%d for %llums\n", width, height, (unsigned long long)duration_ms);
    fflush(stderr);

    struct pollfd pfd = { .fd = wl_display_get_fd(display), .events = POLLIN };
    while (now_ms() - start_ms < duration_ms) {
        wl_display_dispatch_pending(display);
        wl_display_flush(display);
        if (poll(&pfd, 1, 50) > 0)
            wl_display_dispatch(display);
    }

    fprintf(stderr, "sticky-keys-test: done\n");
    return 0;
}
