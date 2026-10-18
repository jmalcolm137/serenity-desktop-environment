/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Compositor.h"
#include "Animations.h"
#include "Focus.h"
#include "Frame.h"
#include "Interop.h"
#include "LayerShell.h"
#include "Output.h"
#include "Pointer.h"
#include "Resources.h"
#include "SerenityWindow.h"
#include "SessionManager.h"
#include "Trace.h"
#include "WindowManager.h"
#include "WindowMenu.h"
#include "WindowSwitcher.h"
#include "XdgShell.h"

#include <AK/StdLibExtras.h>
#include <LibCore/File.h>
#include <LibGfx/ImageFormats/PNGWriter.h>

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>

#include "Wlroots.h"

extern "C" {
#include "serenity-window-server-protocol.h"
}

// --- xdg toplevels / popups ----------------------------------------------

void toplevel_report_position(Toplevel& toplevel)
{
    // Store session geometry (R4.2), independent of whether the client has a
    // chrome handle.
    SessionManager::note_geometry(toplevel);
    if (!toplevel.chrome)
        return;
    // Report the *content* origin, not the frame origin: the client's coordinate
    // space starts at its surface (which the compositor offsets by the frame
    // inset), so screen-relative coordinates (tooltips, widget rects) must be
    // built from the content top-left.
    int content_x = static_cast<int>(toplevel.scene_tree->node.x) + static_cast<int>(toplevel.content_tree->node.x);
    int content_y = static_cast<int>(toplevel.scene_tree->node.y) + static_cast<int>(toplevel.content_tree->node.y);
    serenity_toplevel_send_position(toplevel.chrome, content_x, content_y);
}

// Report a layer surface's (an applet's) real output position. The compositor
// moves applets into the Taskbar's applet area, away from their anchored corner,
// so the client cannot know its own origin without this.
void layer_report_position(LayerSurface& layer)
{
    if (!layer.chrome || !layer.scene)
        return;
    serenity_toplevel_send_position(layer.chrome,
        static_cast<int>(layer.scene->tree->node.x), static_cast<int>(layer.scene->tree->node.y));
}

static void toplevel_update_ext_foreign(Toplevel& toplevel)
{
    if (!toplevel.ext_foreign_handle)
        return;
    wlr_ext_foreign_toplevel_handle_v1_state state {
        .title = toplevel.xdg_toplevel->title ? toplevel.xdg_toplevel->title : "",
        .app_id = toplevel.xdg_toplevel->app_id ? toplevel.xdg_toplevel->app_id : "",
    };
    wlr_ext_foreign_toplevel_handle_v1_update_state(toplevel.ext_foreign_handle, &state);
}

static void toplevel_update_foreign(Toplevel& toplevel)
{
    if (toplevel.foreign_handle) {
        wlr_foreign_toplevel_handle_v1_set_title(toplevel.foreign_handle, toplevel.xdg_toplevel->title ? toplevel.xdg_toplevel->title : "");
        wlr_foreign_toplevel_handle_v1_set_app_id(toplevel.foreign_handle, toplevel.xdg_toplevel->app_id ? toplevel.xdg_toplevel->app_id : "");
    }
    toplevel_update_ext_foreign(toplevel);
}

