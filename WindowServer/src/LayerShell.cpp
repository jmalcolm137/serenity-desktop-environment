/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Compositor.h"
#include "AppletManager.h"
#include "Focus.h"
#include "InputMethod.h"
#include "Keyboard.h"
#include "LayerShell.h"
#include "Output.h"
#include "XWayland.h"
#include "XdgShell.h"

#include <LibGfx/Palette.h>

#include <string.h>

#include "Wlroots.h"

// --- layer shell ----------------------------------------------------------

Output* output_for(Server& server, wlr_output* output)
{
    for (auto& candidate : server.outputs) {
        if (candidate->output == output)
            return candidate.ptr();
    }
    return nullptr;
}

// The primary output's usable area (the window-manager tiling target, WS2).
Gfx::IntRect primary_usable_area(Server& server)
{
    wlr_box box { };
    wlr_output_layout_get_box(server.output_layout, server.primary_output, &box);
    if (auto* output_state = output_for(server, server.primary_output); output_state && !wlr_box_empty(&output_state->usable_area))
        box = output_state->usable_area;
    return { box.x, box.y, box.width, box.height };
}

// The current theme palette, for the window switcher (WS3).
Gfx::Palette const& current_palette(Server&)
{
    VERIFY(g_palette);
    return *g_palette;
}

void arrange_layers(Server& server, wlr_output* output)
{
    if (!output)
        return;

    wlr_box full_area { };
    wlr_output_layout_get_box(server.output_layout, output, &full_area);
    wlr_box usable_area = full_area;

    auto* output_state = output_for(server, output);

    // Configure layer surfaces from the bottom-most layer up, so exclusive
    // zones from lower layers shrink the area available to higher ones.
    static constexpr zwlr_layer_shell_v1_layer layer_order[] = {
        ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND,
        ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM,
        ZWLR_LAYER_SHELL_V1_LAYER_TOP,
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
    };

    for (auto layer : layer_order) {
        for (auto& layer_surface : server.layer_surfaces) {
            auto* surface = layer_surface->layer_surface;
            if (surface->output != output)
                continue;
            if (surface->current.layer != layer)
                continue;
            wlr_scene_layer_surface_v1_configure(layer_surface->scene, &full_area, &usable_area);
        }
    }

    if (output_state
        && (output_state->usable_area.width != usable_area.width
            || output_state->usable_area.height != usable_area.height
            || output_state->usable_area.x != usable_area.x
            || output_state->usable_area.y != usable_area.y)) {
        output_state->usable_area = usable_area;
        dbgln("WindowServer: usable area for '{}': {}x{} at {},{}", output->name ? output->name : "?",
            usable_area.width, usable_area.height, usable_area.x, usable_area.y);
    }

    // The layer-shell arrange resets every node's position, so re-apply the
    // applet-area layout last (WS4).
    AppletManager::relayout(server);

    // Layer-surface clients cache their on-screen origin: the Taskbar uses it to
    // place its Serenity/system menu, and applets use it for tooltips. A mode or
    // scale change moves an anchored panel without the client committing, so
    // report the position the arrange pass just gave every surface on this output.
    for (auto& layer : server.layer_surfaces) {
        if (layer->output == output)
            layer_report_position(*layer);
    }

    // Keep X11 clients' workarea in step with the panels' exclusive zones.
    update_xwayland_workareas(server);
}

// On_demand layer surfaces (client popups: the Wi-Fi network list and its
// passphrase field, ComboBox dropdowns, …) take keyboard focus while they are
// mapped, so their text fields can be typed into. Panels, the desktop and
// applets use KEYBOARD_INTERACTIVITY_NONE and never steal focus.
static bool layer_takes_keyboard(LayerSurface* layer)
{
    return layer->layer_surface->current.keyboard_interactive != ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE;
}

