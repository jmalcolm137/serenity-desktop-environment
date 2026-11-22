/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// A minimal client for the private `serenity_input_capture` protocol
// (docs/17-input-capture.md). It walks the whole P2 flow:
//
//   create_session -> connect_to_eis -> libei receiver -> seat/device
//   -> get_zones -> set_pointer_barriers -> enable
//   -> (pointer crosses the barrier) -> activated + EI pointer motion
//
// It installs one valid vertical barrier and one deliberately diagonal
// (invalid) barrier, and asserts the invalid one is rejected. When the pointer
// crosses the valid barrier the compositor activates capture; the client then
// receives the `activated` event and EI pointer motion.
//
// Not part of the desktop: a test tool.

#define _GNU_SOURCE
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>

#include <libei.h>

#include "serenity-input-capture-client-protocol.h"

struct zone {
    uint32_t width;
    uint32_t height;
    int32_t x;
    int32_t y;
};

struct barrier {
    uint32_t id;
    int32_t x1;
    int32_t y1;
    int32_t x2;
    int32_t y2;
};

static struct wl_display* display;
static struct serenity_input_capture_manager_v1* manager;
static struct serenity_input_capture_session_v1* session;
static struct ei* ei;

static int eis_fd = -1;
static int got_seat = 0;
static int zones_requested = 0;
static int armed = 0;
static int zones_seen = 0;
static int failed_barriers = -1;
static int got_activated = 0;
static int got_motion = 0;
static int got_key = 0;

static void registry_global(void* data, struct wl_registry* registry, uint32_t name, char const* interface, uint32_t version)
{
    (void)data;
    (void)version;
    if (strcmp(interface, serenity_input_capture_manager_v1_interface.name) == 0)
        manager = wl_registry_bind(registry, name, &serenity_input_capture_manager_v1_interface, 1);
}

static void registry_global_remove(void* data, struct wl_registry* registry, uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}

static struct wl_registry_listener const registry_listener = { registry_global, registry_global_remove };

static void arm(struct zone const* z)
{
    struct wl_array barriers;
    wl_array_init(&barriers);

    int32_t const x = z->x + (int32_t)z->width / 2;
    struct barrier valid = { .id = 1, .x1 = x, .y1 = z->y, .x2 = x, .y2 = z->y + (int32_t)z->height - 1 };
    struct barrier diagonal = { .id = 2, .x1 = z->x, .y1 = z->y, .x2 = z->x + 100, .y2 = z->y + 100 };
    memcpy(wl_array_add(&barriers, sizeof(valid)), &valid, sizeof(valid));
    memcpy(wl_array_add(&barriers, sizeof(diagonal)), &diagonal, sizeof(diagonal));

    printf("input-capture-test: barrier at x=%d y=%d..%d\n", x, valid.y1, valid.y2);
    serenity_input_capture_session_v1_set_pointer_barriers(session, &barriers, 1);
    wl_array_release(&barriers);
    serenity_input_capture_session_v1_enable(session, 3 /* KEYBOARD | POINTER */);
    armed = 1;
}

static void session_eis_fd(void* data, struct serenity_input_capture_session_v1* s, int32_t fd)
{
    (void)data;
    (void)s;
    eis_fd = fd;
    printf("input-capture-test: received EIS fd %d\n", fd);
    fflush(stdout);
}

static void session_zones(void* data, struct serenity_input_capture_session_v1* s, struct wl_array* zones, uint32_t zone_set)
{
    (void)data;
    (void)s;
    size_t const count = zones->size / sizeof(struct zone);
    printf("input-capture-test: %zu zone(s), zone_set=%u\n", count, zone_set);
    for (size_t i = 0; i < count; i++) {
        struct zone z;
        memcpy(&z, (char*)zones->data + i * sizeof(struct zone), sizeof(z));
        printf("  zone %zu: %ux%u at %d,%d\n", i, z.width, z.height, z.x, z.y);
    }
    zones_seen = 1;
    if (count > 0 && !armed) {
        struct zone first;
        memcpy(&first, zones->data, sizeof(first));
        arm(&first);
    }
    fflush(stdout);
}

static void session_zones_changed(void* data, struct serenity_input_capture_session_v1* s)
{
    (void)data;
    (void)s;
    printf("input-capture-test: zones changed\n");
}

static void session_barriers_result(void* data, struct serenity_input_capture_session_v1* s, struct wl_array* failed)
{
    (void)data;
    (void)s;
    failed_barriers = (int)(failed->size / sizeof(uint32_t));
    printf("input-capture-test: %d barrier(s) rejected", failed_barriers);
    for (size_t i = 0; i < failed->size / sizeof(uint32_t); i++)
        printf(" %u", ((uint32_t*)failed->data)[i]);
    printf("\n");
    fflush(stdout);
}

