/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Compositor.h"


void initialize_serenity_window(Server&);

// Re-send the current host settings (the key=value blob) to every connected
// serenity-window manager. Call this after changing state the shim answers from,
// so clients see the applied value rather than the one before the change.
void broadcast_host_settings(Server&);
