/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// A minimal zwp_input_method_v2 client, so the headless IME test can assert the
// compositor relays text-input state to an input method and its preedit/commit
// back to the field. On activation it sends a preedit, commits a string, requests
// the keyboard grab and draws a candidate popup; it prints everything it observes:
//
//   input-method-test: activate
//   input-method-test: surrounding "ab" 2 2
//   input-method-test: done
//   input-method-test: key 30 1
//   input-method-test: popup rect ...
//
// Usage: input-method-test [--duration MS]
//
// Not part of the desktop: a test tool.

#define _GNU_SOURCE
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include "input-method-unstable-v2-client-protocol.h"

static struct wl_display* display;
static struct wl_compositor* compositor;
static struct wl_shm* shm;
static struct wl_seat* seat;
static struct zwp_input_method_manager_v2* input_method_manager;
static struct zwp_input_method_v2* input_method;
static struct zwp_input_method_keyboard_grab_v2* keyboard_grab;
static struct zwp_input_popup_surface_v2* popup;

static uint32_t done_count;
static int sent_preedit;
static int committed;

static struct wl_buffer* create_buffer(int w, int h, uint32_t color)
{
    int stride = w * 4;
    int size = stride * h;
    int fd = memfd_create("input-method-test", 0);
    if (fd < 0) {
        perror("memfd_create");
        exit(1);
    }
    if (ftruncate(fd, size) < 0) {
        perror("ftruncate");
        exit(1);
    }
    void* data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }
    for (int i = 0; i < w * h; i++)
        ((uint32_t*)data)[i] = color;

    struct wl_shm_pool* pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer* buffer = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return buffer;
}

// --- keyboard grab ----------------------------------------------------------

static void grab_keymap(void* data, struct zwp_input_method_keyboard_grab_v2* grab, uint32_t format, int32_t fd, uint32_t size)
{
    (void)data;
    (void)grab;
    (void)format;
    (void)size;
    printf("input-method-test: grab keymap\n");
    fflush(stdout);
    close(fd);
}

static void grab_key(void* data, struct zwp_input_method_keyboard_grab_v2* grab, uint32_t serial, uint32_t time, uint32_t key, uint32_t state)
{
    (void)data;
    (void)grab;
    (void)serial;
    (void)time;
    printf("input-method-test: key %u %u\n", key, state);
    fflush(stdout);
}

static void grab_modifiers(void* data, struct zwp_input_method_keyboard_grab_v2* grab, uint32_t serial, uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group)
{
    (void)data;
    (void)grab;
    (void)serial;
    (void)latched;
    (void)locked;
    (void)group;
    printf("input-method-test: modifiers depressed=%u\n", depressed);
    fflush(stdout);
}

static void grab_repeat_info(void* data, struct zwp_input_method_keyboard_grab_v2* grab, int32_t rate, int32_t delay)
{
    (void)data;
    (void)grab;
    (void)rate;
    (void)delay;
}

static struct zwp_input_method_keyboard_grab_v2_listener const grab_listener = {
    grab_keymap, grab_key, grab_modifiers, grab_repeat_info,
};

// --- input method -----------------------------------------------------------

static void im_activate(void* data, struct zwp_input_method_v2* im)
{
    (void)data;
    (void)im;
    printf("input-method-test: activate\n");
    fflush(stdout);
    // Nothing is committed here: the serial in a commit must equal the number of
    // done events received, so we wait for the `done` that follows `activate`.
}

static void im_deactivate(void* data, struct zwp_input_method_v2* im)
{
    (void)data;
    (void)im;
    printf("input-method-test: deactivate\n");
    fflush(stdout);
}

static void im_surrounding_text(void* data, struct zwp_input_method_v2* im, const char* text, uint32_t cursor, uint32_t anchor)
{
    (void)data;
    (void)im;
    printf("input-method-test: surrounding \"%s\" %u %u\n", text ? text : "", cursor, anchor);
    fflush(stdout);
}

static void im_text_change_cause(void* data, struct zwp_input_method_v2* im, uint32_t cause)
{
    (void)data;
    (void)im;
    printf("input-method-test: text_change_cause %u\n", cause);
    fflush(stdout);
}

static void im_content_type(void* data, struct zwp_input_method_v2* im, uint32_t hint, uint32_t purpose)
{
    (void)data;
    (void)im;
    printf("input-method-test: content_type hint=%u purpose=%u\n", hint, purpose);
    fflush(stdout);
}

