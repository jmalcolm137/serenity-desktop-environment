/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include "pointer-warp-v1-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

static struct wl_display* display;
static struct wl_registry* registry;
static struct wl_compositor* compositor;
static struct wl_shm* shm;
static struct zwlr_layer_shell_v1* layer_shell;
static struct wl_seat* seat;
static struct wl_pointer* pointer;
static struct wp_pointer_warp_v1* warp;
static struct wl_surface* surface;
static struct zwlr_layer_surface_v1* layer_surface;
static int warped = 0;
static uint32_t enter_serial = 0;

static void registry_global(void*, struct wl_registry* registry, uint32_t name, const char* interface, uint32_t version)
{
    if (strcmp(interface, wl_compositor_interface.name) == 0)
        compositor = wl_registry_bind(registry, name, &wl_compositor_interface, version < 4 ? version : 4);
    else if (strcmp(interface, wl_shm_interface.name) == 0)
        shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0)
        layer_shell = wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, version < 4 ? version : 4);
    else if (strcmp(interface, wl_seat_interface.name) == 0)
        seat = wl_registry_bind(registry, name, &wl_seat_interface, version < 5 ? version : 5);
    else if (strcmp(interface, wp_pointer_warp_v1_interface.name) == 0)
        warp = wl_registry_bind(registry, name, &wp_pointer_warp_v1_interface, 1);
}

static void registry_global_remove(void*, struct wl_registry*, uint32_t) { }

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static struct wl_buffer* create_buffer(void)
{
    int const width = 200;
    int const height = 200;
    int const stride = width * 4;
    int const size = stride * height;
    int fd = memfd_create("pointer-warp-test", MFD_CLOEXEC);
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
    struct wl_buffer* buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    munmap(pixels, size);
    close(fd);
    return buffer;
}

static void layer_configure(void*, struct zwlr_layer_surface_v1* layer, uint32_t serial, uint32_t, uint32_t)
{
    zwlr_layer_surface_v1_ack_configure(layer, serial);
    struct wl_buffer* buffer = create_buffer();
    if (!buffer) {
        fprintf(stderr, "POINTER-WARP: failed to create the buffer\n");
        exit(1);
    }
    wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_damage(surface, 0, 0, 200, 200);
    wl_surface_commit(surface);
}

static void layer_closed(void*, struct zwlr_layer_surface_v1*)
{
    fprintf(stderr, "POINTER-WARP: the layer surface was closed\n");
    exit(1);
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
    .configure = layer_configure,
    .closed = layer_closed,
};

static void pointer_enter(void*, struct wl_pointer* pointer, uint32_t serial, struct wl_surface* entered, wl_fixed_t, wl_fixed_t)
{
    enter_serial = serial;
    if (warp && entered == surface) {
        wp_pointer_warp_v1_warp_pointer(warp, surface, pointer, wl_fixed_from_int(50), wl_fixed_from_int(50), serial);
        wl_display_flush(display);
    }
}

static void pointer_leave(void*, struct wl_pointer*, uint32_t, struct wl_surface*) { }
static void pointer_motion(void*, struct wl_pointer*, uint32_t, wl_fixed_t sx, wl_fixed_t sy)
{
    int x = wl_fixed_to_int(sx);
    int y = wl_fixed_to_int(sy);
    if (x == 50 && y == 50) {
        printf("POINTER-WARP: ok (motion %d,%d)\n", x, y);
        warped = 1;
        wl_display_flush(display);
    }
}
static void pointer_button(void*, struct wl_pointer*, uint32_t, uint32_t, uint32_t, uint32_t) { }
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
        fprintf(stderr, "POINTER-WARP: cannot connect to the compositor\n");
        return 1;
    }
    registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);

    if (!compositor || !shm || !layer_shell || !seat || !warp) {
        fprintf(stderr, "POINTER-WARP: missing globals (compositor=%p shm=%p layer=%p seat=%p warp=%p)\n",
            (void*)compositor, (void*)shm, (void*)layer_shell, (void*)seat, (void*)warp);
        return 1;
    }

    wl_seat_add_listener(seat, &seat_listener, NULL);
    surface = wl_compositor_create_surface(compositor);
    layer_surface = zwlr_layer_shell_v1_get_layer_surface(layer_shell, surface, NULL,
        ZWLR_LAYER_SHELL_V1_LAYER_TOP, "pointer-warp-test");
    zwlr_layer_surface_v1_add_listener(layer_surface, &layer_listener, NULL);
    zwlr_layer_surface_v1_set_size(layer_surface, 200, 200);
    zwlr_layer_surface_v1_set_anchor(layer_surface,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
    zwlr_layer_surface_v1_set_exclusive_zone(layer_surface, 0);
    zwlr_layer_surface_v1_set_keyboard_interactivity(layer_surface,
        ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
    wl_surface_commit(surface);

    while (!warped && wl_display_dispatch(display) != -1) { }

    if (warped)
        return 0;
    fprintf(stderr, "POINTER-WARP: timed out (enter serial %u)\n", enter_serial);
    return 1;
}
