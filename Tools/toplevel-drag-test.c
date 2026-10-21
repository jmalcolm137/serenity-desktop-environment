/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include "xdg-shell-client-protocol.h"
#include "xdg-toplevel-drag-v1-client-protocol.h"

static struct wl_display* display;
static struct wl_registry* registry;
static struct wl_compositor* compositor;
static struct wl_shm* shm;
static struct xdg_wm_base* wm_base;
static struct wl_seat* seat;
static struct wl_pointer* pointer;
static struct wl_data_device_manager* data_device_manager;
static struct wl_data_device* data_device;
static struct xdg_toplevel_drag_manager_v1* drag_manager;

static struct wl_surface* surface;
static struct xdg_surface* xdg_surface;
static struct xdg_toplevel* xdg_toplevel;
static struct wl_buffer* buffer;
static struct wl_data_source* data_source;
static struct xdg_toplevel_drag_v1* toplevel_drag;

static int drag_started = 0;
static uint32_t button_serial = 0;

static void registry_global(void*, struct wl_registry* registry, uint32_t name, const char* interface, uint32_t version)
{
    if (strcmp(interface, wl_compositor_interface.name) == 0)
        compositor = wl_registry_bind(registry, name, &wl_compositor_interface, version < 4 ? version : 4);
    else if (strcmp(interface, wl_shm_interface.name) == 0)
        shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    else if (strcmp(interface, xdg_wm_base_interface.name) == 0)
        wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
    else if (strcmp(interface, wl_seat_interface.name) == 0)
        seat = wl_registry_bind(registry, name, &wl_seat_interface, version < 5 ? version : 5);
    else if (strcmp(interface, wl_data_device_manager_interface.name) == 0)
        data_device_manager = wl_registry_bind(registry, name, &wl_data_device_manager_interface, version < 3 ? version : 3);
    else if (strcmp(interface, xdg_toplevel_drag_manager_v1_interface.name) == 0)
        drag_manager = wl_registry_bind(registry, name, &xdg_toplevel_drag_manager_v1_interface, 1);
}

static void registry_global_remove(void*, struct wl_registry*, uint32_t) { }

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static struct wl_buffer* create_buffer(void)
{
    int const width = 400;
    int const height = 400;
    int const stride = width * 4;
    int const size = stride * height;
    int fd = memfd_create("toplevel-drag-test", MFD_CLOEXEC);
    if (fd < 0)
        return NULL;
    if (ftruncate(fd, size) < 0) {
        close(fd);
        return NULL;
    }
    void* pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) {
        close(fd);
        return NULL;
    }
    memset(pixels, 0x80, size);
    struct wl_shm_pool* pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer* new_buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    munmap(pixels, size);
    close(fd);
    return new_buffer;
}

static void wm_base_ping(void*, struct xdg_wm_base* wm_base, uint32_t serial)
{
    xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
    .ping = wm_base_ping,
};

static void xdg_surface_configure(void*, struct xdg_surface* xdg_surface, uint32_t serial)
{
    xdg_surface_ack_configure(xdg_surface, serial);
    if (!buffer)
        buffer = create_buffer();
    if (!buffer) {
        fprintf(stderr, "TOPLEVEL-DRAG: failed to create the buffer\n");
        exit(1);
    }
    wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_damage(surface, 0, 0, 400, 400);
    wl_surface_commit(surface);
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static void xdg_toplevel_configure(void*, struct xdg_toplevel*, int32_t, int32_t, struct wl_array*) { }
static void xdg_toplevel_close(void*, struct xdg_toplevel*) { }

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
    .configure = xdg_toplevel_configure,
    .close = xdg_toplevel_close,
};

static void data_source_target(void*, struct wl_data_source*, const char*) { }
static void data_source_send(void*, struct wl_data_source*, const char*, int32_t fd)
{
    if (fd >= 0)
        close(fd);
}
static void data_source_cancelled(void*, struct wl_data_source*) { }
static void data_source_drop_performed(void*, struct wl_data_source*) { }
static void data_source_finished(void*, struct wl_data_source*) { }
static void data_source_action(void*, struct wl_data_source*, uint32_t) { }

static const struct wl_data_source_listener data_source_listener = {
    .target = data_source_target,
    .send = data_source_send,
    .cancelled = data_source_cancelled,
    .dnd_drop_performed = data_source_drop_performed,
    .dnd_finished = data_source_finished,
    .action = data_source_action,
};