static void im_done(void* data, struct zwp_input_method_v2* im)
{
    (void)data;
    done_count++;
    printf("input-method-test: done\n");
    fflush(stdout);
    // The state is applied now, so a commit with the current serial is valid:
    // first a composing preedit, then the finished text (as a real IME would).
    if (!sent_preedit) {
        sent_preedit = 1;
        zwp_input_method_v2_set_preedit_string(im, "nihon", 5, 5);
        zwp_input_method_v2_commit(im, done_count);
    }
    if (!committed) {
        committed = 1;
        zwp_input_method_v2_commit_string(im, "日本");
        zwp_input_method_v2_commit(im, done_count);
    }
}

static void im_unavailable(void* data, struct zwp_input_method_v2* im)
{
    (void)data;
    (void)im;
    printf("input-method-test: unavailable\n");
    fflush(stdout);
}

static struct zwp_input_method_v2_listener const im_listener = {
    im_activate, im_deactivate, im_surrounding_text, im_text_change_cause, im_content_type, im_done, im_unavailable,
};

// --- popup ------------------------------------------------------------------

static void popup_text_input_rectangle(void* data, struct zwp_input_popup_surface_v2* surface, int32_t x, int32_t y, int32_t width, int32_t height)
{
    (void)data;
    (void)surface;
    printf("input-method-test: popup rect %d %d %d %d\n", x, y, width, height);
    fflush(stdout);
}

static struct zwp_input_popup_surface_v2_listener const popup_listener = { popup_text_input_rectangle };

// --- registry ---------------------------------------------------------------

static void registry_global(void* data, struct wl_registry* reg, uint32_t name, const char* interface, uint32_t version)
{
    (void)data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        compositor = wl_registry_bind(reg, name, &wl_compositor_interface, version < 4 ? version : 4);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        seat = wl_registry_bind(reg, name, &wl_seat_interface, version < 7 ? version : 7);
    } else if (strcmp(interface, zwp_input_method_manager_v2_interface.name) == 0) {
        input_method_manager = wl_registry_bind(reg, name, &zwp_input_method_manager_v2_interface, 1);
    }
}

static void registry_global_remove(void* data, struct wl_registry* reg, uint32_t name)
{
    (void)data;
    (void)reg;
    (void)name;
}

static struct wl_registry_listener const registry_listener = { registry_global, registry_global_remove };

int main(int argc, char** argv)
{
    int duration_ms = 8000;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc)
            duration_ms = atoi(argv[++i]);
    }

    display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "input-method-test: cannot connect to the compositor\n");
        return 1;
    }
    struct wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);
    wl_display_roundtrip(display);

    if (!compositor || !shm || !seat || !input_method_manager) {
        fprintf(stderr, "input-method-test: missing globals (input-method=%p)\n", (void*)input_method_manager);
        return 1;
    }

    input_method = zwp_input_method_manager_v2_get_input_method(input_method_manager, seat);
    zwp_input_method_v2_add_listener(input_method, &im_listener, NULL);
    keyboard_grab = zwp_input_method_v2_grab_keyboard(input_method);
    zwp_input_method_keyboard_grab_v2_add_listener(keyboard_grab, &grab_listener, NULL);

    // A candidate popup: a wl_surface with the input-popup role. The compositor
    // shows it while the input method is active.
    struct wl_surface* popup_surface = wl_compositor_create_surface(compositor);
    popup = zwp_input_method_v2_get_input_popup_surface(input_method, popup_surface);
    zwp_input_popup_surface_v2_add_listener(popup, &popup_listener, NULL);
    struct wl_buffer* popup_buffer = create_buffer(200, 60, 0xFFE0E0F0);
    wl_surface_attach(popup_surface, popup_buffer, 0, 0);
    wl_surface_damage_buffer(popup_surface, 0, 0, 200, 60);
    wl_surface_commit(popup_surface);
    wl_display_roundtrip(display);

    printf("input-method-test: ready\n");
    fflush(stdout);

    struct pollfd pfd = { .fd = wl_display_get_fd(display), .events = POLLIN };
    int waited = 0;
    while (waited < duration_ms) {
        wl_display_dispatch_pending(display);
        wl_display_flush(display);
        if (poll(&pfd, 1, 50) > 0)
            wl_display_dispatch(display);
        waited += 50;
    }
    return 0;
}
