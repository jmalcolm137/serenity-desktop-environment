/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Model.h"

// Explicit drag-and-drop (data-device DnD between windows; DragAndDrop.cpp): the
// active drag, the surface it is currently over, and the scene node drawing its
// icon.
struct DragState {
    wlr_drag* active { nullptr };
    wlr_surface* target { nullptr };
    wlr_scene_node* icon_node { nullptr };
    wl_listener request_start { };
    wl_listener start { };
    wl_listener destroy { };
    wl_listener icon_destroy { };
};
