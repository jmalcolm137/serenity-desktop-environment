/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Prints every wl_registry global the compositor advertises, one per line:
//
//   global <name> <interface> v<version>
//
// Used by scripts/run-protocol-matrix-test.sh to assert the advertised protocol
// surface. Iterates the registry explicitly (and re-binds nothing) so it works
// even for protocol versions the client library does not know. Not part of the
// desktop: a test tool.

#define _GNU_SOURCE

#include <stdio.h>
#include <wayland-client.h>

static void registry_global(void* data, struct wl_registry* registry, uint32_t name, char const* interface, uint32_t version)
{
    (void)data;
    (void)registry;
    printf("global %u %s v%u\n", name, interface, version);
    fflush(stdout);
}

static void registry_global_remove(void* data, struct wl_registry* registry, uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}

static struct wl_registry_listener const registry_listener = { registry_global, registry_global_remove };

int main(void)
{
    struct wl_display* display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "wayland-globals-test: cannot connect to a compositor\n");
        return 1;
    }
    struct wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);
    wl_display_roundtrip(display);
    wl_display_disconnect(display);
    return 0;
}
