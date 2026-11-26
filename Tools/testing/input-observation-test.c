/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// ext-input-observation-v1: the pointer position relative to a surface, inside
// and outside it. The surface is a 200x200 top-left layer surface, so the
// origin is (0,0): the virtual pointer's (100,100) is (0.5,0.5) and (400,300)
// is (2.0,1.5), i.e. (32768,32768) and (131072,98304) in 16:16 fixed point.
// The second point is outside the surface and over no surface at all, which is
// exactly what wl_pointer cannot report.

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

#include "ext-input-observation-v1-client-protocol.h"
#include "serenity-window-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

static struct wl_display* display;
static struct wl_registry* registry;
static struct wl_compositor* compositor;
static struct wl_shm* shm;
static struct zwlr_layer_shell_v1* layer_shell;
static struct wl_seat* seat;
static struct wl_pointer* pointer;
static struct ext_input_observation_manager_v1* observation_manager;
static struct serenity_window_manager* serenity_manager;
static struct wl_surface* surface;
static struct zwlr_layer_surface_v1* layer_surface;
static struct ext_input_position_observer_v1* observer;
static int saw_inside = 0;
static int saw_outside = 0;

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
    else if (strcmp(interface, ext_input_observation_manager_v1_interface.name) == 0)
        observation_manager = wl_registry_bind(registry, name, &ext_input_observation_manager_v1_interface, 1);
    else if (strcmp(interface, serenity_window_manager_interface.name) == 0)
        serenity_manager = wl_registry_bind(registry, name, &serenity_window_manager_interface, version < 4 ? version : 4);
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
    int fd = memfd_create("input-observation-test", MFD_CLOEXEC);
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
        fprintf(stderr, "INPUT-OBSERVATION: failed to create the buffer\n");
        exit(1);
    }
    wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_damage(surface, 0, 0, 200, 200);
    wl_surface_commit(surface);
}

static void layer_closed(void*, struct zwlr_layer_surface_v1*)
{
    fprintf(stderr, "INPUT-OBSERVATION: the layer surface was closed\n");
    exit(1);
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
    .configure = layer_configure,
    .closed = layer_closed,
};

static void observer_position(void*, struct ext_input_position_observer_v1*, struct wl_surface* observed,
    uint32_t instance, int32_t relative_x, int32_t relative_y)
{
    printf("INPUT-OBSERVATION: instance=%u rel=%d,%d\n", instance, relative_x, relative_y);
    fflush(stdout);
    if (relative_x == 32768 && relative_y == 32768)
        saw_inside = 1;
    if (relative_x == 131072 && relative_y == 98304)
        saw_outside = 1;
    (void)observed;
}

static const struct ext_input_position_observer_v1_listener observer_listener = {
    .position = observer_position,
};

static void pointer_enter(void*, struct wl_pointer*, uint32_t, struct wl_surface*, wl_fixed_t, wl_fixed_t) { }
static void pointer_leave(void*, struct wl_pointer*, uint32_t, struct wl_surface*) { }
static void pointer_motion(void*, struct wl_pointer*, uint32_t, wl_fixed_t, wl_fixed_t) { }
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
        fprintf(stderr, "INPUT-OBSERVATION: cannot connect to the compositor\n");
        return 1;
    }
    registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);

    // The observer is privileged: a client must already speak the session's
    // serenity-window protocol (LibWM binds it for every application).
    if (!compositor || !shm || !layer_shell || !seat || !observation_manager || !serenity_manager) {
        fprintf(stderr, "INPUT-OBSERVATION: missing globals (compositor=%p shm=%p layer=%p seat=%p observation=%p serenity=%p)\n",
            (void*)compositor, (void*)shm, (void*)layer_shell, (void*)seat,
            (void*)observation_manager, (void*)serenity_manager);
        return 1;
    }

    wl_seat_add_listener(seat, &seat_listener, NULL);
    surface = wl_compositor_create_surface(compositor);
    layer_surface = zwlr_layer_shell_v1_get_layer_surface(layer_shell, surface, NULL,
        ZWLR_LAYER_SHELL_V1_LAYER_TOP, "input-observation-test");
    zwlr_layer_surface_v1_add_listener(layer_surface, &layer_listener, NULL);
    zwlr_layer_surface_v1_set_size(layer_surface, 200, 200);
    zwlr_layer_surface_v1_set_anchor(layer_surface,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
    zwlr_layer_surface_v1_set_exclusive_zone(layer_surface, 0);
    zwlr_layer_surface_v1_set_keyboard_interactivity(layer_surface,
        ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
    wl_surface_commit(surface);

    // The seat only gains a pointer when an input device connects (the test
    // harness adds a virtual one), so wait for the capability before creating
    // the observer -- the observer request needs a wl_pointer.
    while (!pointer && wl_display_dispatch(display) != -1) { }
    if (!pointer) {
        fprintf(stderr, "INPUT-OBSERVATION: the seat has no pointer\n");
        return 1;
    }

    observer = ext_input_observation_manager_v1_create_pointer_observer(observation_manager, pointer, surface);
    if (!observer) {
        fprintf(stderr, "INPUT-OBSERVATION: could not create the observer\n");
        return 1;
    }
    ext_input_position_observer_v1_add_listener(observer, &observer_listener, NULL);
    wl_display_flush(display);

    while (!(saw_inside && saw_outside) && wl_display_dispatch(display) != -1) { }

    if (saw_inside && saw_outside) {
        printf("INPUT-OBSERVATION: ok\n");
        return 0;
    }
    fprintf(stderr, "INPUT-OBSERVATION: timed out (inside=%d outside=%d)\n", saw_inside, saw_outside);
    return 1;
}
