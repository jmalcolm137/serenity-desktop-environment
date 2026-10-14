/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "dbus.h"

// R5 ScreenCast backend (org.freedesktop.impl.portal.ScreenCast): captures an
// output via ext-image-copy-capture-v1 and publishes a PipeWire video stream the
// frontend hands to the client.
namespace Screencast {

// Registers the ScreenCast (and, via the Screenshot portal, PNG capture)
// interfaces on the service.
void register_interface(Portal::Service&);

// The PipeWire stream a started session publishes.
struct Stream {
    u32 node_id { 0 };
    int width { 0 };
    int height { 0 };
    u32 source_type { 0 };
};

// Start the capture for an existing ScreenCast session (its D-Bus object path)
// and report the resulting PipeWire stream. Used by the RemoteDesktop portal,
// which pairs input with the ScreenCast video. Returns 0 on success.
int start_for_session(ByteString const& session_handle, Stream&);

// One-shot capture of the primary output into a PNG at `path`; returns 0 on
// success. Used by the Screenshot portal.
int capture_png(ByteString const& path);

// Restore-token persistence (screencast-tokens in the state directory), shared
// with the RemoteDesktop and InputCapture portals.
struct RestoreToken {
    u32 types { 0 };
    u32 cursor_mode { 0 };
    ByteString source_id;
};
void store_restore_token(ByteString const& token, u32 types, u32 cursor_mode, ByteString const& source_id);
Optional<RestoreToken> load_restore_token(ByteString const& token);

}