static void toplevel_map(wl_listener* listener, void*)
{
    Toplevel* toplevel = nullptr;
    toplevel = wl_container_of(listener, toplevel, map);
    toplevel_update_foreign(*toplevel);
    dbgln("WindowServer: toplevel mapped title='{}' app_id='{}'",
        toplevel->xdg_toplevel->title ? toplevel->xdg_toplevel->title : "",
        toplevel->xdg_toplevel->app_id ? toplevel->xdg_toplevel->app_id : "");
    if (Trace::enabled())
        Trace::event("window"sv, ByteString::formatted("map title='{}' app_id='{}'",
            toplevel->xdg_toplevel->title ? toplevel->xdg_toplevel->title : "",
            toplevel->xdg_toplevel->app_id ? toplevel->xdg_toplevel->app_id : ""));
    wlr_scene_node_set_enabled(&toplevel->scene_tree->node, true);
    toplevel->mapped = true;
    toplevel->frame_active = true;
    apply_toplevel_decoration(*toplevel);
    toplevel_update_frame(*toplevel);
    // The new window joins the current workspace stack.
    WindowManager::on_toplevel_mapped(*toplevel);
    // Apply a restored session position (R4.2), then report the final position.
    bool placed = toplevel->has_session_restore_position;
    SessionManager::on_toplevel_mapped(*toplevel);
    // [Placement] window rules: set the new window's geometry (overrides the
    // session restore), so a script can reproduce a specific desktop layout.
    if (toplevel->xdg_toplevel->title) {
        if (auto placement = Config::window_placement_for(StringView { toplevel->xdg_toplevel->title, strlen(toplevel->xdg_toplevel->title) }); placement.has_value()) {
            auto* geometry = &toplevel->xdg_toplevel->base->geometry;
            wlr_scene_node_set_position(&toplevel->scene_tree->node, placement->x() - geometry->x, placement->y() - geometry->y);
            wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, placement->width(), placement->height());
            toplevel->restore_position = placement->location();
            placed = true;
        }
    }
    // SerenityOS places auto-positioned windows itself: the first at (100,100),
    // then cascading. Without this every new toplevel stayed at the scene origin.
    if (!placed)
        WindowManager::place_new_toplevel(*toplevel);
    toplevel_report_position(*toplevel);
    if (toplevel->foreign_handle && toplevel->server->primary_output)
        wlr_foreign_toplevel_handle_v1_output_enter(toplevel->foreign_handle, toplevel->server->primary_output);
    focus_toplevel(*toplevel->server, toplevel);
    WindowManager::emit_window_state(*toplevel);
    // SerenityOS: the window grows out of the rect it was launched from (R3).
    start_launch_animation(*toplevel->server, *toplevel);
    // A new window means a new running application to restore (R4.2).
    SessionManager::update_running_apps(*toplevel->server);
}

static void toplevel_unmap(wl_listener* listener, void*)
{
    Toplevel* toplevel = nullptr;
    toplevel = wl_container_of(listener, toplevel, unmap);
    if (Trace::enabled())
        Trace::event("window"sv, ByteString::formatted("unmap title='{}'",
            toplevel->xdg_toplevel->title ? toplevel->xdg_toplevel->title : ""));
    toplevel->mapped = false;
    // Hide the compositor-drawn frame along with the client's surface. The frame
    // buffer is a child of this tree and isn't tied to the wl_surface lifetime.
    wlr_scene_node_set_enabled(&toplevel->scene_tree->node, false);
    forget_grab_window(*toplevel->server, *toplevel);
    // An application whose windows have all closed is no longer part of the
    // session to restore.
    SessionManager::update_running_apps(*toplevel->server);
}

// Spike (docs/09-r3-implementation-plan.md §3): capture a toplevel's pixels for
// the switcher's thumbnails. Set WINDOWSERVER_DUMP_THUMBNAIL=<dir> to dump one
// PNG per toplevel, so the capture path can be verified headlessly.
static void maybe_dump_thumbnail(Toplevel& toplevel)
{
    auto const* dir = getenv("WINDOWSERVER_DUMP_THUMBNAIL");
    if (!dir || !*dir)
        return;
    static HashTable<i32> dumped;
    if (dumped.contains(toplevel.wm_id))
        return;

    auto bitmap = snapshot_toplevel(toplevel.content_tree->node);
    if (!bitmap) {
        // Not ready yet (the initial commits have no buffer); retry on the next
        // commit rather than consuming the one-shot dump.
        return;
    }
    dumped.set(toplevel.wm_id);
    dbgln("WindowServer: thumbnail for wm_id={} is {}x{}", toplevel.wm_id, bitmap->width(), bitmap->height());
    if (auto encoded = Gfx::PNGWriter::encode(*bitmap); !encoded.is_error()) {
        auto path = ByteString::formatted("{}/thumb-{}.png", ByteString { dir }, toplevel.wm_id);
        if (auto file = Core::File::open(path, Core::File::OpenMode::Write); !file.is_error())
            (void)file.value()->write_until_depleted(encoded.value());
    }
}