static void session_activated(void* data, struct serenity_input_capture_session_v1* s, uint32_t activation_id, wl_fixed_t cursor_x, wl_fixed_t cursor_y, uint32_t barrier_id)
{
    (void)data;
    (void)s;
    got_activated = 1;
    printf("input-capture-test: activated activation_id=%u barrier=%u at %.1f,%.1f\n",
        activation_id, barrier_id, wl_fixed_to_double(cursor_x), wl_fixed_to_double(cursor_y));
    fflush(stdout);
}

static void session_deactivated(void* data, struct serenity_input_capture_session_v1* s, uint32_t activation_id, wl_fixed_t cursor_x, wl_fixed_t cursor_y)
{
    (void)data;
    (void)s;
    printf("input-capture-test: deactivated activation_id=%u at %.1f,%.1f\n", activation_id, wl_fixed_to_double(cursor_x), wl_fixed_to_double(cursor_y));
    fflush(stdout);
}

static void session_disabled(void* data, struct serenity_input_capture_session_v1* s)
{
    (void)data;
    (void)s;
    printf("input-capture-test: disabled\n");
}

static struct serenity_input_capture_session_v1_listener const session_listener = {
    .eis_fd = session_eis_fd,
    .zones = session_zones,
    .zones_changed = session_zones_changed,
    .pointer_barriers_result = session_barriers_result,
    .activated = session_activated,
    .deactivated = session_deactivated,
    .disabled = session_disabled,
};

static void handle_ei_events(void)
{
    ei_dispatch(ei);
    struct ei_event* event;
    while ((event = ei_get_event(ei))) {
        switch (ei_event_get_type(event)) {
        case EI_EVENT_CONNECT:
            printf("input-capture-test: EIS connected\n");
            break;
        case EI_EVENT_SEAT_ADDED: {
            struct ei_seat* seat = ei_event_get_seat(event);
            ei_seat_bind_capabilities(seat, EI_DEVICE_CAP_POINTER, EI_DEVICE_CAP_BUTTON, EI_DEVICE_CAP_KEYBOARD, NULL);
            got_seat = 1;
            break;
        }
        case EI_EVENT_DEVICE_ADDED:
            printf("input-capture-test: EIS device added\n");
            if (!zones_requested) {
                serenity_input_capture_session_v1_get_zones(session);
                zones_requested = 1;
            }
            break;
        case EI_EVENT_POINTER_MOTION: {
            double const dx = ei_event_pointer_get_dx(event);
            double const dy = ei_event_pointer_get_dy(event);
            printf("input-capture-test: motion dx=%.1f dy=%.1f\n", dx, dy);
            got_motion = 1;
            break;
        }
        case EI_EVENT_KEYBOARD_KEY: {
            uint32_t const key = ei_event_keyboard_get_key(event);
            int const press = ei_event_keyboard_get_key_is_press(event);
            printf("input-capture-test: keyboard key code=%u %s\n", key, press ? "press" : "release");
            got_key = 1;
            break;
        }
        default:
            break;
        }
        ei_event_unref(event);
    }
    fflush(stdout);
}

int main(void)
{
    display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "input-capture-test: cannot connect to a compositor\n");
        return 1;
    }

    struct wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);

    if (!manager) {
        fprintf(stderr, "input-capture-test: compositor does not offer serenity_input_capture (is SDE_INPUT_CAPTURE=1 set?)\n");
        return 2;
    }

    session = serenity_input_capture_manager_v1_create_session(manager, "input-capture-test");
    serenity_input_capture_session_v1_add_listener(session, &session_listener, NULL);
    serenity_input_capture_session_v1_connect_to_eis(session);

    while (eis_fd < 0) {
        if (wl_display_dispatch(display) < 0) {
            fprintf(stderr, "input-capture-test: connection closed before the EIS fd arrived\n");
            return 1;
        }
    }

    ei = ei_new_receiver(NULL);
    ei_configure_name(ei, "input-capture-test");
    if (!ei || ei_setup_backend_fd(ei, eis_fd) < 0) {
        fprintf(stderr, "input-capture-test: could not set up the libei receiver\n");
        return 1;
    }

    struct pollfd fds[2] = {
        { .fd = wl_display_get_fd(display), .events = POLLIN },
        { .fd = ei_get_fd(ei), .events = POLLIN },
    };
    for (int i = 0; i < 80 && !(got_activated && got_motion && got_key); i++) {
        wl_display_dispatch_pending(display);
        wl_display_flush(display);
        if (poll(fds, 2, 100) <= 0)
            continue;
        if (fds[0].revents & POLLIN)
            wl_display_dispatch(display);
        if (fds[1].revents & POLLIN)
            handle_ei_events();
    }

    printf("input-capture-test: summary seat=%d zones=%d failed_barriers=%d activated=%d motion=%d key=%d\n",
        got_seat, zones_seen, failed_barriers, got_activated, got_motion, got_key);
    fflush(stdout);

    // Pass when the full chain worked: a seat, zones, the invalid barrier
    // rejected, activation triggered, pointer motion and a keyboard key delivered.
    return (got_seat && zones_seen && failed_barriers == 1 && got_activated && got_motion && got_key) ? 0 : 1;
}
