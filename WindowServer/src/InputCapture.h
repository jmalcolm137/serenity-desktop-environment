/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stdint.h>

struct Server;

// InputCapture portal support (R5, docs/17-input-capture.md).
//
// The compositor owns the EIS implementation (libeis) that backs
// org.freedesktop.impl.portal.InputCapture, and exposes the private
// serenity_input_capture protocol so the trusted portal backend can start a
// session and obtain its EIS fd. Only offered when SDE_INPUT_CAPTURE=1.
namespace InputCapture {

void initialize(Server& server);

// P2: called from the compositor's pointer path. Return true if an active
// capture consumed the event (so the normal delivery must be skipped).
bool handle_pointer_motion(Server& server, uint32_t time_msec);
bool handle_pointer_button(Server& server, uint32_t button, uint32_t state, uint32_t time_msec);

// P3: called from the compositor's keyboard path. Return true if an active
// capture consumed the key. The XKB modifier state accompanies the key.
bool handle_keyboard_key(Server& server, uint32_t keycode, bool is_press,
    uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group);

// P2: the output set changed, so the session zones did.
void on_outputs_changed(Server& server);

}
