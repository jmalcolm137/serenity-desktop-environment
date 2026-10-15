/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Compositor.h"

void schedule_persist_screen_layout(Server& server);
void apply_saved_settings_to_output(Server& server, struct wlr_output* output, Config::OutputSettings const& settings);
void apply_output_scale(Server& server);
void apply_loaded_screen_layout(Server& server);
void output_manager_send_configuration(Server& server);
void output_manager_apply(wl_listener* listener, void* data);
void output_manager_test(wl_listener* listener, void* data);