static void toplevel_commit(wl_listener* listener, void*)
{
    Toplevel* toplevel = nullptr;
    toplevel = wl_container_of(listener, toplevel, commit);
    if (toplevel->xdg_toplevel->base->initial_commit) {
        // Restore the stored session size (R4.2), if any; otherwise let the
        // client pick.
        if (toplevel->has_session_restore_size) {
            wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, toplevel->session_restore_size.width(), toplevel->session_restore_size.height());
            toplevel->has_session_restore_size = false;
        } else {
            wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, 0, 0);
        }
    }
    // A decoration created (and possibly requested) before the surface was
    // initialized is applied now that it is.
    apply_toplevel_decoration(*toplevel);
    toplevel_update_foreign(*toplevel);
    toplevel_update_frame(*toplevel);
    maybe_dump_thumbnail(*toplevel);
    WindowManager::emit_window_state(*toplevel);
    // Keep the window switcher's live thumbnails current while it is open (WS3).
    if (WindowSwitcher::is_visible())
        WindowSwitcher::refresh(*toplevel->server);
}

static void toplevel_destroy(wl_listener* listener, void*)
{
    Toplevel* toplevel = nullptr;
    toplevel = wl_container_of(listener, toplevel, destroy);
    auto& server = *toplevel->server;
    if (Trace::enabled())
        Trace::event("window"sv, ByteString::formatted("destroy wm_id={} title='{}'", toplevel->wm_id,
            toplevel->xdg_toplevel->title ? toplevel->xdg_toplevel->title : ""));
    SessionManager::on_toplevel_destroyed(*toplevel);
    wl_list_remove(&toplevel->map.link);
    wl_list_remove(&toplevel->unmap.link);
    wl_list_remove(&toplevel->commit.link);
    wl_list_remove(&toplevel->destroy.link);
    wl_list_remove(&toplevel->request_move.link);
    wl_list_remove(&toplevel->request_resize.link);
    wl_list_remove(&toplevel->request_maximize.link);
    wl_list_remove(&toplevel->request_minimize.link);
    wl_list_remove(&toplevel->request_fullscreen.link);
    wl_list_remove(&toplevel->set_title.link);
    wl_list_remove(&toplevel->set_app_id.link);
    wl_list_remove(&toplevel->foreign_request_activate.link);
    wl_list_remove(&toplevel->foreign_request_close.link);
    if (toplevel->foreign_handle)
        wlr_foreign_toplevel_handle_v1_destroy(toplevel->foreign_handle);
    if (toplevel->ext_foreign_handle)
        wlr_ext_foreign_toplevel_handle_v1_destroy(toplevel->ext_foreign_handle);
    // The chrome resource may outlive the toplevel (the client owns it). Detach
    // its toplevel pointer but keep the allocation for the resource destroy
    // callback, or Valgrind reports a definite leak on every clean shutdown.
    serenity_window_toplevel_destroyed(*toplevel);
    if (server.window_menu.open && server.window_menu.toplevel == toplevel)
        close_window_menu(server);
    // `scene_tree` is our own wrapper around the xdg surface's content tree (so
    // the frame buffer can share a node with the content). wlroots only tears
    // down the content tree it created, so without this the frame buffer keeps
    // rendering after the client is gone and the window appears to freeze on
    // screen. Destroying the wrapper also destroys the content child, which
    // detaches wlroots' listener before the xdg_surface itself goes away.
    wlr_scene_node_destroy(&toplevel->scene_tree->node);
    dbgln("WindowServer: toplevel destroyed wm_id={}", toplevel->wm_id);
    WindowManager::emit_window_removed(*toplevel);
    // A destroyed window's application may no longer be running (R4.2). Treat it
    // as unmapped first: a client that goes away can be destroyed without an unmap.
    toplevel->mapped = false;
    SessionManager::update_running_apps(server);
    // A client's decoration may outlive its toplevel: wlroots destroys the
    // wlr_xdg_toplevel first and emits the decoration destroy afterwards. Detach
    // the back-pointer now, or decoration_destroy() reads freed Toplevel memory
    // (found by the ASan run).
    for (auto& decoration : server.decorations) {
        if (decoration->toplevel == toplevel)
            decoration->toplevel = nullptr;
    }
    remove_owned(server.toplevels, toplevel);
}

