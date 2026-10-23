/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Server.h"

// Test-only control channel used by the WLCS integration. Enabled only when
// SDE_WLCS_CONTROL=1 (the harness sets it for its isolated compositor).
void initialize_wlcs_control(Server& server);
