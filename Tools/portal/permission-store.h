/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/StringView.h>
#include <AK/Vector.h>

// A small blocking client for org.freedesktop.impl.portal.PermissionStore on the
// session bus -- the shared, SQLite-backed grant store xdg-desktop-portal ships,
// so SDE's "remember this" decisions are visible to `flatpak permission-show`,
// `d-feet` and any other portal backend.
//
// Calls go over a *private* session-bus connection, blocking with a short timeout
// -- the same idiom as the logind inhibitor in inhibit.cpp -- so they never
// re-enter the service's own connection (whose dispatch we keep non-blocking,
// docs/15 §6.6). A reachable store is authoritative; an absent one is reported as
// such so the caller can fall back to its own persistence (logind/portals are
// optional throughout SDE).
namespace Portal::PermissionStore {

// permissions[app] = that app's permission strings for one (table, id) resource.
using Permissions = HashMap<ByteString, Vector<ByteString>>;

// One resource's per-app permissions. An empty Optional means the store is
// unreachable (no service on the bus); an empty map means it is reachable but has
// no entry for `id`.
Optional<Permissions> lookup(StringView table, StringView id);

// Set one app's permissions for (table, id), creating the table/row if needed.
// Returns false when the store is unreachable.
bool set_permission(StringView table, StringView id, StringView app, Vector<ByteString> const& permissions);

// Forget a resource entirely (revokes every app's grant for it). Returns false
// when the store is unreachable.
bool remove(StringView table, StringView id);

}