static void focus_layer_surface(Server& server, LayerSurface* layer)
{
    auto* surface = layer->layer_surface->surface;
    if (!surface || server.seat->keyboard_state.focused_surface == surface)
        return;
    stop_key_repeat();
    server.keyboard_focus_layer = layer;
    auto* keyboard = wlr_seat_get_keyboard(server.seat);
    if (keyboard)
        wlr_seat_keyboard_notify_enter(server.seat, surface, keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
    // R5 IME: a client popup's text field can be typed into via an input method.
    InputMethod::set_focus(server, surface);
}

// Give keyboard focus back to the topmost normal window (or clear it) when an
// on_demand layer surface stops being focused.
static void restore_keyboard_focus(Server& server)
{
    if (server.keyboard_focus_layer)
        server.keyboard_focus_layer = nullptr;
    stop_key_repeat();
    for (auto it = server.toplevels.rbegin(); it != server.toplevels.rend(); ++it) {
        if (!(*it)->minimized) {
            focus_toplevel(server, it->ptr());
            return;
        }
    }
    wlr_seat_keyboard_notify_clear_focus(server.seat);
    // R5 IME: no focus, so no text input is active.
    InputMethod::set_focus(server, nullptr);
}

static void layer_surface_commit(wl_listener* listener, void*)
{
    LayerSurface* layer = nullptr;
    layer = wl_container_of(listener, layer, commit);

    // Only re-arrange when the committed state can actually change the layout.
    // Layer-shell reconfigures every surface on commit; doing that for a
    // content-only commit re-configures every layer surface and spins any client
    // that commits in response to a configure (see docs/04-known-gaps.md).
    auto const& state = layer->layer_surface->current;
    auto const& previous = layer->arrange_state;
    uint32_t const committed_width = layer->layer_surface->surface ? layer->layer_surface->surface->current.width : 0;
    uint32_t const committed_height = layer->layer_surface->surface ? layer->layer_surface->surface->current.height : 0;
    bool const arrange_relevant = !previous.valid
        || previous.desired_width != state.desired_width
        || previous.desired_height != state.desired_height
        || previous.width != committed_width
        || previous.height != committed_height
        || previous.anchor != state.anchor
        || previous.exclusive_zone != state.exclusive_zone
        || previous.margin_top != state.margin.top
        || previous.margin_right != state.margin.right
        || previous.margin_bottom != state.margin.bottom
        || previous.margin_left != state.margin.left
        || previous.layer != static_cast<uint32_t>(state.layer);
    // Keyboard focus follows on_demand interactivity (a client may toggle it).
    if (layer->layer_surface->surface && layer->layer_surface->surface->mapped) {
        if (layer_takes_keyboard(layer))
            focus_layer_surface(*layer->server, layer);
        else if (layer->server->keyboard_focus_layer == layer)
            restore_keyboard_focus(*layer->server);
    }
    if (!arrange_relevant)
        return;

    layer->arrange_state.valid = true;
    layer->arrange_state.desired_width = state.desired_width;
    layer->arrange_state.desired_height = state.desired_height;
    layer->arrange_state.width = committed_width;
    layer->arrange_state.height = committed_height;
    layer->arrange_state.anchor = state.anchor;
    layer->arrange_state.exclusive_zone = state.exclusive_zone;
    layer->arrange_state.margin_top = state.margin.top;
    layer->arrange_state.margin_right = state.margin.right;
    layer->arrange_state.margin_bottom = state.margin.bottom;
    layer->arrange_state.margin_left = state.margin.left;
    layer->arrange_state.layer = static_cast<uint32_t>(state.layer);

    arrange_layers(*layer->server, layer->output);
}

static void layer_surface_map(wl_listener* listener, void*)
{
    LayerSurface* layer = nullptr;
    layer = wl_container_of(listener, layer, map);
    wlr_scene_node_set_enabled(&layer->scene->tree->node, true);
    arrange_layers(*layer->server, layer->output);
    if (layer->is_menu)
        dbgln("WindowServer: client menu opened");
    // A mapped on_demand surface (a client popup) takes keyboard focus, so its
    // text fields work.
    if (layer_takes_keyboard(layer))
        focus_layer_surface(*layer->server, layer);
}

static void layer_surface_unmap(wl_listener* listener, void*)
{
    LayerSurface* layer = nullptr;
    layer = wl_container_of(listener, layer, unmap);
    wlr_scene_node_set_enabled(&layer->scene->tree->node, false);
    if (layer->is_menu)
        dbgln("WindowServer: client menu closed");
    bool const had_focus = layer->server->keyboard_focus_layer == layer;
    arrange_layers(*layer->server, layer->output);
    if (had_focus)
        restore_keyboard_focus(*layer->server);
}

static void layer_surface_destroy(wl_listener* listener, void*)
{
    LayerSurface* layer = nullptr;
    layer = wl_container_of(listener, layer, destroy);
    wl_list_remove(&layer->map.link);
    wl_list_remove(&layer->unmap.link);
    wl_list_remove(&layer->commit.link);
    wl_list_remove(&layer->destroy.link);
    auto* server = layer->server;
    auto* output = layer->output;
    bool const had_focus = server->keyboard_focus_layer == layer;
    remove_owned(server->layer_surfaces, layer);
    arrange_layers(*server, output);
    if (had_focus)
        restore_keyboard_focus(*server);
}

static void server_new_layer_surface(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, new_layer_surface);
    auto* layer_surface = static_cast<wlr_layer_surface_v1*>(data);

    if (!layer_surface->output)
        layer_surface->output = server->primary_output;
    if (!layer_surface->output) {
        wlr_layer_surface_v1_destroy(layer_surface);
        return;
    }

    auto layer = make<LayerSurface>();
    layer->server = server;
    layer->layer_surface = layer_surface;
    layer->output = layer_surface->output;
    layer->is_menu = layer_surface->layer_namespace && !strcmp(layer_surface->layer_namespace, "serenity-menu");
    layer->is_applet = AppletManager::parse_namespace(layer_surface->layer_namespace, layer->applet_name);
    char const* layer_name = "unknown";
    wlr_scene_tree* layer_parent = server->background_tree;
    switch (layer_surface->pending.layer) {
    case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND:
        layer_parent = server->background_tree;
        layer_name = "background";
        break;
    case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM:
        layer_parent = server->bottom_tree;
        layer_name = "bottom";
        break;
    case ZWLR_LAYER_SHELL_V1_LAYER_TOP:
        layer_parent = server->top_tree;
        layer_name = "top";
        break;
    case ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY:
        layer_parent = server->overlay_tree;
        layer_name = "overlay";
        break;
    default:
        break;
    }
    dbgln("WindowServer: layer surface namespace='{}' layer={}", layer_surface->layer_namespace ? layer_surface->layer_namespace : "", layer_name);
    layer->scene = wlr_scene_layer_surface_v1_create(layer_parent, layer_surface);
    layer->scene->tree->node.data = layer.ptr();
    use_nearest_filter(&layer->scene->tree->node);
    layer_surface->data = layer.ptr();

    layer->map.notify = layer_surface_map;
    wl_signal_add(&layer_surface->surface->events.map, &layer->map);
    layer->unmap.notify = layer_surface_unmap;
    wl_signal_add(&layer_surface->surface->events.unmap, &layer->unmap);
    layer->commit.notify = layer_surface_commit;
    wl_signal_add(&layer_surface->surface->events.commit, &layer->commit);
    layer->destroy.notify = layer_surface_destroy;
    wl_signal_add(&layer_surface->events.destroy, &layer->destroy);

    server->layer_surfaces.append(move(layer));
}

void initialize_layer_shell(Server& server)
{
    server.layer_shell = wlr_layer_shell_v1_create(server.display, 4);
    server.new_layer_surface.notify = server_new_layer_surface;
    wl_signal_add(&server.layer_shell->events.new_surface, &server.new_layer_surface);
}

void finish_layer_shell(Server& server)
{
    remove_listener(server.new_layer_surface);
}
