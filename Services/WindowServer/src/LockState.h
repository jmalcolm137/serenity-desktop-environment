/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Model.h"

// Session lock (R4.1, ext-session-lock-v1; SessionLock.cpp).
struct LockState {
    wlr_session_lock_manager_v1* manager { nullptr };
    wlr_session_lock_v1* lock { nullptr };
    bool locked { false };
    Vector<NonnullOwnPtr<LockSurface>> surfaces;
    wl_listener new_lock { };
    wl_listener new_surface { };
    wl_listener unlock { };
    wl_listener destroy { };
};
