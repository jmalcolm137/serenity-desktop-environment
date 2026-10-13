/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// A minimal drag-and-drop *source*: opens a window, waits for a pointer press,
// then starts a data-device DnD from it with an icon, and prints the drag
// lifecycle. Used by scripts/run-dnd-test.sh to assert the compositor accepts and
// routes an explicit drag (and renders its icon). Not part of the desktop: a test
// tool.

#define _GNU_SOURCE

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include "xdg-shell-client-protocol.h"

static struct wl_display* display;
static struct wl_compositor* compositor;
static struct wl_shm* shm;
static struct wl_seat* seat;
static struct wl_pointer* pointer;
static struct wl_data_device_manager* data_device_manager;
static struct wl_data_device* data_device;
static struct xdg_wm_base* wm_base;
static struct wl_surface* surface;
static struct xdg_surface* xdg_surface;
static struct xdg_toplevel* toplevel;

static int width = 400;
static int height = 300;
static int configured;
static int dragging;

static struct wl_buffer* make_buffer(int w, int h, uint32_t color)
{
    int stride = w * 4;
    int size = stride * h;
    int fd = memfd_create("dnd-test", 0);
    if (fd < 0 || ftruncate(fd, size) < 0) {
        perror("dnd-test: memfd");
        exit(1);
    }
    void* data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }
    for (int i = 0; i < w * h; i++)
        ((uint32_t*)data)[i] = color;
    struct wl_shm_pool* pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer* buffer = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return buffer;
}

