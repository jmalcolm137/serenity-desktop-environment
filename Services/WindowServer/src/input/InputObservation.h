/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

struct Server;

// ext-input-observation-v1: the pointer position relative to a client's
// surface, even outside it (LibWM's set_global_mouse_tracking).
void initialize_input_observation(Server&);
void finish_input_observation(Server&);
