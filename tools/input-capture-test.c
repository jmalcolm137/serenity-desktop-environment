/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// A minimal client for the private `serenity_input_capture` protocol
// (docs/17-input-capture.md, P1 — fd handoff). It creates a session, asks the
// compositor for the session's EIS file descriptor, connects a *libei receiver*
// context to it, and asserts the EIS connection is established
// (EI_EVENT_CONNECT). No input capture is enabled yet — P1 only proves the fd
// handoff works.
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

static struct wl_display* display;
static struct serenity_input_capture_manager_v1* manager;
static int eis_fd = -1;

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

static void session_eis_fd(void* data, struct serenity_input_capture_session_v1* session, int32_t fd)
{
    (void)data;
    (void)session;
    eis_fd = fd;
    printf("input-capture-test: received EIS fd %d\n", fd);
    fflush(stdout);
}

static struct serenity_input_capture_session_v1_listener const session_listener = { session_eis_fd };

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

    struct serenity_input_capture_session_v1* session =
        serenity_input_capture_manager_v1_create_session(manager, "input-capture-test");
    serenity_input_capture_session_v1_add_listener(session, &session_listener, NULL);
    serenity_input_capture_session_v1_connect_to_eis(session);

    // Wait for the compositor to hand back the EIS fd.
    while (eis_fd < 0) {
        if (wl_display_dispatch(display) < 0) {
            fprintf(stderr, "input-capture-test: Wayland connection closed before the EIS fd arrived\n");
            return 1;
        }
    }

    // A receiver (passive) libei context — the InputCapture role.
    struct ei* ei = ei_new_receiver(NULL);
    if (!ei) {
        fprintf(stderr, "input-capture-test: ei_new_receiver failed\n");
        return 1;
    }
    ei_configure_name(ei, "input-capture-test");
    if (ei_setup_backend_fd(ei, eis_fd) < 0) {
        fprintf(stderr, "input-capture-test: ei_setup_backend_fd failed\n");
        return 1;
    }

    int connected = 0;
    int disconnected = 0;
    struct pollfd fds[2] = {
        { .fd = wl_display_get_fd(display), .events = POLLIN },
        { .fd = ei_get_fd(ei), .events = POLLIN },
    };
    for (int i = 0; i < 100 && !connected && !disconnected; i++) {
        wl_display_dispatch_pending(display);
        wl_display_flush(display);
        if (poll(fds, 2, 100) <= 0)
            continue;

        if (fds[0].revents & POLLIN)
            wl_display_dispatch(display);

        if (fds[1].revents & POLLIN) {
            ei_dispatch(ei);
            struct ei_event* event;
            while ((event = ei_get_event(ei))) {
                enum ei_event_type type = ei_event_get_type(event);
                if (type == EI_EVENT_CONNECT) {
                    printf("input-capture-test: EIS connected\n");
                    fflush(stdout);
                    connected = 1;
                } else if (type == EI_EVENT_DISCONNECT) {
                    disconnected = 1;
                }
                ei_event_unref(event);
            }
        }
    }

    if (!connected)
        fprintf(stderr, "input-capture-test: did not reach EI_EVENT_CONNECT\n");
    return connected ? 0 : 1;
}
