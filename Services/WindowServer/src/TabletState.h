/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Model.h"

// zwp-tablet-v2 (pen/tablet; Tablet.cpp): the manager plus the v2 tablet/tool
// wrappers.
struct TabletState {
    wlr_tablet_manager_v2* manager { nullptr };
    HashMap<u64, wlr_tablet_v2_tablet*> tablets;
    HashMap<u64, wlr_tablet_v2_tablet_tool*> tools;
    Vector<NonnullOwnPtr<TabletPad>> pads;
    wl_listener tool_axis { };
    wl_listener tool_proximity { };
    wl_listener tool_tip { };
    wl_listener tool_button { };
};
