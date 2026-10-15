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
void session_lock_new_lock(wl_listener* listener, void* data);
