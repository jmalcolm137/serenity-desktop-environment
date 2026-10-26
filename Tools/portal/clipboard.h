/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "dbus.h"

// Clipboard portal backend (org.freedesktop.impl.portal.Clipboard).
//
// The interface is attached to an existing RemoteDesktop or InputCapture
// session. The backend is a privileged ext-data-control-v1 client: it mirrors
// the compositor selection, creates a data source for a session that sets the
// selection, and exposes fd-based reads/writes to the portal frontend.
namespace Clipboard {

void register_interface(Portal::Service&);

// Whether a session requested clipboard access and may use the clipboard
// methods. RemoteDesktop/InputCapture use this for their `clipboard_enabled`
// Start result.
bool is_enabled(ByteString const& session_handle);

}
