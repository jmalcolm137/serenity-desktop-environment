/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

struct Server;
struct Toplevel;

// xdg-session-management-v1 (R4.2, docs/11-r4-implementation-plan.md): let an
// application restore its toplevels' window-management state across a compositor
// or application restart. A standard protocol (wayland-protocols staging), so it
// works with any compositor that implements it — no SDE-private surface (P8/P9).
//
// wlroots 0.20 has no manager for it, so the server side lives here.
namespace SessionManager {

void initialize(Server& server);

// Record a session-managed toplevel's position/size so it can be restored.
void note_geometry(Toplevel& toplevel);

// Apply a restored position when the toplevel maps.
void on_toplevel_mapped(Toplevel& toplevel);

// Drop references to a toplevel that is being destroyed.
void on_toplevel_destroyed(Toplevel& toplevel);

// Record the applications that currently have a mapped toplevel, so the session
// supervisor can relaunch them after a compositor (re)start — the "relaunch the
// user's windows" half of session restore. Written whenever the set changes.
void update_running_apps(Server& server);

}
