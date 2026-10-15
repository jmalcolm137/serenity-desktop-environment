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

// Restore data, shared with the RemoteDesktop and InputCapture portals: the
// `(suv)` a backend returns from Start when the app set `persist_mode`, and is
// handed back as the `restore_data` option of a later session. The frontend keeps
// it per application in the permission store (tables `screencast`,
// `remote-desktop`, `input-capture`), so a remembered choice is listed and revoked
// by the standard tooling and the backend stores nothing itself. Ours is
// ("SDE", 2, <a{sv}>) carrying the fields below.
struct RestoreData {
    u32 types { 0 };
    u32 cursor_mode { 0 };
    ByteString source_id;
};
Portal::Value make_restore_data(RestoreData const&);
// Reads `restore_data` out of an options dictionary. Also accepts version 1, whose
// variant is a token into the legacy screencast-tokens file of the state directory.
Optional<RestoreData> parse_restore_data(Portal::Value const& options);
// The persist_mode the backend honours (0 none, 1 while running, 2 until revoked).
u32 clamp_persist_mode(u32 requested);

}
