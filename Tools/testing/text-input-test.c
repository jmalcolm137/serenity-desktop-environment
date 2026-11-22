/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// A minimal xdg-shell client with a wp_text_input_v3 field, so the headless IME
// test can assert the compositor relays text into the focused field. It maps a
// toplevel, enables its text input when the compositor sends `enter`, and prints
// every IME event it receives:
//
//   text-input-test: enter
//   text-input-test: preedit "nihon" 5 5
//   text-input-test: commit "日本"
//   text-input-test: done
//
// Usage: text-input-test [--duration MS]
//
// Not part of the desktop: a test tool.

#define _GNU_SOURCE
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include "text-input-unstable-v3-client-protocol.h"
#include "xdg-shell-client-protocol.h"

static struct wl_display* display;
static struct wl_compositor* compositor;
static struct wl_shm* shm;
static struct wl_seat* seat;
static struct xdg_wm_base* wm_base;
static struct zwp_text_input_manager_v3* text_input_manager;
static struct wl_surface* surface;
static struct xdg_surface* xdg_surface;
static struct xdg_toplevel* toplevel;
static struct zwp_text_input_v3* text_input;
static int width = 400;
static int height = 200;
static int configured;

static struct wl_buffer* create_buffer(int w, int h)
{
    int stride = w * 4;
    int size = stride * h;
    int fd = memfd_create("text-input-test", 0);
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
        ((uint32_t*)data)[i] = 0xFF4060A0;

    struct wl_shm_pool* pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer* buffer = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return buffer;
}

static void render()
{
    struct wl_buffer* buffer = create_buffer(width, height);
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

static void ti_enter(void* data, struct zwp_text_input_v3* ti, struct wl_surface* surf)
{
    (void)data;
    (void)surf;
    printf("text-input-test: enter\n");
    fflush(stdout);
    // Enabling is only valid after `enter`. Report surrounding text and a caret
    // position, so the input method sees them and can place a candidate window.
    zwp_text_input_v3_enable(ti);
    zwp_text_input_v3_set_surrounding_text(ti, "ab", 2, 2);
    zwp_text_input_v3_set_text_change_cause(ti, ZWP_TEXT_INPUT_V3_CHANGE_CAUSE_INPUT_METHOD);
    zwp_text_input_v3_set_cursor_rectangle(ti, 20, 20, 1, 18);
    zwp_text_input_v3_commit(ti);
}

static void ti_leave(void* data, struct zwp_text_input_v3* ti, struct wl_surface* surf)
{
    (void)data;
    (void)ti;
    (void)surf;
    printf("text-input-test: leave\n");
    fflush(stdout);
}

static void ti_preedit_string(void* data, struct zwp_text_input_v3* ti, const char* text, int32_t cursor_begin, int32_t cursor_end)
{
    (void)data;
    (void)ti;
    printf("text-input-test: preedit \"%s\" %d %d\n", text ? text : "", cursor_begin, cursor_end);
    fflush(stdout);
}

static void ti_commit_string(void* data, struct zwp_text_input_v3* ti, const char* text)
{
    (void)data;
    (void)ti;
    printf("text-input-test: commit \"%s\"\n", text ? text : "");
    fflush(stdout);
}

static void ti_delete_surrounding_text(void* data, struct zwp_text_input_v3* ti, uint32_t before, uint32_t after)
{
    (void)data;
    (void)ti;
    printf("text-input-test: delete before=%u after=%u\n", before, after);
    fflush(stdout);
}

static void ti_done(void* data, struct zwp_text_input_v3* ti, uint32_t serial)
{
    (void)data;
    (void)ti;
    (void)serial;
    printf("text-input-test: done\n");
    fflush(stdout);
}

static struct zwp_text_input_v3_listener const ti_listener = {
    ti_enter, ti_leave, ti_preedit_string, ti_commit_string, ti_delete_surrounding_text, ti_done,
};

static void registry_global(void* data, struct wl_registry* reg, uint32_t name, const char* interface, uint32_t version)
{
    (void)data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        compositor = wl_registry_bind(reg, name, &wl_compositor_interface, version < 4 ? version : 4);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        seat = wl_registry_bind(reg, name, &wl_seat_interface, version < 7 ? version : 7);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        wm_base = wl_registry_bind(reg, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(wm_base, &wm_base_listener, NULL);
    } else if (strcmp(interface, zwp_text_input_manager_v3_interface.name) == 0) {
        text_input_manager = wl_registry_bind(reg, name, &zwp_text_input_manager_v3_interface, 1);
    }
}

static void registry_global_remove(void* data, struct wl_registry* reg, uint32_t name)
{
    (void)data;
    (void)reg;
    (void)name;
}

static struct wl_registry_listener const registry_listener = { registry_global, registry_global_remove };

int main(int argc, char** argv)
{
    int duration_ms = 8000;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc)
            duration_ms = atoi(argv[++i]);
    }

    display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "text-input-test: cannot connect to the compositor\n");
        return 1;
    }
    struct wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);
    wl_display_roundtrip(display);

    if (!compositor || !shm || !wm_base || !seat || !text_input_manager) {
        fprintf(stderr, "text-input-test: missing required globals (text-input-v3=%p)\n", (void*)text_input_manager);
        return 1;
    }

    // Create the text input before mapping, so the compositor sees it when it
    // focuses the field.
    text_input = zwp_text_input_manager_v3_get_text_input(text_input_manager, seat);
    zwp_text_input_v3_add_listener(text_input, &ti_listener, NULL);

    surface = wl_compositor_create_surface(compositor);
    xdg_surface = xdg_wm_base_get_xdg_surface(wm_base, surface);
    xdg_surface_add_listener(xdg_surface, &xdg_surface_listener, NULL);
    toplevel = xdg_surface_get_toplevel(xdg_surface);
    xdg_toplevel_add_listener(toplevel, &toplevel_listener, NULL);
    xdg_toplevel_set_title(toplevel, "SDE Text Input Test");
    xdg_toplevel_set_app_id(toplevel, "sde-text-input-test");
    wl_surface_commit(surface);

    // Let the compositor process the map (and so focus the field).
    wl_display_roundtrip(display);
    wl_display_roundtrip(display);

    printf("text-input-test: mapped\n");
    fflush(stdout);

    struct pollfd pfd = { .fd = wl_display_get_fd(display), .events = POLLIN };
    int waited = 0;
    while (waited < duration_ms) {
        wl_display_dispatch_pending(display);
        wl_display_flush(display);
        if (poll(&pfd, 1, 50) > 0)
            wl_display_dispatch(display);
        waited += 50;
    }
    return 0;
}
