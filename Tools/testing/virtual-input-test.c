/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// A minimal Wayland client that injects keyboard and pointer events through the
// wlroots virtual-input protocols, so headless tests can script shell behaviour.
// See docs/09-r3-implementation-plan.md §2.4.
//
// Usage: virtual-input-test [--extent W H] <op>...
//   --move X Y            absolute pointer motion
//   --axis DX DY          scroll (axis) delta; fractional values are allowed
//   --button BTN press|release   (BTN is numeric: 0x110 left, 0x111 right, ...)
//   --key CODE press|release     (raw evdev keycode)
//   --touch X Y                  touch down (id 0) at layout X,Y
//   --touch-motion X Y           move the touch point
//   --touch-up                   lift the touch point
//   --touch-cancel               cancel all touch points
//   --tablet-proximity X Y       pen enters proximity at layout X,Y
//   --tablet-proximity-out       pen leaves proximity
//   --tablet-tip-down / --tablet-tip-up
//   --tablet-motion X Y          move the pen
//   --tablet-axis P D TX TY R S W  pen axis values (as fixed points)
//   --tablet-button BTN press|release
//   --sleep MS
//   --linger MS                   keep the connection open for MS after the ops
//                                 (e.g. to keep a virtual keyboard present)
//
// A virtual pointer is always created; a virtual keyboard when the compositor
// offers one. Touch and tablet use the SDE test-only serenity-virtual-input
// protocol (wlroots offers no virtual touch/tablet).

#define _GNU_SOURCE

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"
#if defined(SDE_HAVE_VIRTUAL_INPUT)
#    include "serenity-virtual-input-client-protocol.h"
#else
// Scripts that only need pointer/keyboard do not generate the SDE virtual-input
// protocol; the touch/tablet ops then compile to no-ops.
#    define serenity_virtual_touch_v1_down(...) ((void)0)
#    define serenity_virtual_touch_v1_motion(...) ((void)0)
#    define serenity_virtual_touch_v1_up(...) ((void)0)
#    define serenity_virtual_touch_v1_cancel(...) ((void)0)
#    define serenity_virtual_tablet_v1_proximity_in(...) ((void)0)
#    define serenity_virtual_tablet_v1_proximity_out(...) ((void)0)
#    define serenity_virtual_tablet_v1_tip_down(...) ((void)0)
#    define serenity_virtual_tablet_v1_tip_up(...) ((void)0)
#    define serenity_virtual_tablet_v1_motion(...) ((void)0)
#    define serenity_virtual_tablet_v1_axis(...) ((void)0)
#    define serenity_virtual_tablet_v1_button(...) ((void)0)
#endif

struct state {
    struct wl_display* display;
    struct wl_registry* registry;
    struct wl_seat* seat;
    struct zwp_virtual_keyboard_manager_v1* keyboard_manager;
    struct zwp_virtual_keyboard_v1* keyboard;
    struct zwlr_virtual_pointer_manager_v1* pointer_manager;
    struct zwlr_virtual_pointer_v1* pointer;
#if defined(SDE_HAVE_VIRTUAL_INPUT)
    struct serenity_virtual_input_manager_v1* virtual_input;
    struct serenity_virtual_touch_v1* touch;
    struct serenity_virtual_tablet_v1* tablet;
#else
    void* virtual_input;
    void* touch;
    void* tablet;
#endif
    uint32_t x_extent;
    uint32_t y_extent;
};

static void registry_global(void* data, struct wl_registry* registry, uint32_t name, const char* interface, uint32_t version)
{
    (void)version;
    struct state* s = data;
    if (!strcmp(interface, wl_seat_interface.name))
        s->seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
    else if (!strcmp(interface, zwp_virtual_keyboard_manager_v1_interface.name))
        s->keyboard_manager = wl_registry_bind(registry, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
    else if (!strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name))
        s->pointer_manager = wl_registry_bind(registry, name, &zwlr_virtual_pointer_manager_v1_interface, 2);
#if defined(SDE_HAVE_VIRTUAL_INPUT)
    else if (!strcmp(interface, serenity_virtual_input_manager_v1_interface.name))
        s->virtual_input = wl_registry_bind(registry, name, &serenity_virtual_input_manager_v1_interface, 1);
#endif
}

