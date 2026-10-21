/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Model.h"

// R4.8 / R5: the standard interop surface (Interop.cpp). ext-workspace-v1 maps the
// SDE grid, ext-foreign-toplevel-list-v1 and wlr-foreign-toplevel-management map
// the window list, xdg-activation-v1 lets clients ask to be focused, plus client
// window icons and the system bell (docs/21).
struct InteropState {
    wlr_foreign_toplevel_manager_v1* foreign_toplevel_manager { nullptr };
    wlr_ext_workspace_manager_v1* ext_workspace_manager { nullptr };
    wlr_ext_workspace_group_handle_v1* ext_workspace_group { nullptr };
    Vector<wlr_ext_workspace_handle_v1*> ext_workspaces;
    wlr_ext_foreign_toplevel_list_v1* ext_foreign_toplevel_list { nullptr };
    wlr_ext_foreign_toplevel_image_capture_source_manager_v1* ext_toplevel_capture_source_manager { nullptr };
    wlr_xdg_activation_v1* xdg_activation { nullptr };
    wlr_xdg_toplevel_icon_manager_v1* toplevel_icon_manager { nullptr };
    wlr_xdg_system_bell_v1* system_bell { nullptr };
    wl_listener ext_workspace_commit { };
    wl_listener ext_toplevel_capture_request { };
    wl_listener xdg_activation_activate { };
    wl_listener toplevel_icon_set { };
    wl_listener system_bell_ring { };
};
