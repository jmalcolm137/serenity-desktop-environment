/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Model.h"

// Pointer interaction with window chrome (titlebar buttons, titlebar drags and
// resize borders), mirroring SerenityOS's WindowFrame/Button state (Frame.cpp,
// XWaylandFrame.cpp).
struct FramePressState {
    Toplevel* toplevel { nullptr };
    int button_index { -1 };
    // The X11 window whose frame button is held, and which one.
    X11Toplevel* x11 { nullptr };
    int x11_button_index { -1 };
    Toplevel* last_titlebar_toplevel { nullptr };
    uint32_t last_titlebar_time { 0 };
    double last_titlebar_x { 0 };
    double last_titlebar_y { 0 };
    bool double_click_pending { false };
};
