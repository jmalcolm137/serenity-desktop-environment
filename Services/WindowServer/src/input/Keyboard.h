/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Compositor.h"

void keyboard_modifiers(wl_listener* listener, void*);
void keyboard_key(wl_listener* listener, void* data);
// Drop keys queued while a lock client had no wl_keyboard resource yet.
void clear_pending_lock_keys();
void update_seat_capabilities(Server& server);

// Monotonic milliseconds (the perf log in main.cpp times with it).
u32 monotonic_time_msec();
// A virtual touch device (serenity-virtual-input) came or went; the seat
// advertises touch while any exists.
void add_virtual_touch_device(Server& server);
void remove_virtual_touch_device(Server& server);
void keyboard_destroy(wl_listener* listener, void*);