void toplevel_begin_interactive(Toplevel& toplevel, CursorMode mode, uint32_t edges)
{
    auto& server = *toplevel.server;
    if (mode == CursorMode::Move) {
        // SerenityOS: dragging a maximized window restores its size, keeping the
        // cursor at the same horizontal fraction of the window.
        if (toplevel.maximized && !toplevel.restore_size.is_empty()) {
            int spanned_width = max(1, toplevel.frame_size.width());
            double fraction_x = clamp((server.cursor->x - toplevel.scene_tree->node.x) / spanned_width, 0.0, 1.0);
            toplevel.maximized = false;
            wlr_xdg_toplevel_set_maximized(toplevel.xdg_toplevel, false);
            wlr_xdg_toplevel_set_size(toplevel.xdg_toplevel, toplevel.restore_size.width(), toplevel.restore_size.height());
            if (toplevel.foreign_handle)
                wlr_foreign_toplevel_handle_v1_set_maximized(toplevel.foreign_handle, false);
            double new_x = server.cursor->x - fraction_x * toplevel.restore_size.width();
            wlr_scene_node_set_position(&toplevel.scene_tree->node, new_x, toplevel.scene_tree->node.y);
            toplevel_report_position(toplevel);
        }
        begin_grab(server, &toplevel, nullptr, mode, server.cursor->x - toplevel.scene_tree->node.x, server.cursor->y - toplevel.scene_tree->node.y);
        return;
    }
    auto* geometry = &toplevel.xdg_toplevel->base->geometry;
    double border_x = (toplevel.scene_tree->node.x + geometry->x) + ((edges & WLR_EDGE_RIGHT) ? geometry->width : 0);
    double border_y = (toplevel.scene_tree->node.y + geometry->y) + ((edges & WLR_EDGE_BOTTOM) ? geometry->height : 0);
    wlr_box box = *geometry;
    box.x += toplevel.scene_tree->node.x;
    box.y += toplevel.scene_tree->node.y;
    begin_grab(server, &toplevel, nullptr, mode, server.cursor->x - border_x, server.cursor->y - border_y, box, edges);
}

void toplevel_close(Toplevel& toplevel)
{
    wlr_xdg_toplevel_send_close(toplevel.xdg_toplevel);
}

// Minimize/restore a window: state, visibility, the foreign handle, and the
// outline animation. Shared by the compositor's own minimize and the Taskbar's.
void set_toplevel_minimized(Server& server, Toplevel& toplevel, bool minimized)
{
    if (toplevel.minimized == minimized)
        return;

    Gfx::IntRect window_rect {
        toplevel.scene_tree->node.x, toplevel.scene_tree->node.y,
        toplevel.frame_size.width(), toplevel.frame_size.height()
    };
    if (window_rect.is_empty())
        window_rect = { toplevel.scene_tree->node.x, toplevel.scene_tree->node.y, toplevel.xdg_toplevel->base->geometry.width, toplevel.xdg_toplevel->base->geometry.height };

    toplevel.minimized = minimized;
    WindowManager::update_toplevel_visibility(toplevel);
    if (toplevel.foreign_handle)
        wlr_foreign_toplevel_handle_v1_set_minimized(toplevel.foreign_handle, minimized);
    start_minimize_animation(server, toplevel, window_rect, minimized);
}