static void data_device_data_offer(void*, struct wl_data_device*, struct wl_data_offer* offer)
{
    wl_data_offer_destroy(offer);
}
static void data_device_enter(void*, struct wl_data_device*, uint32_t, struct wl_surface*, wl_fixed_t, wl_fixed_t, struct wl_data_offer*) { }
static void data_device_leave(void*, struct wl_data_device*) { }
static void data_device_motion(void*, struct wl_data_device*, uint32_t, wl_fixed_t, wl_fixed_t) { }
static void data_device_drop(void*, struct wl_data_device*) { }
static void data_device_selection(void*, struct wl_data_device*, struct wl_data_offer* offer)
{
    if (offer)
        wl_data_offer_destroy(offer);
}

static const struct wl_data_device_listener data_device_listener = {
    .data_offer = data_device_data_offer,
    .enter = data_device_enter,
    .leave = data_device_leave,
    .motion = data_device_motion,
    .drop = data_device_drop,
    .selection = data_device_selection,
};

static void start_drag(void)
{
    if (drag_started || !data_device || !drag_manager || !button_serial)
        return;
    drag_started = 1;
    data_source = wl_data_device_manager_create_data_source(data_device_manager);
    wl_data_source_add_listener(data_source, &data_source_listener, NULL);
    wl_data_source_offer(data_source, "text/plain");
    wl_data_source_set_actions(data_source, WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE);

    toplevel_drag = xdg_toplevel_drag_manager_v1_get_xdg_toplevel_drag(drag_manager, data_source);
    xdg_toplevel_drag_v1_attach(toplevel_drag, xdg_toplevel, 50, 50);
    wl_data_device_start_drag(data_device, data_source, surface, NULL, button_serial);
    wl_display_flush(display);
    printf("TOPLEVEL-DRAG: started\n");
    fflush(stdout);
}

static void pointer_enter(void*, struct wl_pointer*, uint32_t, struct wl_surface*, wl_fixed_t, wl_fixed_t) { }
static void pointer_leave(void*, struct wl_pointer*, uint32_t, struct wl_surface*) { }
static void pointer_motion(void*, struct wl_pointer*, uint32_t, wl_fixed_t, wl_fixed_t) { }
static void pointer_button(void*, struct wl_pointer*, uint32_t serial, uint32_t, uint32_t button, uint32_t state)
{
    if (button == 0x110 && state == WL_POINTER_BUTTON_STATE_PRESSED) {
        button_serial = serial;
        start_drag();
    }
}
static void pointer_axis(void*, struct wl_pointer*, uint32_t, uint32_t, wl_fixed_t) { }
static void pointer_frame(void*, struct wl_pointer*) { }

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
    .frame = pointer_frame,
};

static void seat_capabilities(void*, struct wl_seat* seat, uint32_t capabilities)
{
    if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && !pointer) {
        pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(pointer, &pointer_listener, NULL);
    }
}

static void seat_name(void*, struct wl_seat*, const char*) { }

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

int main(void)
{
    display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "TOPLEVEL-DRAG: cannot connect\n");
        return 1;
    }
    registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);
    if (!compositor || !shm || !wm_base || !seat || !data_device_manager || !drag_manager) {
        fprintf(stderr, "TOPLEVEL-DRAG: missing globals\n");
        return 1;
    }

    xdg_wm_base_add_listener(wm_base, &wm_base_listener, NULL);
    wl_seat_add_listener(seat, &seat_listener, NULL);
    data_device = wl_data_device_manager_get_data_device(data_device_manager, seat);
    wl_data_device_add_listener(data_device, &data_device_listener, NULL);

    surface = wl_compositor_create_surface(compositor);
    xdg_surface = xdg_wm_base_get_xdg_surface(wm_base, surface);
    xdg_surface_add_listener(xdg_surface, &xdg_surface_listener, NULL);
    xdg_toplevel = xdg_surface_get_toplevel(xdg_surface);
    xdg_toplevel_add_listener(xdg_toplevel, &xdg_toplevel_listener, NULL);
    xdg_toplevel_set_title(xdg_toplevel, "toplevel-drag-test");
    wl_surface_commit(surface);

    while (wl_display_dispatch(display) != -1) { }
    return 0;
}
