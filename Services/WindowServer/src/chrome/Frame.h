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

// Inputs for painting a SerenityOS server-side window frame (shared by both
// Wayland xdg-toplevels and XWayland windows).
struct FramePaintParams {
    Gfx::IntRect content_rect;
    Gfx::IntSize frame_size;
    int frame_left { 0 };
    int frame_top { 0 };
    int frame_right { 0 };
    int frame_bottom { 0 };
    StringView title;
    Gfx::Bitmap const* icon { nullptr };
    bool active { false };
    bool highlighted { false };
    bool maximized { false };
    int hovered_button { -1 };
    int pressed_button { -1 };
    char const* dump_path { nullptr };
};

struct FramePaintResult {
    Gfx::IntRect titlebar_icon_rect;
    Vector<Gfx::IntRect> button_rects;
};

// Paint a server-side frame using the active WindowTheme and upload the
// resulting decoration strips to `buffers` (the content area is a solid scene
// rect, so a window retains only the titlebar/border pixels).
FramePaintResult paint_and_update_frame_buffers(
    FrameBuffers& buffers, wlr_scene_tree* parent, FramePaintParams const& params);
void frame_buffers_set_visible(FrameBuffers& buffers, bool visible);

FrameHit hit_test_frame(Server& server, double x, double y);
char const* resize_cursor_name(uint32_t edges);
void update_button_hover(Server& server, FrameHit const& hit);
void update_button_pressed(Server& server, FrameHit const& hit);
void toplevel_update_frame(Toplevel& toplevel);
void apply_toplevel_decoration(Toplevel& toplevel);
bool handle_frame_press(Server& server, uint32_t button, uint32_t time);
void handle_frame_release(Server& server, uint32_t);