// Fill the primary output. Safe to call on a window that is already maximised,
// which is what re-fitting needs: its old size was the workarea of an output it is
// no longer on (multi-output/hotplug), so the same work has to be redone against
// the new one — without clobbering the restore size the first pass saved.
void toplevel_maximize_on_primary(Toplevel& toplevel)
{
    auto& server = *toplevel.server;
    // A rolled-up window can't be meaningfully maximized; unroll it first so the
    // frame metrics used below are those of the full window.
    if (toplevel.rolled_up) {
        toplevel.rolled_up = false;
        toplevel_update_frame(toplevel);
    }
    if (!toplevel.maximized)
        toplevel.restore_size = { toplevel.xdg_toplevel->base->geometry.width, toplevel.xdg_toplevel->base->geometry.height };

    wlr_box box { };
    wlr_output_layout_get_box(server.output_layout, server.primary_output, &box);
    if (auto* output_state = output_for(server, server.primary_output); output_state && !wlr_box_empty(&output_state->usable_area))
        box = output_state->usable_area;
    toplevel.maximized = true;
    wlr_xdg_toplevel_set_maximized(toplevel.xdg_toplevel, true);
    if (server.primary_output)
        wlr_scene_node_set_position(&toplevel.scene_tree->node, box.x, box.y);
    toplevel_report_position(toplevel);
    wlr_xdg_toplevel_set_size(toplevel.xdg_toplevel, box.width - toplevel.frame_left - toplevel.frame_right, box.height - toplevel.frame_top - toplevel.frame_bottom);
    if (toplevel.foreign_handle)
        wlr_foreign_toplevel_handle_v1_set_maximized(toplevel.foreign_handle, true);
}

static void toplevel_set_maximized(Toplevel& toplevel, bool maximized)
{
    if (maximized) {
        toplevel_maximize_on_primary(toplevel);
        return;
    }
    if (!toplevel.maximized)
        return;
    toplevel.maximized = false;
    wlr_xdg_toplevel_set_maximized(toplevel.xdg_toplevel, false);
    wlr_xdg_toplevel_set_size(toplevel.xdg_toplevel, toplevel.restore_size.width(), toplevel.restore_size.height());
    if (toplevel.foreign_handle)
        wlr_foreign_toplevel_handle_v1_set_maximized(toplevel.foreign_handle, false);
}

void toplevel_toggle_maximized(Toplevel& toplevel)
{
    toplevel_set_maximized(toplevel, !toplevel.maximized);
}

void toplevel_toggle_minimized(Toplevel& toplevel)
{
    set_toplevel_minimized(*toplevel.server, toplevel, !toplevel.minimized);
}

static void toplevel_request_move(wl_listener* listener, void*)
{
    Toplevel* toplevel = nullptr;
    toplevel = wl_container_of(listener, toplevel, request_move);
    toplevel_begin_interactive(*toplevel, CursorMode::Move, 0);
}

static void toplevel_request_resize(wl_listener* listener, void* data)
{
    Toplevel* toplevel = nullptr;
    toplevel = wl_container_of(listener, toplevel, request_resize);
    auto* event = static_cast<wlr_xdg_toplevel_resize_event*>(data);
    toplevel_begin_interactive(*toplevel, CursorMode::Resize, event->edges);
}

static void toplevel_request_maximize(wl_listener* listener, void*)
{
    Toplevel* toplevel = nullptr;
    toplevel = wl_container_of(listener, toplevel, request_maximize);
    if (!toplevel->xdg_toplevel->base->initialized)
        return;
    // A client's own titlebar (Firefox's, GTK's) asks through here:
    // xdg_toplevel.set_maximized. Scheduling a configure *without applying
    // anything* is what this used to do — the request was noted and thrown away,
    // so the button did nothing. Apply the state the client asked for, through the
    // same path the compositor's own titlebar and the Taskbar use.
    bool const wanted = toplevel->xdg_toplevel->requested.maximized;
    if (wanted == toplevel->maximized) {
        wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
        return;
    }
    dbgln("WindowServer: '{}' asked to be {}",
        toplevel->xdg_toplevel->title ? toplevel->xdg_toplevel->title : "",
        wanted ? "maximized" : "restored");
    toplevel_set_maximized(*toplevel, wanted);
}

