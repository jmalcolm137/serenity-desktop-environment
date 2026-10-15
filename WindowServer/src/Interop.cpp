/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Compositor.h"
#include "Focus.h"
#include "Frame.h"
#include "Interop.h"
#include "Pointer.h"
#include "Resources.h"
#include "WindowManager.h"

#include <string.h>

#include "Wlroots.h"

static void update_ext_workspace_active(Server& server);

// --- setup ----------------------------------------------------------------

// --- R4.8: standard interop (ext-workspace-v1, xdg-activation-v1) ----------

// Publish the SDE workspace grid over ext-workspace-v1: one group (the outputs)
// and one workspace per grid cell, with grid coordinates and the active state.
static void rebuild_ext_workspaces(Server& server)
{
    for (auto* workspace : server.ext_workspaces)
        wlr_ext_workspace_handle_v1_destroy(workspace);
    server.ext_workspaces.clear();
    if (!server.ext_workspace_manager)
        return;

    if (!server.ext_workspace_group) {
        server.ext_workspace_group = wlr_ext_workspace_group_handle_v1_create(server.ext_workspace_manager, 0);
        for (auto& output : server.outputs)
            wlr_ext_workspace_group_handle_v1_output_enter(server.ext_workspace_group, output->output);
    }

    u32 rows = WindowManager::workspace_rows();
    u32 columns = WindowManager::workspace_columns();
    u32 current_row = WindowManager::current_workspace_row();
    u32 current_column = WindowManager::current_workspace_column();
    for (u32 row = 0; row < rows; ++row) {
        for (u32 column = 0; column < columns; ++column) {
            auto id = ByteString::formatted("{},{}", row, column);
            auto* workspace = wlr_ext_workspace_handle_v1_create(server.ext_workspace_manager, id.characters(), EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ACTIVATE);
            auto name = ByteString::formatted("{}", row * columns + column + 1);
            wlr_ext_workspace_handle_v1_set_name(workspace, name.characters());
            uint32_t coordinates[2] = { column, row };
            wlr_ext_workspace_handle_v1_set_coordinates(workspace, coordinates, 2);
            wlr_ext_workspace_handle_v1_set_group(workspace, server.ext_workspace_group);
            wlr_ext_workspace_handle_v1_set_active(workspace, row == current_row && column == current_column);
            server.ext_workspaces.append(workspace);
        }
    }
    dbgln("WindowServer: ext-workspace-v1 published {}x{} grid", rows, columns);
}

void on_workspace_changed(Server& server)
{
    update_ext_workspace_active(server);
    // SerenityOS: switching window stacks closes any fragile (popup/menu) window.
    dismiss_fragile_windows(server, "workspace switch"sv);
}

static void update_ext_workspace_active(Server& server)
{
    if (!server.ext_workspace_manager)
        return;
    u32 columns = max(1u, WindowManager::workspace_columns());
    if (server.ext_workspaces.size() != WindowManager::workspace_rows() * columns) {
        rebuild_ext_workspaces(server);
        return;
    }
    u32 current_row = WindowManager::current_workspace_row();
    u32 current_column = WindowManager::current_workspace_column();
    for (size_t index = 0; index < server.ext_workspaces.size(); ++index) {
        u32 row = static_cast<u32>(index) / columns;
        u32 column = static_cast<u32>(index) % columns;
        wlr_ext_workspace_handle_v1_set_active(server.ext_workspaces[index], row == current_row && column == current_column);
    }
}

void ext_workspace_commit(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, ext_workspace_commit);
    auto* event = static_cast<wlr_ext_workspace_v1_commit_event*>(data);
    wlr_ext_workspace_v1_request* request = nullptr;
    wl_list_for_each(request, event->requests, link)
    {
        if (request->type != WLR_EXT_WORKSPACE_V1_REQUEST_ACTIVATE || !request->activate.workspace)
            continue;
        auto* workspace = request->activate.workspace;
        if (workspace->coordinates.size < 2 * sizeof(uint32_t))
            continue;
        auto const* coordinates = static_cast<uint32_t const*>(workspace->coordinates.data);
        dbgln("WindowServer: ext-workspace-v1 activate {},{}", coordinates[1], coordinates[0]);
        WindowManager::switch_workspace(*server, coordinates[1], coordinates[0], false);
    }
}

// xdg-activation-v1: a client asked to be focused. Switch to its workspace and
// focus it.
void xdg_activation_activate(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, xdg_activation_activate);
    auto* event = static_cast<wlr_xdg_activation_v1_request_activate_event*>(data);
    for (auto& toplevel : server->toplevels) {
        if (!toplevel->xdg_toplevel || toplevel->xdg_toplevel->base->surface != event->surface)
            continue;
        WindowManager::switch_workspace(*server, toplevel->workspace_row, toplevel->workspace_column, false);
        focus_toplevel(*server, toplevel.ptr());
        dbgln("WindowServer: xdg-activation focused '{}'", toplevel->xdg_toplevel->title ? toplevel->xdg_toplevel->title : "");
        break;
    }
}

