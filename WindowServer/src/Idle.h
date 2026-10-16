/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Compositor.h"

void notify_activity(Server& server);

void initialize_idle(Server&);

// Unhook the listeners initialize wired, before the objects they listen on go.
void finish_idle(Server&);
