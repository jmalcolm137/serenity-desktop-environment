/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Compositor.h"

void on_workspace_changed(Server& server);

// A new window's handles in the foreign-toplevel protocols (taskbars, docks,
// screencast pickers); null when that protocol is not offered.
wlr_foreign_toplevel_handle_v1* create_foreign_toplevel_handle(Server& server);
wlr_ext_foreign_toplevel_handle_v1* create_ext_foreign_toplevel_handle(Server& server, char const* title, char const* app_id);
void initialize_interop(Server&);
// Unhook the listeners initialize wired, before the objects they listen on go.
void finish_interop(Server&);
