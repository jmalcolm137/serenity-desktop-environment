/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// sde-idle — a small, portable idle-action daemon (R4.2,
// docs/11-r4-implementation-plan.md). It is a plain ext-idle-notify-v1 client,
// so it runs under **any** compositor that implements the protocol (SDE, sway,
// KWin, Hyprland, …) — the same shape as `swayidle`. SerenityOS has no separate
// idle daemon, so we provide one rather than depending on another ecosystem.
//
// Usage:
//   sde-idle [timeout <seconds> <command>]...
//
// Each `timeout` runs <command> (via `/bin/sh -c`) when the seat has been idle
// for <seconds>; a `resumed` event is ignored. Example, lock after 5 minutes:
//
//   sde-idle timeout 300 "swaylock -f"
//
// (`zwp_idle_inhibit_v1` is honoured by the compositor, so e.g. a video player
// can keep the session awake.)

#define _POSIX_C_SOURCE 200809L

#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>

#include "ext-idle-notify-v1-client-protocol.h"

static struct wl_seat* seat;
static struct ext_idle_notifier_v1* notifier;
static volatile sig_atomic_t s_running = 1;
static char* s_resume_command = NULL;

static void handle_idled(void* data, struct ext_idle_notification_v1* notification)
{
    (void)notification;
    char const* command = data;
    fprintf(stderr, "sde-idle: idle; running: %s\n", command);
    fflush(stderr);
    int status = system(command);
    (void)status;
}

static void handle_resumed(void* data, struct ext_idle_notification_v1* notification)
{
    (void)data;
    (void)notification;
    if (s_resume_command) {
        fprintf(stderr, "sde-idle: resumed; running: %s\n", s_resume_command);
        fflush(stderr);
        int status = system(s_resume_command);
        (void)status;
    }
}

static struct ext_idle_notification_v1_listener const s_listener = {
    .idled = handle_idled,
    .resumed = handle_resumed,
};

static void registry_global(void* data, struct wl_registry* registry, uint32_t name, char const* interface, uint32_t version)
{
    (void)data;
    if (strcmp(interface, wl_seat_interface.name) == 0)
        seat = wl_registry_bind(registry, name, &wl_seat_interface, version < 7 ? version : 7);
    else if (strcmp(interface, ext_idle_notifier_v1_interface.name) == 0)
        notifier = wl_registry_bind(registry, name, &ext_idle_notifier_v1_interface, 1);
}

static void registry_global_remove(void* data, struct wl_registry* registry, uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}

static struct wl_registry_listener const s_registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static void on_signal(int signum)
{
    (void)signum;
    s_running = 0;
}

int main(int argc, char** argv)
{
    /* Parse `timeout <seconds> <command>` pairs. */
    static uint32_t timeouts[64];
    static char* commands[64];
    int count = 0;

    for (int i = 1; i < argc;) {
        if (strcmp(argv[i], "timeout") == 0 && i + 2 < argc && count < 64) {
            timeouts[count] = (uint32_t)(atoi(argv[i + 1]) * 1000);
            commands[count] = argv[i + 2];
            ++count;
            i += 3;
        } else if (strcmp(argv[i], "resume") == 0 && i + 1 < argc && s_resume_command == NULL) {
            s_resume_command = argv[i + 1];
            i += 2;
        } else {
            fprintf(stderr, "usage: sde-idle [timeout <seconds> <command>]... [resume <command>]\n");
            return 2;
        }
    }
    if (count == 0) {
        fprintf(stderr, "sde-idle: no timeouts given\n");
        return 2;
    }

    struct wl_display* display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "sde-idle: cannot connect to the compositor\n");
        return 1;
    }

    struct wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &s_registry_listener, NULL);
    wl_display_roundtrip(display);

    if (!seat || !notifier) {
        fprintf(stderr, "sde-idle: compositor lacks wl_seat or ext-idle-notify-v1\n");
        return 1;
    }

    for (int i = 0; i < count; ++i) {
        struct ext_idle_notification_v1* notification = ext_idle_notifier_v1_get_idle_notification(notifier, timeouts[i], seat);
        ext_idle_notification_v1_add_listener(notification, &s_listener, commands[i]);
        fprintf(stderr, "sde-idle: armed timeout %us -> %s\n", timeouts[i] / 1000, commands[i]);
    }
    wl_display_flush(display);

    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);

    while (s_running) {
        while (wl_display_prepare_read(display) != 0)
            wl_display_dispatch_pending(display);
        wl_display_flush(display);

        struct pollfd pfd = { .fd = wl_display_get_fd(display), .events = POLLIN };
        if (poll(&pfd, 1, 200) > 0 && (pfd.revents & POLLIN))
            wl_display_read_events(display);
        else
            wl_display_cancel_read(display);
        wl_display_dispatch_pending(display);
    }

    wl_display_disconnect(display);
    return 0;
}
