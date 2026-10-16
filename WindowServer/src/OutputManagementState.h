/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Model.h"

// wlr-output-management (OutputManagement.cpp).
struct OutputManagementState {
    wlr_output_manager_v1* manager { nullptr };
    wl_listener apply { };
    wl_listener test { };
};