static void toplevel_request_minimize(wl_listener* listener, void*)
{
    Toplevel* toplevel = nullptr;
    toplevel = wl_container_of(listener, toplevel, request_minimize);
    if (!toplevel->xdg_toplevel->base->initialized)
        return;
    // xdg_shell has no way for a *compositor* to ask a client to minimise; this is
    // the client asking (Firefox's minimise button). It had no handler at all, so
    // the button did nothing.
    bool const wanted = toplevel->xdg_toplevel->requested.minimized;
    if (wanted == toplevel->minimized) {
        wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
        return;
    }
    dbgln("WindowServer: '{}' asked to be {}",
        toplevel->xdg_toplevel->title ? toplevel->xdg_toplevel->title : "",
        wanted ? "minimized" : "restored");
    set_toplevel_minimized(*toplevel->server, *toplevel, wanted);
}

static void toplevel_request_fullscreen(wl_listener* listener, void*)
{
    Toplevel* toplevel = nullptr;
    toplevel = wl_container_of(listener, toplevel, request_fullscreen);
    auto& server = *toplevel->server;
    if (!toplevel->xdg_toplevel->base->initialized)
        return;

    bool fullscreen = toplevel->xdg_toplevel->requested.fullscreen;
    dbgln("WindowServer: request_fullscreen={} title='{}'", fullscreen,
        toplevel->xdg_toplevel->title ? toplevel->xdg_toplevel->title : "");
    // Acknowledge the request. The client only learns it is fullscreen from the
    // configure we send back, so both the state and the size have to be set.
    wlr_xdg_toplevel_set_fullscreen(toplevel->xdg_toplevel, fullscreen);

    wlr_box box { };
    if (server.primary_output)
        wlr_output_layout_get_box(server.output_layout, server.primary_output, &box);

    if (fullscreen) {
        toplevel->restore_size = { toplevel->xdg_toplevel->base->geometry.width, toplevel->xdg_toplevel->base->geometry.height };
        toplevel->restore_position = { toplevel->scene_tree->node.x, toplevel->scene_tree->node.y };
        toplevel->fullscreen = true;
        if (!wlr_box_empty(&box)) {
            wlr_scene_node_set_position(&toplevel->scene_tree->node, box.x, box.y);
            wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, box.width, box.height);
        }
    } else {
        toplevel->fullscreen = false;
        wlr_scene_node_set_position(&toplevel->scene_tree->node, toplevel->restore_position.x(), toplevel->restore_position.y());
        wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, toplevel->restore_size.width(), toplevel->restore_size.height());
    }
    toplevel_update_frame(*toplevel);
}

static void toplevel_set_title(wl_listener* listener, void*)
{
    Toplevel* toplevel = nullptr;
    toplevel = wl_container_of(listener, toplevel, set_title);
    toplevel_update_foreign(*toplevel);
}

static void toplevel_set_app_id(wl_listener* listener, void*)
{
    Toplevel* toplevel = nullptr;
    toplevel = wl_container_of(listener, toplevel, set_app_id);
    toplevel_update_foreign(*toplevel);
}

static void toplevel_foreign_request_activate(wl_listener* listener, void*)
{
    Toplevel* toplevel = nullptr;
    toplevel = wl_container_of(listener, toplevel, foreign_request_activate);
    focus_toplevel(*toplevel->server, toplevel);
    WindowManager::emit_window_state(*toplevel);
}

static void toplevel_foreign_request_close(wl_listener* listener, void*)
{
    Toplevel* toplevel = nullptr;
    toplevel = wl_container_of(listener, toplevel, foreign_request_close);
    wlr_xdg_toplevel_send_close(toplevel->xdg_toplevel);
}

