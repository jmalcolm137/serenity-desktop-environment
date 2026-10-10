/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefPtr.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Rect.h>

struct wlr_buffer;
struct wlr_scene_buffer;
struct wlr_scene_node;
struct wlr_scene_tree;

// Compositor-drawn chrome and overlays.
//
// Everything the compositor itself paints - window frames, menus, and the
// overlays R3 adds (snap preview, geometry HUD, workspace-switch indicator,
// switcher thumbnails) - is a Gfx::Bitmap presented through the wlroots scene
// graph. This module owns that shared infrastructure so the policy areas can be
// split out of main.cpp without each re-inventing it. See
// docs/05-r3-plan.md §3 and docs/09-r3-implementation-plan.md §2.

// A wlr_buffer backed by a Gfx::Bitmap, so a LibGfx-drawn bitmap can be shown by
// a wlr_scene_buffer. The scene buffer takes its own reference; drop yours.
wlr_buffer* bitmap_buffer_create(NonnullRefPtr<Gfx::Bitmap>);

// A persistent, LibGfx-rendered node in the scene graph: owns a scene buffer and
// the screen rect it occupies. Call render() with a painter callback to (re)paint
// it; set_rect() moves/sizes it. This is the "chrome node in overlay_tree" the
// R3 foundation asks for, and it backs the window menu today.
class ChromeNode {
public:
    explicit ChromeNode(wlr_scene_tree& parent);
    ~ChromeNode();

    Gfx::IntRect rect() const { return m_rect; }
    void set_rect(Gfx::IntRect);

    void set_visible(bool);
    bool is_visible() const;

    void raise_to_top();

    // (Re)allocate the bitmap to the current rect size, let the callback draw it
    // (in rect-local coordinates) and present it.
    void render(Function<void(Gfx::Painter&)> const&);

private:
    Gfx::IntRect m_rect;
    wlr_scene_buffer* m_buffer { nullptr };
};

// Snapshot a toplevel's rendered content, for the switcher's live thumbnails.
//
// The capture mechanism is undecided - composite the toplevel's scene subtree
// into a texture, or drive `ext-image-copy-capture-v1` - and is the R3
// foundation spike (docs/09-r3-implementation-plan.md §3). Until it lands this
// returns nullptr, and callers fall back to the application icon.
RefPtr<Gfx::Bitmap> snapshot_toplevel(wlr_scene_node& toplevel_node);
