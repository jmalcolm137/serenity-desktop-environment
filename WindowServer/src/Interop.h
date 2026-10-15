/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Compositor.h"

void on_workspace_changed(Server& server);
void ext_workspace_commit(wl_listener* listener, void* data);
void xdg_activation_activate(wl_listener* listener, void* data);
void cursor_shape_request(wl_listener* listener, void* data);
void toplevel_icon_set(wl_listener* listener, void* data);
void system_bell_ring(wl_listener* listener, void*);
