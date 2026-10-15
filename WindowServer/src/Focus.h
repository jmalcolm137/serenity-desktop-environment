/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Compositor.h"

void deactivate_other_windows(Server& server, Toplevel* keep_toplevel, X11Toplevel* keep_x11);
void focus_toplevel(Server& server, Toplevel* toplevel);