static void render(void)
{
    struct wl_buffer* buffer = make_buffer(width, height, 0xFF406080);
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

static void xdg_surface_configure(void* data, struct xdg_surface* s, uint32_t serial)
{
    (void)data;
    xdg_surface_ack_configure(s, serial);
    if (!configured) {
        configured = 1;
        printf("mapped\n");
        fflush(stdout);
    }
    render();
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
    printf("configure %dx%d\n", width, height);
    fflush(stdout);
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

// --- data source (the drag payload) ----------------------------------------
static void source_target(void* d, struct wl_data_source* s, char const* mime)
{
    (void)d;
    (void)s;
    (void)mime;
}
static void source_send(void* d, struct wl_data_source* s, char const* mime, int32_t fd)
{
    (void)d;
    (void)s;
    (void)mime;
    char const payload[] = "dnd-test";
    (void)write(fd, payload, sizeof(payload) - 1);
    close(fd);
}
static void source_cancelled(void* d, struct wl_data_source* s)
{
    (void)d;
    printf("drag-cancelled\n");
    fflush(stdout);
    wl_data_source_destroy(s);
    exit(0);
}
static void source_dnd_drop(void* d, struct wl_data_source* s)
{
    (void)d;
    (void)s;
    printf("drag-drop\n");
    fflush(stdout);
}
static void source_dnd_finished(void* d, struct wl_data_source* s)
{
    (void)d;
    (void)s;
    printf("drag-finished\n");
    fflush(stdout);
    exit(0);
}
static void source_action(void* d, struct wl_data_source* s, uint32_t action)
{
    (void)d;
    (void)s;
    (void)action;
}
static struct wl_data_source_listener const source_listener = {
    source_target, source_send, source_cancelled,
    source_dnd_drop, source_dnd_finished, source_action
};

// --- pointer: the press that starts the drag -------------------------------
static void pointer_enter(void* d, struct wl_pointer* p, uint32_t serial, struct wl_surface* s, wl_fixed_t x, wl_fixed_t y)
{
    (void)d; (void)p; (void)serial; (void)s; (void)x; (void)y;
    printf("pointer-enter\n");
    fflush(stdout);
}
static void pointer_leave(void* d, struct wl_pointer* p, uint32_t serial, struct wl_surface* s)
{
    (void)d; (void)p; (void)serial; (void)s;
    printf("pointer-leave\n");
    fflush(stdout);
}
static void pointer_motion(void* d, struct wl_pointer* p, uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
    (void)d; (void)p; (void)time; (void)x; (void)y;
}
static void pointer_button(void* d, struct wl_pointer* p, uint32_t serial, uint32_t time, uint32_t button, uint32_t state)
{
    (void)d;
    (void)p;
    (void)time;
    printf("pointer-button button=0x%x state=%u serial=%u\n", button, state, serial);
    fflush(stdout);
    if (state != WL_POINTER_BUTTON_STATE_PRESSED || button != 0x110 /* BTN_LEFT */ || dragging)
        return;
    dragging = 1;

    struct wl_data_source* source = wl_data_device_manager_create_data_source(data_device_manager);
    wl_data_source_offer(source, "text/plain;charset=utf-8");
    wl_data_source_add_listener(source, &source_listener, NULL);

    // A small drag icon: a plain surface with a buffer.
    struct wl_surface* icon = wl_compositor_create_surface(compositor);
    struct wl_buffer* icon_buffer = make_buffer(16, 16, 0xFF3070B0);
    wl_surface_attach(icon, icon_buffer, 0, 0);
    wl_surface_damage_buffer(icon, 0, 0, 16, 16);
    wl_surface_commit(icon);

    wl_data_device_start_drag(data_device, source, surface, icon, serial);
    wl_display_flush(display);
    printf("drag-started\n");
    fflush(stdout);
}
static void pointer_axis(void* d, struct wl_pointer* p, uint32_t time, uint32_t axis, wl_fixed_t value)
{
    (void)d; (void)p; (void)time; (void)axis; (void)value;
}
static void pointer_frame(void* d, struct wl_pointer* p) { (void)d; (void)p; }
static void pointer_axis_source(void* d, struct wl_pointer* p, uint32_t source) { (void)d; (void)p; (void)source; }
static void pointer_axis_stop(void* d, struct wl_pointer* p, uint32_t time, uint32_t axis) { (void)d; (void)p; (void)time; (void)axis; }
static void pointer_axis_discrete(void* d, struct wl_pointer* p, uint32_t axis, int32_t discrete) { (void)d; (void)p; (void)axis; (void)discrete; }
static void pointer_axis_value120(void* d, struct wl_pointer* p, uint32_t axis, int32_t value120) { (void)d; (void)p; (void)axis; (void)value120; }
static void pointer_axis_relative_direction(void* d, struct wl_pointer* p, uint32_t axis, uint32_t direction) { (void)d; (void)p; (void)axis; (void)direction; }
static struct wl_pointer_listener const pointer_listener = {
    pointer_enter, pointer_leave, pointer_motion, pointer_button, pointer_axis,
    pointer_frame, pointer_axis_source, pointer_axis_stop, pointer_axis_discrete,
    pointer_axis_value120, pointer_axis_relative_direction
};

// --- the receive side (observed when the drop target is this same surface) --
static void offer_offer(void* d, struct wl_data_offer* o, char const* mime)
{
    (void)d; (void)o; (void)mime;
}
static void offer_source_actions(void* d, struct wl_data_offer* o, uint32_t actions)
{
    (void)d; (void)o; (void)actions;
}
static void offer_action(void* d, struct wl_data_offer* o, uint32_t action)
{
    (void)d; (void)o; (void)action;
}
static struct wl_data_offer_listener const offer_listener = { offer_offer, offer_source_actions, offer_action };

static void device_data_offer(void* d, struct wl_data_device* dev, struct wl_data_offer* offer)
{
    (void)d; (void)dev;
    wl_data_offer_add_listener(offer, &offer_listener, NULL);
}
static void device_enter(void* d, struct wl_data_device* dev, uint32_t serial, struct wl_surface* s, wl_fixed_t x, wl_fixed_t y, struct wl_data_offer* offer)
{
    (void)d; (void)dev; (void)serial; (void)s; (void)x; (void)y;
    printf("target-enter\n");
    fflush(stdout);
    (void)offer;
}
static void device_leave(void* d, struct wl_data_device* dev)
{
    (void)d; (void)dev;
    printf("target-leave\n");
    fflush(stdout);
}
static void device_motion(void* d, struct wl_data_device* dev, uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
    (void)d; (void)dev; (void)time; (void)x; (void)y;
}
static void device_drop(void* d, struct wl_data_device* dev)
{
    (void)d; (void)dev;
    printf("target-drop\n");
    fflush(stdout);
}
static void device_selection(void* d, struct wl_data_device* dev, struct wl_data_offer* offer)
{
    (void)d; (void)dev;
    (void)offer;
}
static struct wl_data_device_listener const device_listener = {
    device_data_offer, device_enter, device_leave, device_motion, device_drop, device_selection
};

static void seat_capabilities(void* d, struct wl_seat* s, uint32_t caps)
{
    (void)d;
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !pointer) {
        pointer = wl_seat_get_pointer(s);
        wl_pointer_add_listener(pointer, &pointer_listener, NULL);
        if (!data_device) {
            data_device = wl_data_device_manager_get_data_device(data_device_manager, s);
            wl_data_device_add_listener(data_device, &device_listener, NULL);
        }
    }
}
static void seat_name(void* d, struct wl_seat* s, char const* name) { (void)d; (void)s; (void)name; }
static struct wl_seat_listener const seat_listener = { seat_capabilities, seat_name };

static void registry_global(void* d, struct wl_registry* reg, uint32_t name, char const* interface, uint32_t version)
{
    (void)d;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        compositor = wl_registry_bind(reg, name, &wl_compositor_interface, version < 4 ? version : 4);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        seat = wl_registry_bind(reg, name, &wl_seat_interface, version < 5 ? version : 5);
        wl_seat_add_listener(seat, &seat_listener, NULL);
    } else if (strcmp(interface, wl_data_device_manager_interface.name) == 0) {
        data_device_manager = wl_registry_bind(reg, name, &wl_data_device_manager_interface, version < 3 ? version : 3);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        wm_base = wl_registry_bind(reg, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(wm_base, &wm_base_listener, NULL);
    }
}
static void registry_global_remove(void* d, struct wl_registry* reg, uint32_t name) { (void)d; (void)reg; (void)name; }
static struct wl_registry_listener const registry_listener = { registry_global, registry_global_remove };

int main(void)
{
    display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "dnd-test: cannot connect to a compositor\n");
        return 1;
    }
    struct wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);
    if (!compositor || !shm || !wm_base || !data_device_manager) {
        fprintf(stderr, "dnd-test: missing required globals\n");
        return 1;
    }

    surface = wl_compositor_create_surface(compositor);
    xdg_surface = xdg_wm_base_get_xdg_surface(wm_base, surface);
    xdg_surface_add_listener(xdg_surface, &xdg_surface_listener, NULL);
    toplevel = xdg_surface_get_toplevel(xdg_surface);
    xdg_toplevel_add_listener(toplevel, &toplevel_listener, NULL);
    xdg_toplevel_set_title(toplevel, "dnd-test");
    // Fill the output so the pointer lands on us.
    xdg_toplevel_set_maximized(toplevel);
    wl_surface_commit(surface);

    while (wl_display_dispatch(display) != -1) {
    }
    return 0;
}
