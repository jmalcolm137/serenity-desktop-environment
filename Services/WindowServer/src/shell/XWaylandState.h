/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Model.h"

// XWayland (R5; XWayland.cpp): X11 client support.
struct XWaylandState {
    wlr_xwayland* wlr { nullptr };
    // Set when the X server is up (the xwayland "ready" event), so the compositor
    // only calls XWM-backed APIs (e.g. set_workareas) once xwm exists.
    bool started { false };
    X11Toplevel* focused { nullptr };
    wl_listener new_surface { };
    wl_listener ready { };
};