static void registry_global_remove(void* data, struct wl_registry* registry, uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static uint32_t now_ms()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

int main(int argc, char** argv)
{
    struct state s = { .x_extent = 1280, .y_extent = 720 };
    int linger_ms = 200;

    s.display = wl_display_connect(NULL);
    if (!s.display) {
        fprintf(stderr, "virtual-input-test: could not connect to the compositor\n");
        return 1;
    }

    s.registry = wl_display_get_registry(s.display);
    wl_registry_add_listener(s.registry, &registry_listener, &s);
    wl_display_roundtrip(s.display);

    if (!s.seat || !s.pointer_manager) {
        fprintf(stderr, "virtual-input-test: compositor does not offer virtual input\n");
        return 1;
    }

    s.pointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(s.pointer_manager, s.seat);
    if (s.keyboard_manager) {
        s.keyboard = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(s.keyboard_manager, s.seat);
        // The compositor requires a keymap before any key event; send a default one.
        struct xkb_context* ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        struct xkb_keymap* keymap = xkb_keymap_new_from_names(ctx, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
        char* keymap_string = xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
        size_t length = strlen(keymap_string) + 1;
        int fd = memfd_create("sde-keymap", MFD_CLOEXEC);
        ftruncate(fd, length);
        void* mapping = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        memcpy(mapping, keymap_string, length);
        munmap(mapping, length);
        zwp_virtual_keyboard_v1_keymap(s.keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, (uint32_t)length);
        close(fd);
        free(keymap_string);
        xkb_keymap_unref(keymap);
        xkb_context_unref(ctx);
        wl_display_roundtrip(s.display);
    }

#if defined(SDE_HAVE_VIRTUAL_INPUT)
    if (s.virtual_input) {
        s.touch = serenity_virtual_input_manager_v1_create_touch(s.virtual_input);
        s.tablet = serenity_virtual_input_manager_v1_create_tablet(s.virtual_input);
        wl_display_roundtrip(s.display);
    }
#endif

    for (int i = 1; i < argc;) {
        if (!strcmp(argv[i], "--extent") && i + 2 < argc) {
            s.x_extent = (uint32_t)atoi(argv[i + 1]);
            s.y_extent = (uint32_t)atoi(argv[i + 2]);
            i += 3;
        } else if (!strcmp(argv[i], "--move") && i + 2 < argc) {
            zwlr_virtual_pointer_v1_motion_absolute(s.pointer, now_ms(), (uint32_t)atoi(argv[i + 1]), (uint32_t)atoi(argv[i + 2]), s.x_extent, s.y_extent);
            zwlr_virtual_pointer_v1_frame(s.pointer);
            i += 3;
        } else if (!strcmp(argv[i], "--axis") && i + 2 < argc) {
            double dx = strtod(argv[i + 1], NULL);
            double dy = strtod(argv[i + 2], NULL);
            if (dx != 0)
                zwlr_virtual_pointer_v1_axis(s.pointer, now_ms(), WL_POINTER_AXIS_HORIZONTAL_SCROLL, wl_fixed_from_double(dx));
            if (dy != 0)
                zwlr_virtual_pointer_v1_axis(s.pointer, now_ms(), WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_double(dy));
            zwlr_virtual_pointer_v1_frame(s.pointer);
            i += 3;
        } else if (!strcmp(argv[i], "--button") && i + 2 < argc) {
            uint32_t button = (uint32_t)strtoul(argv[i + 1], NULL, 0);
            uint32_t state = !strcmp(argv[i + 2], "press") ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED;
            zwlr_virtual_pointer_v1_button(s.pointer, now_ms(), button, state);
            zwlr_virtual_pointer_v1_frame(s.pointer);
            i += 3;
        } else if (!strcmp(argv[i], "--key") && i + 2 < argc) {
            uint32_t code = (uint32_t)strtoul(argv[i + 1], NULL, 0);
            uint32_t state = !strcmp(argv[i + 2], "press") ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED;
            if (s.keyboard)
                zwp_virtual_keyboard_v1_key(s.keyboard, now_ms(), code, state);
            i += 3;
        } else if (!strcmp(argv[i], "--touch") && i + 2 < argc && s.touch) {
            serenity_virtual_touch_v1_down(s.touch, now_ms(), 0, wl_fixed_from_int(atoi(argv[i + 1])), wl_fixed_from_int(atoi(argv[i + 2])));
            i += 3;
        } else if (!strcmp(argv[i], "--touch-motion") && i + 2 < argc && s.touch) {
            serenity_virtual_touch_v1_motion(s.touch, now_ms(), 0, wl_fixed_from_int(atoi(argv[i + 1])), wl_fixed_from_int(atoi(argv[i + 2])));
            i += 3;
        } else if (!strcmp(argv[i], "--touch-up") && s.touch) {
            serenity_virtual_touch_v1_up(s.touch, now_ms(), 0);
            i += 1;
        } else if (!strcmp(argv[i], "--touch-cancel") && s.touch) {
            serenity_virtual_touch_v1_cancel(s.touch, now_ms());
            i += 1;
        } else if (!strcmp(argv[i], "--tablet-proximity") && i + 2 < argc && s.tablet) {
            serenity_virtual_tablet_v1_proximity_in(s.tablet, now_ms(), wl_fixed_from_int(atoi(argv[i + 1])), wl_fixed_from_int(atoi(argv[i + 2])));
            i += 3;
        } else if (!strcmp(argv[i], "--tablet-proximity-out") && s.tablet) {
            serenity_virtual_tablet_v1_proximity_out(s.tablet, now_ms());
            i += 1;
        } else if (!strcmp(argv[i], "--tablet-tip-down") && s.tablet) {
            serenity_virtual_tablet_v1_tip_down(s.tablet, now_ms());
            i += 1;
        } else if (!strcmp(argv[i], "--tablet-tip-up") && s.tablet) {
            serenity_virtual_tablet_v1_tip_up(s.tablet, now_ms());
            i += 1;
        } else if (!strcmp(argv[i], "--tablet-motion") && i + 2 < argc && s.tablet) {
            serenity_virtual_tablet_v1_motion(s.tablet, now_ms(), wl_fixed_from_int(atoi(argv[i + 1])), wl_fixed_from_int(atoi(argv[i + 2])));
            i += 3;
        } else if (!strcmp(argv[i], "--tablet-axis") && i + 7 < argc && s.tablet) {
            serenity_virtual_tablet_v1_axis(s.tablet, now_ms(),
                wl_fixed_from_double(strtod(argv[i + 1], NULL)), wl_fixed_from_double(strtod(argv[i + 2], NULL)),
                wl_fixed_from_double(strtod(argv[i + 3], NULL)), wl_fixed_from_double(strtod(argv[i + 4], NULL)),
                wl_fixed_from_double(strtod(argv[i + 5], NULL)), wl_fixed_from_double(strtod(argv[i + 6], NULL)),
                wl_fixed_from_double(strtod(argv[i + 7], NULL)));
            i += 8;
        } else if (!strcmp(argv[i], "--tablet-button") && i + 2 < argc && s.tablet) {
            uint32_t button = (uint32_t)strtoul(argv[i + 1], NULL, 0);
            uint32_t pressed = !strcmp(argv[i + 2], "press") ? 1 : 0;
            serenity_virtual_tablet_v1_button(s.tablet, now_ms(), button, pressed);
            i += 3;
        } else if (!strcmp(argv[i], "--sleep") && i + 1 < argc) {
            usleep((useconds_t)atoi(argv[i + 1]) * 1000);
            i += 2;
        } else if (!strcmp(argv[i], "--linger") && i + 1 < argc) {
            linger_ms = atoi(argv[i + 1]);
            i += 2;
        } else {
            fprintf(stderr, "virtual-input-test: unknown argument '%s'\n", argv[i]);
            i++;
        }
        wl_display_flush(s.display);
    }

    wl_display_flush(s.display);
    // Give the compositor time to process the injected events, then optionally
    // linger so the virtual devices stay present for the caller to observe.
    usleep((useconds_t)linger_ms * 1000);
    return 0;
}
