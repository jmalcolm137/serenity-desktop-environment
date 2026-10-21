/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// output-mode-test — set the (first) output to a custom mode via
// wlr-output-management, so tests can change an output's size at runtime (e.g.
// to check that a session lock surface reconfigures). Plain Wayland client.
//
// Usage: output-mode-test <WxH>   e.g. 1024x768

#define _POSIX_C_SOURCE 200809L

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>

#include "wlr-output-management-unstable-v1-client-protocol.h"

static struct zwlr_output_manager_v1* manager;
static struct zwlr_output_head_v1* head;
static uint32_t manager_serial = 0;
static int want_w = 0;
static int want_h = 0;
static bool applied = false;

static void mode_size(void* d, struct zwlr_output_mode_v1* m, int32_t w, int32_t h) { (void)d; (void)m; (void)w; (void)h; }
static void mode_refresh(void* d, struct zwlr_output_mode_v1* m, int32_t r) { (void)d; (void)m; (void)r; }
static void mode_preferred(void* d, struct zwlr_output_mode_v1* m) { (void)d; (void)m; }
static void mode_finished(void* d, struct zwlr_output_mode_v1* m) { (void)d; (void)m; }
static const struct zwlr_output_mode_v1_listener mode_listener = {
    .size = mode_size, .refresh = mode_refresh, .preferred = mode_preferred, .finished = mode_finished,
};

static void head_name(void* d, struct zwlr_output_head_v1* h, const char* n) { (void)d; (void)h; (void)n; }
static void head_description(void* d, struct zwlr_output_head_v1* h, const char* n) { (void)d; (void)h; (void)n; }
static void head_physical_size(void* d, struct zwlr_output_head_v1* h, int32_t w, int32_t hh) { (void)d; (void)h; (void)w; (void)hh; }
static void head_mode(void* d, struct zwlr_output_head_v1* h, struct zwlr_output_mode_v1* m)
{
    (void)d;
    (void)h;
    zwlr_output_mode_v1_add_listener(m, &mode_listener, NULL);
}
static void head_enabled(void* d, struct zwlr_output_head_v1* h, int32_t e) { (void)d; (void)h; (void)e; }
static void head_current_mode(void* d, struct zwlr_output_head_v1* h, struct zwlr_output_mode_v1* m) { (void)d; (void)h; (void)m; }
static void head_position(void* d, struct zwlr_output_head_v1* h, int32_t x, int32_t y) { (void)d; (void)h; (void)x; (void)y; }
static void head_transform(void* d, struct zwlr_output_head_v1* h, int32_t t) { (void)d; (void)h; (void)t; }
static void head_scale(void* d, struct zwlr_output_head_v1* h, wl_fixed_t s) { (void)d; (void)h; (void)s; }
static void head_finished(void* d, struct zwlr_output_head_v1* h) { (void)d; (void)h; }
static void head_make(void* d, struct zwlr_output_head_v1* h, const char* n) { (void)d; (void)h; (void)n; }
static void head_model(void* d, struct zwlr_output_head_v1* h, const char* n) { (void)d; (void)h; (void)n; }
static void head_serial_number(void* d, struct zwlr_output_head_v1* h, const char* n) { (void)d; (void)h; (void)n; }
static void head_adaptive_sync(void* d, struct zwlr_output_head_v1* h, uint32_t s) { (void)d; (void)h; (void)s; }
static const struct zwlr_output_head_v1_listener head_listener = {
    .name = head_name, .description = head_description, .physical_size = head_physical_size,
    .mode = head_mode, .enabled = head_enabled, .current_mode = head_current_mode,
    .position = head_position, .transform = head_transform, .scale = head_scale, .finished = head_finished,
    .make = head_make, .model = head_model, .serial_number = head_serial_number, .adaptive_sync = head_adaptive_sync,
};

static void config_succeeded(void* d, struct zwlr_output_configuration_v1* c) { (void)d; printf("OUTPUT-MODE: succeeded\n"); fflush(stdout); zwlr_output_configuration_v1_destroy(c); }
static void config_failed(void* d, struct zwlr_output_configuration_v1* c) { (void)d; printf("OUTPUT-MODE: failed\n"); fflush(stdout); zwlr_output_configuration_v1_destroy(c); }
static void config_cancelled(void* d, struct zwlr_output_configuration_v1* c) { (void)d; printf("OUTPUT-MODE: cancelled\n"); fflush(stdout); zwlr_output_configuration_v1_destroy(c); }
static const struct zwlr_output_configuration_v1_listener config_listener = {
    .succeeded = config_succeeded, .failed = config_failed, .cancelled = config_cancelled,
};

static void manager_head(void* d, struct zwlr_output_manager_v1* m, struct zwlr_output_head_v1* h)
{
    (void)d;
    (void)m;
    if (!head) {
        head = h;
        zwlr_output_head_v1_add_listener(h, &head_listener, NULL);
    }
}

static void manager_done(void* d, struct zwlr_output_manager_v1* m, uint32_t serial)
{
    (void)d;
    manager_serial = serial;
    if (!head || applied)
        return;
    applied = true;
    struct zwlr_output_configuration_v1* config = zwlr_output_manager_v1_create_configuration(m, manager_serial);
    zwlr_output_configuration_v1_add_listener(config, &config_listener, NULL);
    struct zwlr_output_configuration_head_v1* head_config = zwlr_output_configuration_v1_enable_head(config, head);
    zwlr_output_configuration_head_v1_set_custom_mode(head_config, want_w, want_h, 60000);
    zwlr_output_configuration_v1_apply(config);
}

static void manager_finished(void* d, struct zwlr_output_manager_v1* m) { (void)d; (void)m; }
static const struct zwlr_output_manager_v1_listener manager_listener = {
    .head = manager_head, .done = manager_done, .finished = manager_finished,
};

static void registry_global(void* d, struct wl_registry* registry, uint32_t name, const char* interface, uint32_t version)
{
    (void)d;
    if (strcmp(interface, zwlr_output_manager_v1_interface.name) == 0) {
        manager = wl_registry_bind(registry, name, &zwlr_output_manager_v1_interface, version < 4 ? version : 4);
        zwlr_output_manager_v1_add_listener(manager, &manager_listener, NULL);
    }
}
static void registry_remove(void* d, struct wl_registry* registry, uint32_t name) { (void)d; (void)registry; (void)name; }
static const struct wl_registry_listener registry_listener = { .global = registry_global, .global_remove = registry_remove };

int main(int argc, char** argv)
{
    if (argc < 2 || sscanf(argv[1], "%dx%d", &want_w, &want_h) != 2) {
        fprintf(stderr, "usage: output-mode-test <WxH>\n");
        return 2;
    }

    struct wl_display* display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "output-mode-test: cannot connect\n");
        return 1;
    }
    struct wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display); // bind the manager
    wl_display_roundtrip(display); // head details + done
    // Flush the apply and wait for the succeeded/failed result.
    wl_display_roundtrip(display);
    wl_display_roundtrip(display);

    wl_display_disconnect(display);
    return 0;
}
