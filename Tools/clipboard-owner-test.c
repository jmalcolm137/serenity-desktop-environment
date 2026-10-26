/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// A test-only selection owner using ext-data-control-v1. It offers text/plain
// containing "clipboard-portal-ok" and keeps the source alive until killed.
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>

#include "ext-data-control-v1-client-protocol.h"

static struct wl_display* display;
static struct wl_seat* seat;
static struct ext_data_control_manager_v1* manager;
static struct ext_data_control_device_v1* device;
static struct ext_data_control_source_v1* source;
static int sent;

static void source_send(void* data, struct ext_data_control_source_v1* s, char const* mime_type, int32_t fd)
{
    (void)data;
    (void)s;
    if (mime_type && strcmp(mime_type, "text/plain") == 0) {
        (void)write(fd, "clipboard-portal-ok", 19);
        sent = 1;
        printf("clipboard-owner-test: served text/plain\n");
        fflush(stdout);
    }
    close(fd);
}

static void source_cancelled(void* data, struct ext_data_control_source_v1* s)
{
    (void)data;
    (void)s;
    printf("clipboard-owner-test: selection cancelled\n");
    fflush(stdout);
}

static struct ext_data_control_source_v1_listener source_listener = {
    .send = source_send,
    .cancelled = source_cancelled,
};

static void registry_global(void* data, struct wl_registry* registry, uint32_t name, char const* interface, uint32_t version)
{
    (void)data;
    if (strcmp(interface, wl_seat_interface.name) == 0)
        seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
    else if (strcmp(interface, ext_data_control_manager_v1_interface.name) == 0)
        manager = wl_registry_bind(registry, name, &ext_data_control_manager_v1_interface, version < 1 ? version : 1);
}

static void registry_remove(void* data, struct wl_registry* registry, uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}

static struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_remove,
};

int main(void)
{
    display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "clipboard-owner-test: no Wayland display\n");
        return 1;
    }
    struct wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);
    wl_display_roundtrip(display);
    if (!manager || !seat) {
        fprintf(stderr, "clipboard-owner-test: ext-data-control or seat unavailable\n");
        return 1;
    }
    device = ext_data_control_manager_v1_get_data_device(manager, seat);
    source = ext_data_control_manager_v1_create_data_source(manager);
    ext_data_control_source_v1_add_listener(source, &source_listener, NULL);
    ext_data_control_source_v1_offer(source, "text/plain");
    ext_data_control_device_v1_set_selection(device, source);
    wl_display_flush(display);
    printf("clipboard-owner-test: ready\n");
    fflush(stdout);
    while (!sent) {
        if (wl_display_dispatch(display) < 0)
            break;
    }
    sleep(15);
    return 0;
}
