/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Model.h"

// Session safety net (R4.1; Idle.cpp), see docs/11-r4-implementation-plan.md:
// idle notification (ext-idle-notify-v1), idle inhibition (zwp_idle_inhibit_v1)
// and DPMS (wlr-output-power-management).
struct IdleState {
    wlr_idle_notifier_v1* notifier { nullptr };
    wlr_idle_inhibit_manager_v1* inhibit_manager { nullptr };
    wlr_output_power_manager_v1* output_power_manager { nullptr };
    Vector<NonnullOwnPtr<IdleInhibitor>> inhibitors;
    wl_listener new_inhibitor { };
    wl_listener output_power_set_mode { };
};
