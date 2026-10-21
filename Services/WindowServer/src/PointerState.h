/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Model.h"

// The cursor and the seat's pointer-side requests (Pointer.cpp).
struct PointerState {
    wl_listener motion { };
    wl_listener motion_absolute { };
    wl_listener button { };
    wl_listener axis { };
    wl_listener frame { };
    wl_listener touch_down { };
    wl_listener touch_motion { };
    wl_listener touch_up { };
    wl_listener touch_cancel { };
    wl_listener seat_request_cursor { };
    wl_listener seat_pointer_focus_change { };
    wl_listener seat_request_set_selection { };
    wl_listener seat_request_set_primary_selection { };
    // zwp-pointer-constraints-v1: pointer lock/confine for games. The active
    // constraint (for the focused pointer surface) is enforced in
    // process_cursor_motion(); null when none applies.
    wlr_pointer_constraints_v1* constraints { nullptr };
    wlr_pointer_constraint_v1* active_constraint { nullptr };
    wl_listener new_constraint { };
    // zwp-relative-pointer-v1: unaccelerated motion for games, sent with every
    // pointer motion.
    wlr_relative_pointer_manager_v1* relative_pointer_manager { nullptr };
    // wp-cursor-shape-v1: clients can set named cursor shapes (R4.7/R4.8).
    wlr_cursor_shape_manager_v1* cursor_shape_manager { nullptr };
    wl_listener cursor_shape_request { };

    // Implicit pointer grab: while a client button is held, keep the pointer
    // focused on the surface that received the press and forward motion to it,
    // even if the cursor crosses into another surface. wlroots' seat forgets the
    // pressed button when pointer focus changes, so without this the eventual
    // release is silently dropped and the client stays stuck mid-press.
    wlr_surface* grab_surface { nullptr };
    double grab_origin_x { 0 };
    double grab_origin_y { 0 };
    uint32_t buttons_down { 0 };
};

// An interactive move or resize of a window (xdg or X11), started from its frame,
// a client request or a Super-drag.
struct GrabState {
    CursorMode mode { CursorMode::Passthrough };
    Toplevel* toplevel { nullptr };
    X11Toplevel* x11 { nullptr };
    double x { 0 };
    double y { 0 };
    wlr_box box { };
    uint32_t edges { 0 };
    // Started from the server-side frame (titlebar / border), not by the client.
    bool from_frame { false };
};
