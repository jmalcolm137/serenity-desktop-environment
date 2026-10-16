/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Compositor.h"

void spawn_lock_client();
LockSurface* lock_surface_at_cursor(Server& server);
void reconfigure_lock_surface(Server& server, wlr_output* output);

// Whether a session lock is up: input then goes only to the lock surfaces.
bool session_locked(Server const& server);
void initialize_session_lock(Server&);
