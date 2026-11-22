/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "AppletManager.h"

#include "AppletLayout.h"

#include <AK/InsertionSort.h>
#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibGfx/Rect.h>
#include <string.h>

#include "Config.h"

// wlroots is a C library and does not wrap its headers in extern "C", so we do.
extern "C" {
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_scene.h>
}

namespace AppletManager {

static HostHooks g_hooks;

void set_host_hooks(HostHooks hooks)
{
    g_hooks = hooks;
}

// SerenityOS AppletManager constants.
static int const applet_spacing = AppletLayout::default_spacing;
static int const applet_area_height = 19;

static Gfx::IntPoint s_position { 0, 0 };
static bool s_position_known { false };
// The Taskbar/usable-area geometry in effect when s_position was reported. The
// position is absolute, so when an output's resolution or scale changes the
// applet area has to be moved by the same delta as its anchor; otherwise it
// stays at the old screen coordinates and floats somewhere on the new screen.
static Gfx::IntRect s_position_anchor;
static Gfx::IntSize s_area_size;
static Gfx::IntSize s_last_emitted;

bool parse_namespace(char const* layer_namespace, ByteString& name)
{
    if (!layer_namespace)
        return false;
    StringView const prefix = "serenity-applet"sv;
    StringView view { layer_namespace, strlen(layer_namespace) };
    if (!view.starts_with(prefix))
        return false;
    auto rest = view.substring_view(prefix.length());
    if (rest.starts_with(':'))
        name = rest.substring_view(1).to_byte_string();
    return true;
}

struct Applet {
    LayerSurface* surface { nullptr };
    Gfx::IntRect rect; // Relative to the applet-area origin.
};

// Collect the mapped applets, order them as SerenityOS does (descending by
// [Applet] Order index, unmatched = 0), and lay them out right-to-left.
static Vector<Applet> build_layout(Server& server)
{
    Vector<LayerSurface*> applets;
    for (auto& layer : server.layer_surfaces) {
        if (!layer->is_applet)
            continue;
        if (!layer->layer_surface || !layer->layer_surface->surface || !layer->layer_surface->surface->mapped)
            continue;
        applets.append(layer.ptr());
    }

    // The order and the rectangles are AppletLayout.h's rules.
    Vector<AppletLayout::Applet> inputs;
    inputs.ensure_capacity(applets.size());
    for (auto* applet : applets) {
        auto size = applet->layer_surface->surface->current;
        inputs.append({ applet->applet_name, { static_cast<int>(size.width), static_cast<int>(size.height) } });
    }
    auto placed = AppletLayout::layout(inputs, Config::applet_order(), applet_area_height, applet_spacing);

    Vector<Applet> layout;
    layout.ensure_capacity(placed.placements.size());
    for (auto const& placement : placed.placements)
        layout.append({ applets[placement.applet_index], placement.rect });

    Gfx::IntSize area_size = placed.area_size;
    if (g_hooks.applet_area_size_changed && area_size != s_last_emitted) {
        s_last_emitted = area_size;
        ByteString names;
        // In placement order, which is the configured order: the log line is what
        // the tests read to see that [Applet] Order was honoured.
        for (auto const& applet : layout)
            names = ByteString::formatted("{}{},", names, applet.surface->applet_name);
        dbgln("AppletManager: applet area size {}x{} ({} applets: {})", area_size.width(), area_size.height(), applets.size(), names);
        g_hooks.applet_area_size_changed(server, area_size);
    }
    s_area_size = area_size;

    return layout;
}

static ByteString s_last_layout;

// The Taskbar is a layer surface ("serenity-taskbar"). Return its on-screen rect
// so the applet area can be anchored to it rather than to absolute coordinates.
static Optional<Gfx::IntRect> taskbar_rect(Server& server)
{
    for (auto& layer : server.layer_surfaces) {
        auto* surface = layer->layer_surface;
        if (!surface || !surface->surface || !surface->surface->mapped)
            continue;
        if (!surface->layer_namespace || strcmp(surface->layer_namespace, "serenity-taskbar"))
            continue;
        if (!layer->scene)
            continue;
        auto size = surface->surface->current;
        if (size.width <= 0 || size.height <= 0)
            continue;
        auto const& node = layer->scene->tree->node;
        return Gfx::IntRect { node.x, node.y, static_cast<int>(size.width), static_cast<int>(size.height) };
    }
    return {};
}

// What the Taskbar-supplied position is relative to: the Taskbar if it is mapped,
// otherwise the usable area (the fallback's reference).
static Gfx::IntRect current_anchor(Server& server)
{
    if (auto taskbar = taskbar_rect(server); taskbar.has_value())
        return taskbar.value();
    if (g_hooks.primary_usable_area)
        return g_hooks.primary_usable_area(server);
    return {};
}

static void apply(Server& server)
{
    auto layout = build_layout(server);

    // The Taskbar positions the applet area; until it does, anchor the area to
    // the bottom-right of the usable area (where layer-shell would put applets).
    Gfx::IntPoint origin = s_position;
    if (!s_position_known) {
        auto usable = g_hooks.primary_usable_area ? g_hooks.primary_usable_area(server) : Gfx::IntRect { };
        if (usable.is_empty())
            return;
        origin = AppletLayout::fallback_origin(usable, s_area_size);
    } else {
        // The Taskbar's position is absolute. If its anchor (the Taskbar itself,
        // or the screen it sits on) has moved or resized since it reported — a
        // resolution or scale change — move the applet area by the same delta.
        auto anchor = current_anchor(server);
        if (!anchor.is_empty() && !s_position_anchor.is_empty())
            origin = AppletLayout::translate_with_anchor(s_position_anchor, anchor, s_position);
    }

    ByteString signature;
    for (auto& applet : layout) {
        int x = origin.x() + applet.rect.x();
        int y = origin.y() + applet.rect.y();
        wlr_scene_node_set_position(&applet.surface->scene->tree->node, x, y);
        // The Taskbar is a top-layer surface too; keep the applets above it so
        // they draw on top of the applet-area frame (as they do in SerenityOS,
        // where they are children of the Taskbar).
        wlr_scene_node_raise_to_top(&applet.surface->scene->tree->node);
        if (g_hooks.report_position)
            g_hooks.report_position(*applet.surface);
        signature = ByteString::formatted("{}'{}' {}x{}+{},{}; ", signature, applet.surface->applet_name,
            applet.rect.width(), applet.rect.height(), x, y);
    }
    if (signature != s_last_layout) {
        s_last_layout = signature;
        dbgln("AppletManager: layout {}", signature);
    }
}

void relayout(Server& server)
{
    apply(server);
}

void set_position(Server& server, Gfx::IntPoint position)
{
    s_position = position;
    s_position_known = true;
    // Remember the Taskbar geometry this position was computed against, so a
    // later resize/scale can translate the area along with it.
    s_position_anchor = current_anchor(server);
    apply(server);
}

Gfx::IntSize area_size()
{
    return s_area_size;
}

}
