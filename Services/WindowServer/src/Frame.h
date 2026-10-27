/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Compositor.h"

enum class FrameRegion {
    None,
    Button,
    Titlebar,
    Border,
};

// Where the cursor is within a window's server-side frame.
struct FrameHit {
    Toplevel* toplevel { nullptr };
    X11Toplevel* x11_toplevel { nullptr };
    FrameRegion region { FrameRegion::None };
    int button_index { -1 };
    uint32_t edges { 0 };
    Gfx::IntPoint local;
};

// Split a fully painted server-side frame into decoration-only strip buffers.
// The content area becomes a solid scene rect, so a window only retains the
// titlebar/border pixels instead of a full-window bitmap.
void frame_buffers_update(FrameBuffers& buffers, wlr_scene_tree* parent, Gfx::Bitmap const& painted_frame,
    Gfx::IntSize frame_size, int frame_left, int frame_top, int frame_right, int frame_bottom, Gfx::Color background);
void frame_buffers_set_visible(FrameBuffers& buffers, bool visible);

FrameHit hit_test_frame(Server& server, double x, double y);
char const* resize_cursor_name(uint32_t edges);
void update_button_hover(Server& server, FrameHit const& hit);
void update_button_pressed(Server& server, FrameHit const& hit);
void toplevel_update_frame(Toplevel& toplevel);
void apply_toplevel_decoration(Toplevel& toplevel);
bool handle_frame_press(Server& server, uint32_t button, uint32_t time);
void handle_frame_release(Server& server, uint32_t);
