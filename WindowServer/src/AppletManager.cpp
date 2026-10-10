/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "AppletManager.h"

#include <AK/InsertionSort.h>
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
static int const applet_spacing = 4;
static int const applet_area_height = 19;

static Gfx::IntPoint s_position { 0, 0 };
static bool s_position_known { false };
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

    auto const& order = Config::applet_order();
    insertion_sort(applets, [&](LayerSurface* a, LayerSurface* b) {
        auto index_a = order.find_first_index(a->applet_name).value_or(0);
        auto index_b = order.find_first_index(b->applet_name).value_or(0);
        return index_a > index_b;
    });

    int total_width = 0;
    for (auto* applet : applets)
        total_width += max(0, static_cast<int>(applet->layer_surface->surface->current.width)) + applet_spacing;
    if (!applets.is_empty())
        total_width -= applet_spacing;

    Vector<Applet> layout;
    int right_edge_x = total_width;
    for (auto* applet : applets) {
        auto size = applet->layer_surface->surface->current;
        int width = static_cast<int>(size.width);
        int height = static_cast<int>(size.height);
        Gfx::IntRect rect { right_edge_x - width, 0, width, height };
        rect.center_vertically_within({ 0, 0, 0, applet_area_height });
        layout.append({ applet, rect });
        right_edge_x = rect.x() - applet_spacing;
    }

    Gfx::IntSize area_size { total_width, applet_area_height };
    if (g_hooks.applet_area_size_changed && area_size != s_last_emitted) {
        s_last_emitted = area_size;
        ByteString names;
        for (auto* applet : applets)
            names = ByteString::formatted("{}{},", names, applet->applet_name);
        dbgln("AppletManager: applet area size {}x{} ({} applets: {})", area_size.width(), area_size.height(), applets.size(), names);
        g_hooks.applet_area_size_changed(server, area_size);
    }
    s_area_size = area_size;

    return layout;
}

static ByteString s_last_layout;

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
        origin = { usable.right() - s_area_size.width(), usable.bottom() - s_area_size.height() };
    }

    ByteString signature;
    for (auto& applet : layout) {
        int x = origin.x() + applet.rect.x();
        int y = origin.y() + applet.rect.y();
        wlr_scene_node_set_position(&applet.surface->scene->tree->node, x, y);
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
    apply(server);
}

}
