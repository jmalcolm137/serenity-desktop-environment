/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Scripted client for the R4.1 session safety net (docs/11-r4-implementation-plan.md):
//
//   --timeout MS   create an ext-idle-notify-v1 notification for the seat and
//                  print "idled"/"resumed" as the compositor reports them.
//   --inhibit      create a zwp_idle_inhibit_v1 inhibitor on a surface, so the
//                  compositor must not report the seat idle.
//   --dpms on|off  request output power via wlr-output-power-management.
//   --hold MS      how long to run before exiting (default 1500).
//
// The driver, scripts/run-idle-test.sh, asserts on the markers and the
// compositor log. Runs headless.

#define _POSIX_C_SOURCE 200809L

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>

#include "ext-idle-notify-v1-client-protocol.h"
#include "idle-inhibit-unstable-v1-client-protocol.h"
#include "wlr-output-power-management-unstable-v1-client-protocol.h"

static struct wl_compositor* compositor;
static struct wl_seat* seat;
static struct wl_output* output;
static struct ext_idle_notifier_v1* idle_notifier;
static struct zwp_idle_inhibit_manager_v1* inhibit_manager;
static struct zwlr_output_power_manager_v1* power_manager;

static void notif_idled(void*, struct ext_idle_notification_v1*)
{
    printf("IDLE-TEST: idled\n");
    fflush(stdout);
}

static void notif_resumed(void*, struct ext_idle_notification_v1*)
{
    printf("IDLE-TEST: resumed\n");
    fflush(stdout);
}

static struct ext_idle_notification_v1_listener const notif_listener = {
    .idled = notif_idled,
    .resumed = notif_resumed,
};

static void registry_global(void*, struct wl_registry* registry, uint32_t name, char const* interface, uint32_t version)
{
    if (strcmp(interface, wl_compositor_interface.name) == 0)
        compositor = wl_registry_bind(registry, name, &wl_compositor_interface, version < 4 ? version : 4);
    else if (strcmp(interface, wl_seat_interface.name) == 0)
        seat = wl_registry_bind(registry, name, &wl_seat_interface, version < 7 ? version : 7);
    else if (strcmp(interface, wl_output_interface.name) == 0 && !output)
        output = wl_registry_bind(registry, name, &wl_output_interface, version < 4 ? version : 4);
    else if (strcmp(interface, ext_idle_notifier_v1_interface.name) == 0)
        idle_notifier = wl_registry_bind(registry, name, &ext_idle_notifier_v1_interface, 1);
    else if (strcmp(interface, zwp_idle_inhibit_manager_v1_interface.name) == 0)
        inhibit_manager = wl_registry_bind(registry, name, &zwp_idle_inhibit_manager_v1_interface, 1);
    else if (strcmp(interface, zwlr_output_power_manager_v1_interface.name) == 0)
        power_manager = wl_registry_bind(registry, name, &zwlr_output_power_manager_v1_interface, 1);
}

static void registry_global_remove(void*, struct wl_registry*, uint32_t) { }

static struct wl_registry_listener const registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

int main(int argc, char** argv)
{
    int timeout = -1;
    int hold = 1500;
    bool inhibit = false;
    int dpms = -1; // 0 off, 1 on

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc)
            timeout = atoi(argv[++i]);
        else if (strcmp(argv[i], "--hold") == 0 && i + 1 < argc)
            hold = atoi(argv[++i]);
        else if (strcmp(argv[i], "--inhibit") == 0)
            inhibit = true;
        else if (strcmp(argv[i], "--dpms") == 0 && i + 1 < argc)
            dpms = strcmp(argv[++i], "on") == 0 ? 1 : 0;
        else {
            fprintf(stderr, "idle-test: unknown argument '%s'\n", argv[i]);
            return 2;
        }
    }

    struct wl_display* display = wl_display_connect(nullptr);
    if (!display) {
        fprintf(stderr, "idle-test: cannot connect to the compositor\n");
        return 1;
    }

    struct wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, nullptr);
    wl_display_roundtrip(display);

    if (!compositor || !seat) {
        fprintf(stderr, "idle-test: missing wl_compositor/wl_seat\n");
        return 1;
    }

    if (inhibit) {
        if (!inhibit_manager) {
            fprintf(stderr, "idle-test: no zwp_idle_inhibit_manager_v1\n");
            return 1;
        }
        struct wl_surface* surface = wl_compositor_create_surface(compositor);
        zwp_idle_inhibit_manager_v1_create_inhibitor(inhibit_manager, surface);
        wl_surface_commit(surface);
        printf("IDLE-TEST: inhibited\n");
        fflush(stdout);
    }

    if (timeout >= 0) {
        if (!idle_notifier) {
            fprintf(stderr, "idle-test: no ext_idle_notifier_v1\n");
            return 1;
        }
        struct ext_idle_notification_v1* notif = ext_idle_notifier_v1_get_idle_notification(idle_notifier, (uint32_t)timeout, seat);
        ext_idle_notification_v1_add_listener(notif, &notif_listener, nullptr);
        printf("IDLE-TEST: notification timeout=%d\n", timeout);
        fflush(stdout);
    }

    if (dpms >= 0) {
        if (!power_manager || !output) {
            fprintf(stderr, "idle-test: no output power manager/output\n");
            return 1;
        }
        struct zwlr_output_power_v1* power = zwlr_output_power_manager_v1_get_output_power(power_manager, output);
        zwlr_output_power_v1_set_mode(power, dpms ? ZWLR_OUTPUT_POWER_V1_MODE_ON : ZWLR_OUTPUT_POWER_V1_MODE_OFF);
        printf("IDLE-TEST: dpms %s\n", dpms ? "on" : "off");
        fflush(stdout);
    }

    wl_display_flush(display);

    for (int elapsed = 0; elapsed < hold; elapsed += 25) {
        while (wl_display_prepare_read(display) != 0)
            wl_display_dispatch_pending(display);
        wl_display_flush(display);

        struct pollfd pfd = { .fd = wl_display_get_fd(display), .events = POLLIN };
        if (poll(&pfd, 1, 25) > 0 && (pfd.revents & POLLIN))
            wl_display_read_events(display);
        else
            wl_display_cancel_read(display);
        wl_display_dispatch_pending(display);
    }

    wl_display_disconnect(display);
    return 0;
}