static void server_new_xdg_toplevel(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, new_xdg_toplevel);
    auto* xdg_toplevel = static_cast<wlr_xdg_toplevel*>(data);

    auto toplevel = make<Toplevel>();
    toplevel->server = server;
    toplevel->xdg_toplevel = xdg_toplevel;
    // Every window has an icon (SerenityOS behaviour): the client's, else the
    // default, so the titlebar, switcher and Taskbar always have one to draw.
    toplevel->icon = g_default_window_icon;
    toplevel->scene_tree = wlr_scene_tree_create(server->windows_tree);
    toplevel->scene_tree->node.data = toplevel.ptr();
    toplevel->content_tree = wlr_scene_xdg_surface_create(toplevel->scene_tree, xdg_toplevel->base);
    xdg_toplevel->base->data = toplevel->content_tree;
    use_nearest_filter(&toplevel->content_tree->node);

    toplevel->foreign_handle = create_foreign_toplevel_handle(*server);
    if (toplevel->foreign_handle) {
        toplevel->foreign_handle->data = toplevel.ptr();
        toplevel->foreign_request_activate.notify = toplevel_foreign_request_activate;
        wl_signal_add(&toplevel->foreign_handle->events.request_activate, &toplevel->foreign_request_activate);
        toplevel->foreign_request_close.notify = toplevel_foreign_request_close;
        wl_signal_add(&toplevel->foreign_handle->events.request_close, &toplevel->foreign_request_close);
    }

    toplevel->ext_foreign_handle = create_ext_foreign_toplevel_handle(*server, xdg_toplevel->title, xdg_toplevel->app_id);
    if (toplevel->ext_foreign_handle)
        toplevel->ext_foreign_handle->data = toplevel.ptr();

    toplevel->map.notify = toplevel_map;
    wl_signal_add(&xdg_toplevel->base->surface->events.map, &toplevel->map);
    toplevel->unmap.notify = toplevel_unmap;
    wl_signal_add(&xdg_toplevel->base->surface->events.unmap, &toplevel->unmap);
    toplevel->commit.notify = toplevel_commit;
    wl_signal_add(&xdg_toplevel->base->surface->events.commit, &toplevel->commit);
    toplevel->destroy.notify = toplevel_destroy;
    wl_signal_add(&xdg_toplevel->events.destroy, &toplevel->destroy);
    toplevel->request_move.notify = toplevel_request_move;
    wl_signal_add(&xdg_toplevel->events.request_move, &toplevel->request_move);
    toplevel->request_resize.notify = toplevel_request_resize;
    wl_signal_add(&xdg_toplevel->events.request_resize, &toplevel->request_resize);
    toplevel->request_maximize.notify = toplevel_request_maximize;
    wl_signal_add(&xdg_toplevel->events.request_maximize, &toplevel->request_maximize);
    toplevel->request_minimize.notify = toplevel_request_minimize;
    wl_signal_add(&xdg_toplevel->events.request_minimize, &toplevel->request_minimize);
    toplevel->request_fullscreen.notify = toplevel_request_fullscreen;
    wl_signal_add(&xdg_toplevel->events.request_fullscreen, &toplevel->request_fullscreen);
    toplevel->set_title.notify = toplevel_set_title;
    wl_signal_add(&xdg_toplevel->events.set_title, &toplevel->set_title);
    toplevel->set_app_id.notify = toplevel_set_app_id;
    wl_signal_add(&xdg_toplevel->events.set_app_id, &toplevel->set_app_id);

    server->toplevels.append(move(toplevel));
    server->toplevels.last()->wm_id = g_next_wm_id++;
}

static void popup_commit(wl_listener* listener, void*)
{
    Popup* popup = nullptr;
    popup = wl_container_of(listener, popup, commit);
    if (popup->xdg_popup->base->initial_commit)
        wlr_xdg_surface_schedule_configure(popup->xdg_popup->base);
}

static void popup_destroy(wl_listener* listener, void*)
{
    Popup* popup = nullptr;
    popup = wl_container_of(listener, popup, destroy);
    wl_list_remove(&popup->commit.link);
    wl_list_remove(&popup->destroy.link);
    remove_owned(popup->server->popups, popup);
}

