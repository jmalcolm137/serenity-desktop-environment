/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "dbus.h"

// R5 InputCapture backend (org.freedesktop.impl.portal.InputCapture): bridges
// the freedesktop InputCapture portal to the compositor's compositor-hosted EIS
// implementation over the private serenity_input_capture protocol
// (docs/17-input-capture.md). The app receives captured input over EIS; this
// backend only relays session control and the EIS file descriptor.
namespace InputCapture {

void register_interface(Portal::Service&);

bool has_session(ByteString const& session_handle);
void set_clipboard_enabled(ByteString const& session_handle, bool enabled);

}
