/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Compositor.h"

void notify_activity(Server& server);
void server_new_idle_inhibitor(wl_listener* listener, void* data);
void output_power_set_mode(wl_listener* listener, void* data);
