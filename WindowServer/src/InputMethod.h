/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stdint.h>

struct Server;
struct wlr_keyboard;
struct wlr_surface;

// IME: text-input-v3 + input-method-v2 (R5, docs/22-input-method.md).
//
// The compositor is the relay between a client's wp_text_input_v3 (the focused
// text field) and the desktop's input method (zwp_input_method_v2 — an IBus /
// Fcitx / Maliit front end): it forwards the field's surrounding text, content
// type and cursor rectangle, applies the input method's preedit/commit strings
// back to the field, and routes the keyboard to the input method while it holds
// a keyboard grab.
namespace InputMethod {

// Create the text-input-v3 and input-method-v2 globals and wire the managers.
void initialize(Server& server);
// Unhook the listeners initialize wired, before the objects they listen on go.
void finish(Server& server);

// The seat's keyboard focus changed (surface, or null when it was cleared).
// Sends text_input_v3 enter/leave and re-targets the candidate popups.
void set_focus(Server& server, wlr_surface* surface);

// Route a key event to the input method's keyboard grab. Returns true when an
// active grab consumed it (so the caller neither handles nor forwards the key).
bool handle_key(Server& server, uint32_t time_msec, uint32_t keycode, uint32_t state);

// Route a modifier change to the input method's keyboard grab. Returns true when
// a grab consumed it (so the caller must not notify the focused client instead).
bool handle_modifiers(Server& server, wlr_keyboard* keyboard);

}
