/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Compositor.h"


void initialize_serenity_window(Server&);

// Re-send the current host settings (the key=value blob) to every connected
// serenity-window manager. Call this after changing state the shim answers from,
// so clients see the applied value rather than the one before the change.
void broadcast_host_settings(Server&);

// The toplevel is going away before its client-owned serenity_toplevel resource.
// Detach the resource's user data from the freed Toplevel but keep the resource
// (and its SerenityChrome allocation) alive; the resource destroy callback then
// frees it when the client disconnects. Without this, the resource outlives the
// toplevel and the destroy callback sees a null user_data and leaks the chrome.
void serenity_window_toplevel_destroyed(Toplevel&);