// wp-cursor-shape-v1: a client asked for a named cursor shape (e.g. "text").
void cursor_shape_request(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, cursor_shape_request);
    auto* event = static_cast<wlr_cursor_shape_manager_v1_request_set_shape_event*>(data);
    if (event->device_type != WLR_CURSOR_SHAPE_MANAGER_V1_DEVICE_TYPE_POINTER)
        return;
    if (auto const* name = wlr_cursor_shape_v1_name(event->shape); name && server->cursor)
        wlr_cursor_set_xcursor(server->cursor, server->cursor_manager, name);
}

// --- R5 protocol parity: xdg-toplevel-icon-v1 + xdg-system-bell-v1 (docs/21) --

static Toplevel* toplevel_for_xdg(Server& server, wlr_xdg_toplevel* xdg_toplevel)
{
    for (auto& toplevel : server.toplevels) {
        if (toplevel->xdg_toplevel == xdg_toplevel)
            return toplevel.ptr();
    }
    return nullptr;
}

// wl_shm fourcc channel order: ARGB8888/XRGB8888 are B,G,R,(X) in memory; the
// XBGR/ABGR variants are R,G,B,(X). (Same values as the DRM formats.)
static bool shm_format_is_bgr(uint32_t format)
{
    return format == 0x34325241u /* ARGB8888 */ || format == 0x34325258u /* XRGB8888 */;
}

static bool shm_format_is_rgb(uint32_t format)
{
    return format == 0x34324241u /* ABGR8888 */ || format == 0x34324258u /* XBGR8888 */;
}

// xdg-toplevel-icon-v1: a client set (or cleared) a window icon. Convert the
// chosen buffer to a Gfx::Bitmap and report it, like the X11 _NET_WM_ICON path
// (docs/20 item 3) — the Taskbar button and the Alt-Tab switcher draw it.
void toplevel_icon_set(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, toplevel_icon_set);
    auto* event = static_cast<wlr_xdg_toplevel_icon_manager_v1_set_icon_event*>(data);
    auto* toplevel = toplevel_for_xdg(*server, event->toplevel);
    if (!toplevel)
        return;

    RefPtr<Gfx::Bitmap> icon;
    if (event->icon) {
        // Prefer a scale-1 buffer nearest 16x16 (the Taskbar/switcher size).
        wlr_xdg_toplevel_icon_v1_buffer* best = nullptr;
        int best_score = -1000000;
        wlr_xdg_toplevel_icon_v1_buffer* buffer = nullptr;
        wl_list_for_each(buffer, &event->icon->buffers, link)
        {
            if (!buffer->buffer)
                continue;
            int const w = buffer->buffer->width;
            int const distance = w >= 16 ? w - 16 : 16 - w;
            int const score = (buffer->scale == 1 ? 100 : 0) - distance;
            if (score > best_score) {
                best_score = score;
                best = buffer;
            }
        }
        if (best) {
            auto* buffer = best->buffer;
            void* pixels = nullptr;
            uint32_t format = 0;
            size_t stride = 0;
            if (wlr_buffer_begin_data_ptr_access(buffer, WLR_BUFFER_DATA_PTR_ACCESS_READ, &pixels, &format, &stride)) {
                int const w = buffer->width;
                int const h = buffer->height;
                if ((shm_format_is_bgr(format) || shm_format_is_rgb(format)) && w > 0 && h > 0 && stride >= static_cast<size_t>(w) * 4) {
                    if (auto bitmap = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, { w, h }); !bitmap.is_error()) {
                        auto& created = *bitmap.value();
                        for (int y = 0; y < h; ++y) {
                            auto const* src = static_cast<uint8_t const*>(pixels) + static_cast<size_t>(y) * stride;
                            auto* dst = created.scanline(y);
                            if (shm_format_is_bgr(format)) {
                                memcpy(dst, src, static_cast<size_t>(w) * 4);
                            } else {
                                for (int x = 0; x < w; ++x) {
                                    dst[x * 4 + 0] = src[x * 4 + 2];
                                    dst[x * 4 + 1] = src[x * 4 + 1];
                                    dst[x * 4 + 2] = src[x * 4 + 0];
                                    dst[x * 4 + 3] = src[x * 4 + 3];
                                }
                            }
                        }
                        icon = bitmap.release_value();
                    }
                }
                wlr_buffer_end_data_ptr_access(buffer);
            }
        }
    }

    toplevel->icon = icon ? move(icon) : g_default_window_icon;
    WindowManager::emit_window_icon(*toplevel);
    toplevel_update_frame(*toplevel);
    dbgln("WindowServer: xdg-toplevel-icon set for wm_id={} ({}x{})", toplevel->wm_id,
        toplevel->icon ? toplevel->icon->width() : 0, toplevel->icon ? toplevel->icon->height() : 0);
}

// xdg-system-bell-v1: a client asked for the system bell. Advertise the global so
// such clients do not fail; the audible/visual bell itself is not implemented.
void system_bell_ring(wl_listener* listener, void*)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, system_bell_ring);
    (void)server;
    dbgln("WindowServer: xdg-system-bell ring (bell output not implemented)");
}
