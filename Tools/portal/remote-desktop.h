/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "dbus.h"

// R5 RemoteDesktop backend (org.freedesktop.impl.portal.RemoteDesktop): injects
// pointer/keyboard input into the compositor via wlr-virtual-pointer and
// virtual-keyboard, and returns the paired ScreenCast stream.
namespace RemoteDesktop {

void register_interface(Portal::Service&);

bool has_session(ByteString const& session_handle);
void set_clipboard_enabled(ByteString const& session_handle, bool enabled);
bool clipboard_enabled(ByteString const& session_handle);

}
