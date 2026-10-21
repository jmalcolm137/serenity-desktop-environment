/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Compositor.h"

bool cycle_keyboard_layout(Server& server);
void apply_input_settings(Server& server);

void initialize_input_devices(Server&);

// Unhook the listeners initialize wired, before the objects they listen on go.
void finish_input_devices(Server&);