static void server_new_xdg_popup(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, new_xdg_popup);
    auto* xdg_popup = static_cast<wlr_xdg_popup*>(data);

    auto popup = make<Popup>();
    popup->server = server;
    popup->xdg_popup = xdg_popup;

    auto* parent = wlr_xdg_surface_try_from_wlr_surface(xdg_popup->parent);
    if (!parent)
        return;
    auto* parent_tree = static_cast<wlr_scene_tree*>(parent->data);
    auto* popup_tree = wlr_scene_xdg_surface_create(parent_tree, xdg_popup->base);
    xdg_popup->base->data = popup_tree;
    use_nearest_filter(&popup_tree->node);

    popup->commit.notify = popup_commit;
    wl_signal_add(&xdg_popup->base->surface->events.commit, &popup->commit);
    popup->destroy.notify = popup_destroy;
    wl_signal_add(&xdg_popup->events.destroy, &popup->destroy);

    server->popups.append(move(popup));
}

// --- xdg-decoration -------------------------------------------------------

static void decoration_request_mode(wl_listener* listener, void*)
{
    Decoration* decoration = nullptr;
    decoration = wl_container_of(listener, decoration, request_mode);
    if (!decoration->toplevel)
        return;
    // Honour the client's request (CLIENT_SIDE -> no compositor frame; anything
    // else -> server-side decorations). Applied now if initialized, else at the
    // initial commit.
    apply_toplevel_decoration(*decoration->toplevel);
}

static void decoration_destroy(wl_listener* listener, void*)
{
    Decoration* decoration = nullptr;
    decoration = wl_container_of(listener, decoration, destroy);
    wl_list_remove(&decoration->request_mode.link);
    wl_list_remove(&decoration->destroy.link);
    if (decoration->toplevel && decoration->toplevel->decoration == decoration->decoration)
        decoration->toplevel->decoration = nullptr;
    remove_owned(decoration->server->decorations, decoration);
}

static void server_new_toplevel_decoration(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, new_toplevel_decoration);
    auto* wlr_decoration = static_cast<wlr_xdg_toplevel_decoration_v1*>(data);

    auto decoration = make<Decoration>();
    decoration->server = server;
    decoration->decoration = wlr_decoration;
    decoration->request_mode.notify = decoration_request_mode;
    wl_signal_add(&wlr_decoration->events.request_mode, &decoration->request_mode);
    decoration->destroy.notify = decoration_destroy;
    wl_signal_add(&wlr_decoration->events.destroy, &decoration->destroy);

    auto* xdg_toplevel = wlr_decoration->toplevel;
    for (auto& candidate : server->toplevels) {
        if (candidate->xdg_toplevel == xdg_toplevel) {
            decoration->toplevel = candidate.ptr();
            break;
        }
    }

    // Honour the client's decoration choice (Firefox/GTK ask for CSD); apply it
    // now if the toplevel is initialized, else at the initial commit.
    if (decoration->toplevel) {
        decoration->toplevel->decoration = wlr_decoration;
        apply_toplevel_decoration(*decoration->toplevel);
    }
    server->decorations.append(move(decoration));
}

void initialize_xdg_shell(Server& server)
{
    server.xdg_shell = wlr_xdg_shell_create(server.display, 3);
    server.new_xdg_toplevel.notify = server_new_xdg_toplevel;
    wl_signal_add(&server.xdg_shell->events.new_toplevel, &server.new_xdg_toplevel);
    server.new_xdg_popup.notify = server_new_xdg_popup;
    wl_signal_add(&server.xdg_shell->events.new_popup, &server.new_xdg_popup);

    if (auto* decoration_manager = wlr_xdg_decoration_manager_v1_create(server.display)) {
        server.new_toplevel_decoration.notify = server_new_toplevel_decoration;
        wl_signal_add(&decoration_manager->events.new_toplevel_decoration, &server.new_toplevel_decoration);
    }
}

void finish_xdg_shell(Server& server)
{
    remove_listener(server.new_xdg_toplevel);
    remove_listener(server.new_xdg_popup);
    remove_listener(server.new_toplevel_decoration);
}
