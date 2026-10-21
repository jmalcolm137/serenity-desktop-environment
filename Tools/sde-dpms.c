/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// sde-dpms — set output power (DPMS) from the command line, so `sde-idle` can
// blank the screen before locking and restore it on resume (R4.1). Uses
// wlr-output-power-management; a plain Wayland client, so it is portable to any
// compositor implementing that protocol.
//
// Usage: sde-dpms {on|off} [output-name]

#define _POSIX_C_SOURCE 200809L

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>

#include "wlr-output-power-management-unstable-v1-client-protocol.h"

static struct wl_output* output;
static struct zwlr_output_power_manager_v1* power_manager;

static void registry_global(void* data, struct wl_registry* registry, uint32_t name, char const* interface, uint32_t version)
{
    (void)data;
    if (strcmp(interface, wl_output_interface.name) == 0 && !output)
        output = wl_registry_bind(registry, name, &wl_output_interface, version < 4 ? version : 4);
    else if (strcmp(interface, zwlr_output_power_manager_v1_interface.name) == 0)
        power_manager = wl_registry_bind(registry, name, &zwlr_output_power_manager_v1_interface, 1);
}

static void registry_global_remove(void* data, struct wl_registry* registry, uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}

static struct wl_registry_listener const registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

int main(int argc, char** argv)
{
    if (argc < 2 || (strcmp(argv[1], "on") != 0 && strcmp(argv[1], "off") != 0)) {
        fprintf(stderr, "usage: sde-dpms {on|off}\n");
        return 2;
    }
    bool const on = strcmp(argv[1], "on") == 0;

    struct wl_display* display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "sde-dpms: cannot connect to the compositor\n");
        return 1;
    }
    struct wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);

    if (!power_manager || !output) {
        fprintf(stderr, "sde-dpms: compositor lacks wlr-output-power-management or an output\n");
        wl_display_disconnect(display);
        return 1;
    }

    struct zwlr_output_power_v1* power = zwlr_output_power_manager_v1_get_output_power(power_manager, output);
    zwlr_output_power_v1_set_mode(power, on ? ZWLR_OUTPUT_POWER_V1_MODE_ON : ZWLR_OUTPUT_POWER_V1_MODE_OFF);
    wl_display_roundtrip(display);
    wl_display_disconnect(display);
    return 0;
}
