/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Compositor.h"

bool surface_global_origin(Server& server, wlr_surface* surface, double* out_x, double* out_y);
void seat_start_drag(wl_listener* listener, void* data);
void seat_request_start_drag(wl_listener* listener, void* data);
