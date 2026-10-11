/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "WindowManager.h"

#include <AK/ByteString.h>
#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibCore/Socket.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibGfx/Painter.h>
#include <LibGfx/Palette.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>
#include <LibIPC/Connection.h>
#include <WindowServer/WindowManagerClientEndpoint.h>
#include <WindowServer/WindowManagerServerEndpoint.h>
#include <WindowServer/WindowType.h>
#include <string.h>
#include <xkbcommon/xkbcommon.h>

#include "AppletManager.h"
#include "Config.h"
#include "Overlays.h"
#include "WindowManagerServerDefaultStub.h"

// wlroots is a C library and does not wrap its headers in extern "C", so we do.
extern "C" {
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/edges.h>
}

namespace WindowManager {

static HostHooks g_hooks;

void set_host_hooks(HostHooks hooks)
{
    g_hooks = hooks;
}

// The compositor's end of the Serenity WindowManagerServer protocol.
class WindowManagerServerConnection final
    : public IPC::Connection<WindowManagerServerEndpoint, WindowManagerClientEndpoint>
    , public ::SDE::WindowManagerServerDefaultStub
    , public WindowManagerClientEndpoint::Proxy<WindowManagerServerEndpoint> {
public:
    static NonnullRefPtr<WindowManagerServerConnection> create(Server& server, NonnullOwnPtr<Core::LocalSocket> socket)
    {
        return adopt_ref(*new WindowManagerServerConnection(server, move(socket)));
    }

    i32 manager_window_id() const { return m_manager_window_id; }

    virtual void set_event_mask(u32 event_mask) override { m_event_mask = event_mask; }

    virtual void set_manager_window(i32 window_id) override
    {
        m_manager_window_id = window_id;
        for (auto& toplevel : m_server.toplevels)
            emit_window_state(*toplevel);
    }

    virtual void set_active_window(i32, i32 window_id) override
    {
        if (auto* toplevel = find_toplevel_by_wm_id(m_server, window_id)) {
            if (toplevel->minimized) {
                toplevel->minimized = false;
                update_toplevel_visibility(*toplevel);
            }
            if (g_hooks.focus_toplevel)
                g_hooks.focus_toplevel(m_server, toplevel);
            emit_window_state(*toplevel);
        }
    }

    virtual void set_window_minimized(i32, i32 window_id, bool minimized) override
    {
        if (auto* toplevel = find_toplevel_by_wm_id(m_server, window_id)) {
            toplevel->minimized = minimized;
            update_toplevel_visibility(*toplevel);
            emit_window_state(*toplevel);
        }
    }

    virtual void toggle_show_desktop() override
    {
        auto& server = m_server;
        server.show_desktop = !server.show_desktop;
        for (auto& toplevel : server.toplevels) {
            bool changed = false;
            if (server.show_desktop) {
                if (!toplevel->minimized) {
                    toplevel->hidden_by_show_desktop = true;
                    toplevel->minimized = true;
                    update_toplevel_visibility(*toplevel);
                    changed = true;
                }
            } else if (toplevel->hidden_by_show_desktop) {
                toplevel->hidden_by_show_desktop = false;
                toplevel->minimized = false;
                update_toplevel_visibility(*toplevel);
                changed = true;
            }
            if (changed)
                emit_window_state(*toplevel);
        }
    }
    virtual void start_window_resize(i32, i32 window_id, i32 resize_direction) override
    {
        // The Taskbar asks the compositor to begin resizing a window.
        auto* toplevel = find_toplevel_by_wm_id(m_server, window_id);
        if (!toplevel)
            return;
        if (g_hooks.begin_interactive)
            g_hooks.begin_interactive(*toplevel, CursorMode::Resize, static_cast<uint32_t>(resize_direction));
    }
    virtual void popup_window_menu(i32, i32 window_id, Gfx::IntPoint screen_position) override
    {
        // Right-clicking a Taskbar window-list button opens the window menu. The
        // compositor already renders that menu for the titlebar, so reuse it (the
        // layout clamps it on-screen, which places it above the Taskbar).
        if (auto* toplevel = find_toplevel_by_wm_id(m_server, window_id)) {
            if (g_hooks.open_window_menu)
                g_hooks.open_window_menu(m_server, *toplevel, screen_position);
        }
    }
    // The Taskbar reports each button's rect for a minimise/restore animation the
    // compositor does not implement; deliberately not consumed (see known-gaps).
    virtual void set_window_taskbar_rect(i32, i32, Gfx::IntRect const&) override { }

    // Applets are layer surfaces; the compositor owns their layout (WS4). The
    // Taskbar tells us where the applet area sits and we place each applet.
    virtual void set_applet_area_position(Gfx::IntPoint position) override
    {
        AppletManager::set_position(m_server, position);
    }

    virtual void set_workspace(u32 row, u32 column) override
    {
        switch_workspace(m_server, row, column, false);
    }
    virtual void set_keymap(ByteString const&) override { }

private:
    explicit WindowManagerServerConnection(Server& server, NonnullOwnPtr<Core::LocalSocket> socket)
        : IPC::Connection<WindowManagerServerEndpoint, WindowManagerClientEndpoint>(*this, move(socket))
        , WindowManagerClientEndpoint::Proxy<WindowManagerServerEndpoint>(*this, { })
        , m_server(server)
    {
    }

    Server& m_server;
    i32 m_manager_window_id { -1 };
    u32 m_event_mask { 0 };
};

static Vector<NonnullRefPtr<WindowManagerServerConnection>> g_connections;

void accept_connection(Server& server, NonnullOwnPtr<Core::LocalSocket> socket)
{
    g_connections.append(WindowManagerServerConnection::create(server, move(socket)));
}

Toplevel* find_toplevel_by_wm_id(Server& server, i32 wm_id)
{
    for (auto& toplevel : server.toplevels) {
        if (toplevel->wm_id == wm_id)
            return toplevel.ptr();
    }
    return nullptr;
}

// --- snapping (WS2) ---------------------------------------------------------

static Toplevel* focused_toplevel(Server& server)
{
    auto* surface = server.seat->keyboard_state.focused_surface;
    if (!surface)
        return nullptr;
    auto* xdg_toplevel = wlr_xdg_toplevel_try_from_wlr_surface(surface);
    if (!xdg_toplevel)
        return nullptr;
    auto* content_tree = static_cast<wlr_scene_tree*>(xdg_toplevel->base->data);
    if (!content_tree)
        return nullptr;
    auto* frame_tree = content_tree->node.parent;
    if (!frame_tree)
        return nullptr;
    return static_cast<Toplevel*>(frame_tree->node.data);
}

static Gfx::IntRect usable_area(Server& server)
{
    if (g_hooks.primary_usable_area)
        return g_hooks.primary_usable_area(server);
    return {};
}

// The geometry a tile type covers within the usable area.
static Gfx::IntRect tile_rect(Gfx::IntRect const& area, WindowTileType type)
{
    Gfx::IntRect rect = area;
    int const half_w = area.width() / 2;
    int const half_h = area.height() / 2;
    switch (type) {
    case WindowTileType::Left:
        rect.set_width(half_w);
        break;
    case WindowTileType::Right:
        rect.set_x(area.x() + half_w);
        rect.set_width(area.width() - half_w);
        break;
    case WindowTileType::Top:
        rect.set_height(half_h);
        break;
    case WindowTileType::Bottom:
        rect.set_y(area.y() + half_h);
        rect.set_height(area.height() - half_h);
        break;
    case WindowTileType::TopLeft:
        rect.set_size({ half_w, half_h });
        break;
    case WindowTileType::TopRight:
        rect.set_x(area.x() + half_w);
        rect.set_size({ area.width() - half_w, half_h });
        break;
    case WindowTileType::BottomLeft:
        rect.set_y(area.y() + half_h);
        rect.set_size({ half_w, area.height() - half_h });
        break;
    case WindowTileType::BottomRight:
        rect.set_location({ area.x() + half_w, area.y() + half_h });
        rect.set_size({ area.width() - half_w, area.height() - half_h });
        break;
    default:
        break; // Maximized / horizontally / vertically maximized = the whole area
    }
    return rect;
}

// The xdg_toplevel tiled edges for a tile type.
static uint32_t tile_edges(WindowTileType type)
{
    switch (type) {
    case WindowTileType::Left:
        return WLR_EDGE_LEFT | WLR_EDGE_TOP | WLR_EDGE_BOTTOM;
    case WindowTileType::Right:
        return WLR_EDGE_RIGHT | WLR_EDGE_TOP | WLR_EDGE_BOTTOM;
    case WindowTileType::Top:
        return WLR_EDGE_TOP | WLR_EDGE_LEFT | WLR_EDGE_RIGHT;
    case WindowTileType::Bottom:
        return WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT;
    case WindowTileType::HorizontallyMaximized:
        return WLR_EDGE_TOP | WLR_EDGE_BOTTOM;
    case WindowTileType::VerticallyMaximized:
        return WLR_EDGE_LEFT | WLR_EDGE_RIGHT;
    case WindowTileType::Maximized:
        return WLR_EDGE_LEFT | WLR_EDGE_RIGHT | WLR_EDGE_TOP | WLR_EDGE_BOTTOM;
    default:
        return 0;
    }
}

static void apply_tile_geometry(Toplevel& toplevel, Gfx::IntRect const& rect, uint32_t xdg_edges)
{
    wlr_xdg_toplevel_set_tiled(toplevel.xdg_toplevel, xdg_edges);
    wlr_xdg_toplevel_set_maximized(toplevel.xdg_toplevel, toplevel.tile_type == WindowTileType::Maximized);
    wlr_scene_node_set_position(&toplevel.scene_tree->node, rect.x(), rect.y());
    if (g_hooks.report_position)
        g_hooks.report_position(toplevel);
    wlr_xdg_toplevel_set_size(toplevel.xdg_toplevel,
        max(1, rect.width() - toplevel.frame_left - toplevel.frame_right),
        max(1, rect.height() - toplevel.frame_top - toplevel.frame_bottom));
}

static void set_untiled(Toplevel& toplevel)
{
    toplevel.tile_type = WindowTileType::None;
    toplevel.maximized = false;
    wlr_xdg_toplevel_set_tiled(toplevel.xdg_toplevel, 0);
    wlr_xdg_toplevel_set_maximized(toplevel.xdg_toplevel, false);
    wlr_scene_node_set_position(&toplevel.scene_tree->node, toplevel.restore_position.x(), toplevel.restore_position.y());
    if (g_hooks.report_position)
        g_hooks.report_position(toplevel);
    wlr_xdg_toplevel_set_size(toplevel.xdg_toplevel, toplevel.restore_size.width(), toplevel.restore_size.height());
}

static void set_tiled(Server& server, Toplevel& toplevel, WindowTileType type)
{
    // Remember where to restore to on the first tile.
    if (toplevel.tile_type == WindowTileType::None) {
        toplevel.restore_size = { toplevel.xdg_toplevel->base->geometry.width, toplevel.xdg_toplevel->base->geometry.height };
        toplevel.restore_position = { toplevel.scene_tree->node.x, toplevel.scene_tree->node.y };
    }

    auto area = usable_area(server);
    if (area.is_empty())
        return;

    auto rect = tile_rect(area, type);
    uint32_t const xdg_edges = tile_edges(type);

    toplevel.tile_type = type;
    toplevel.maximized = type == WindowTileType::Maximized;
    apply_tile_geometry(toplevel, rect, xdg_edges);
    emit_window_state(toplevel);
    dbgln("WindowServer: tiled wm_id={} as {}", toplevel.wm_id, to_underlying(type));
}

bool handle_tile_key(Server& server, u32 keysym, u32 modifiers)
{
    // Plain Super (or Super+Alt); Ctrl/Shift combos belong to other bindings.
    if ((modifiers & WLR_MODIFIER_LOGO) == 0)
        return false;
    if ((modifiers & (WLR_MODIFIER_CTRL | WLR_MODIFIER_SHIFT)) != 0)
        return false;
    bool const with_alt = (modifiers & WLR_MODIFIER_ALT) != 0;

    auto* toplevel = focused_toplevel(server);
    if (!toplevel)
        return false;

    if (!with_alt && keysym == XKB_KEY_Up) {
        if (toplevel->maximized)
            set_untiled(*toplevel);
        else
            set_tiled(server, *toplevel, WindowTileType::Maximized);
        return true;
    }
    if (!with_alt && keysym == XKB_KEY_Down) {
        if (toplevel->maximized) {
            set_untiled(*toplevel);
        } else {
            toplevel->minimized = true;
            update_toplevel_visibility(*toplevel);
            emit_window_state(*toplevel);
        }
        return true;
    }
    if (!with_alt && keysym == XKB_KEY_Left) {
        if (toplevel->tile_type == WindowTileType::Left)
            set_untiled(*toplevel);
        else
            set_tiled(server, *toplevel, WindowTileType::Left);
        return true;
    }
    if (!with_alt && keysym == XKB_KEY_Right) {
        if (toplevel->tile_type == WindowTileType::Right)
            set_untiled(*toplevel);
        else
            set_tiled(server, *toplevel, WindowTileType::Right);
        return true;
    }
    if (with_alt && (keysym == XKB_KEY_Left || keysym == XKB_KEY_Right)) {
        if (toplevel->tile_type == WindowTileType::HorizontallyMaximized)
            set_untiled(*toplevel);
        else
            set_tiled(server, *toplevel, WindowTileType::HorizontallyMaximized);
        return true;
    }
    if (with_alt && (keysym == XKB_KEY_Up || keysym == XKB_KEY_Down)) {
        if (toplevel->tile_type == WindowTileType::VerticallyMaximized)
            set_untiled(*toplevel);
        else
            set_tiled(server, *toplevel, WindowTileType::VerticallyMaximized);
        return true;
    }
    return false;
}

// --- drag-to-edge snapping --------------------------------------------------

static OwnPtr<ChromeNode> s_tile_preview;
static WindowTileType s_pending_tile { WindowTileType::None };

// --- geometry HUD (WS2) -----------------------------------------------------
//
// SerenityOS WindowGeometryOverlay: a small size readout shown while moving or
// resizing. During a move that is previewing a tile it shows the prospective tile
// size instead of the window's.

static OwnPtr<ChromeNode> s_geometry_hud;

void hide_geometry_hud()
{
    if (s_geometry_hud)
        s_geometry_hud->set_visible(false);
}

static void show_geometry_hud(Server& server, Gfx::IntRect geometry)
{
    if (!server.overlay_tree || geometry.is_empty())
        return;

    auto label = ByteString::formatted("{}x{}", geometry.width(), geometry.height());
    auto const& font = Gfx::FontDatabase::default_font();
    int const pad_x = 10;
    int const pad_y = 4;
    Gfx::IntSize const hud_size { font.width_rounded_up(label) + pad_x * 2, font.pixel_size_rounded_up() + pad_y * 2 };
    Gfx::IntRect hud_rect { {}, hud_size };
    hud_rect.center_within(geometry);

    // Keep the HUD on the screen.
    auto const area = usable_area(server);
    if (!area.is_empty()) {
        if (hud_rect.left() < area.left())
            hud_rect.set_x(area.x());
        if (hud_rect.top() < area.top())
            hud_rect.set_y(area.y());
        if (hud_rect.right() > area.right())
            hud_rect.set_right_without_resize(area.right());
        if (hud_rect.bottom() > area.bottom())
            hud_rect.set_bottom_without_resize(area.bottom());
    }

    if (!s_geometry_hud)
        s_geometry_hud = make<ChromeNode>(*server.overlay_tree);
    s_geometry_hud->set_rect(hud_rect);

    auto const* palette = g_hooks.palette ? &g_hooks.palette(server) : nullptr;
    s_geometry_hud->render([&](Gfx::Painter& painter) {
        Gfx::IntRect local { {}, hud_size };
        Gfx::Color background = Gfx::Color::from_rgb(0x2b2b2b).with_alpha(0xe0);
        Gfx::Color border = Gfx::Color::from_rgb(0x9db8d2);
        Gfx::Color text = Gfx::Color::White;
        if (palette) {
            background = palette->overlay_background();
            border = palette->overlay_text().with_alpha(0x80);
            text = palette->overlay_text();
        }
        painter.fill_rect(local, background);
        painter.draw_rect(local.shrunken(1, 1), border);
        painter.draw_ui_text(local, label, font, Gfx::TextAlignment::Center, text);
    });
    s_geometry_hud->set_visible(true);
    s_geometry_hud->raise_to_top();
}

void update_resize_hud(Server& server, Toplevel& toplevel, Gfx::IntRect content_rect)
{
    // Show the frame rect (content plus the server-side frame), like SerenityOS.
    Gfx::IntRect geometry {
        content_rect.x() - toplevel.frame_left,
        content_rect.y() - toplevel.frame_top,
        content_rect.width() + toplevel.frame_left + toplevel.frame_right,
        content_rect.height() + toplevel.frame_top + toplevel.frame_bottom,
    };
    show_geometry_hud(server, geometry);
}

static Gfx::IntRect toplevel_frame_rect(Toplevel const& toplevel)
{
    return { toplevel.scene_tree->node.x, toplevel.scene_tree->node.y, toplevel.frame_size.width(), toplevel.frame_size.height() };
}

void update_move_snap(Server& server, Toplevel& toplevel)
{
    auto area = usable_area(server);
    if (area.is_empty())
        return;

    int const edge = 48;
    double const x = server.cursor->x;
    double const y = server.cursor->y;

    WindowTileType target = WindowTileType::None;
    if (x <= area.x() + edge)
        target = y <= area.y() + edge ? WindowTileType::TopLeft
            : y >= area.bottom() - edge ? WindowTileType::BottomLeft
                                        : WindowTileType::Left;
    else if (x >= area.right() - edge)
        target = y <= area.y() + edge ? WindowTileType::TopRight
            : y >= area.bottom() - edge ? WindowTileType::BottomRight
                                        : WindowTileType::Right;
    else if (y <= area.y() + edge)
        target = WindowTileType::Top;
    else if (y >= area.bottom() - edge)
        target = WindowTileType::Bottom;

    s_pending_tile = target;

    if (target != WindowTileType::None) {
        if (server.overlay_tree) {
            if (!s_tile_preview)
                s_tile_preview = make<ChromeNode>(*server.overlay_tree);
            auto rect = tile_rect(area, target);
            s_tile_preview->set_rect(rect);
            s_tile_preview->render([rect](Gfx::Painter& painter) {
                Gfx::IntRect local { { }, rect.size() };
                painter.fill_rect(local, Gfx::Color::from_rgb(0x3b6ea5).with_alpha(0x50));
                painter.draw_rect(local.shrunken(1, 1), Gfx::Color::from_rgb(0x9db8d2).with_alpha(0xc0));
            });
            s_tile_preview->set_visible(true);
            s_tile_preview->raise_to_top();
        }
    } else if (s_tile_preview) {
        s_tile_preview->set_visible(false);
    }

    // The HUD previews the prospective tile while one is shown, else the window.
    auto geometry = target != WindowTileType::None ? tile_rect(area, target) : toplevel_frame_rect(toplevel);
    show_geometry_hud(server, geometry);
}

void finish_move_snap(Server& server, Toplevel& toplevel)
{
    if (s_tile_preview)
        s_tile_preview->set_visible(false);
    hide_geometry_hud();
    if (s_pending_tile != WindowTileType::None)
        set_tiled(server, toplevel, s_pending_tile);
    s_pending_tile = WindowTileType::None;
}

// --- workspaces (WS1) -------------------------------------------------------

static u32 s_rows { 2 };
static u32 s_columns { 2 };
static u32 s_current_row { 0 };
static u32 s_current_column { 0 };

u32 workspace_rows() { return s_rows; }
u32 workspace_columns() { return s_columns; }
u32 current_workspace_row() { return s_current_row; }
u32 current_workspace_column() { return s_current_column; }

void update_toplevel_visibility(Toplevel& toplevel)
{
    bool const on_current = toplevel.workspace_row == s_current_row && toplevel.workspace_column == s_current_column;
    wlr_scene_node_set_enabled(&toplevel.scene_tree->node, on_current && !toplevel.minimized);
}

void on_toplevel_mapped(Toplevel& toplevel)
{
    toplevel.workspace_row = s_current_row;
    toplevel.workspace_column = s_current_column;
    update_toplevel_visibility(toplevel);
}

static void emit_workspace_changed(Server& server)
{
    for (auto& connection : g_connections) {
        if (connection->manager_window_id() >= 0)
            connection->async_workspace_changed(connection->manager_window_id(), s_current_row, s_current_column);
    }
    (void)server;
}

void load_workspaces(Server& server)
{
    auto const& grid = Config::workspaces();
    s_rows = static_cast<u32>(max(1, grid.rows));
    s_columns = static_cast<u32>(max(1, grid.columns));
    for (auto& toplevel : server.toplevels)
        update_toplevel_visibility(*toplevel);
    dbgln("WindowServer: workspaces {}x{}, current {},{}", s_rows, s_columns, s_current_row, s_current_column);
}

void switch_workspace(Server& server, u32 row, u32 column, bool carry_focused)
{
    if (row >= s_rows || column >= s_columns)
        return;
    if (row == s_current_row && column == s_current_column)
        return;

    // Carry the focused window into the target stack (Ctrl+Shift+Alt+arrow).
    Toplevel* carried = carry_focused ? focused_toplevel(server) : nullptr;

    s_current_row = row;
    s_current_column = column;
    if (carried) {
        carried->workspace_row = row;
        carried->workspace_column = column;
    }

    for (auto& toplevel : server.toplevels)
        update_toplevel_visibility(*toplevel);

    // Focus a visible window on the new stack so the keyboard follows.
    for (auto& toplevel : server.toplevels) {
        if (toplevel->workspace_row == row && toplevel->workspace_column == column && !toplevel->minimized) {
            if (g_hooks.focus_toplevel)
                g_hooks.focus_toplevel(server, toplevel.ptr());
            break;
        }
    }

    for (auto& toplevel : server.toplevels)
        emit_window_state(*toplevel);
    emit_workspace_changed(server);
    dbgln("WindowServer: switched to workspace {},{}", row, column);
}

bool handle_workspace_key(Server& server, u32 keysym, u32 modifiers)
{
    bool const ctrl = (modifiers & WLR_MODIFIER_CTRL) != 0;
    bool const alt = (modifiers & WLR_MODIFIER_ALT) != 0;
    if (!ctrl || !alt)
        return false;
    bool const carry = (modifiers & WLR_MODIFIER_SHIFT) != 0;

    u32 row = s_current_row;
    u32 column = s_current_column;
    switch (keysym) {
    case XKB_KEY_Left:
        if (column == 0)
            return true;
        column--;
        break;
    case XKB_KEY_Right:
        if (column + 1 >= s_columns)
            return true;
        column++;
        break;
    case XKB_KEY_Up:
        if (row == 0)
            return true;
        row--;
        break;
    case XKB_KEY_Down:
        if (row + 1 >= s_rows)
            return true;
        row++;
        break;
    default:
        return false;
    }

    switch_workspace(server, row, column, carry);
    return true;
}

bool handle_super_key(Server& server, u32 keysym, u32 modifiers, bool pressed)
{
    if ((modifiers & WLR_MODIFIER_LOGO) == 0)
        return false;
    if (!pressed)
        return true;

    auto emit = [&](auto&& call) {
        for (auto& connection : g_connections) {
            if (connection->manager_window_id() >= 0)
                call(*connection, connection->manager_window_id());
        }
    };

    switch (keysym) {
    case XKB_KEY_Super_L:
    case XKB_KEY_Super_R:
        emit([](auto& connection, i32 wm_id) { connection.async_super_key_pressed(wm_id); });
        return true;
    case XKB_KEY_space:
        emit([](auto& connection, i32 wm_id) { connection.async_super_space_key_pressed(wm_id); });
        return true;
    case XKB_KEY_d:
    case XKB_KEY_D:
        emit([](auto& connection, i32 wm_id) { connection.async_super_d_key_pressed(wm_id); });
        return true;
    default:
        break;
    }

    if (keysym >= XKB_KEY_1 && keysym <= XKB_KEY_9) {
        auto digit = static_cast<u8>(keysym - XKB_KEY_1 + 1);
        emit([digit](auto& connection, i32 wm_id) { connection.async_super_digit_key_pressed(wm_id, digit); });
        return true;
    }

    return false;
}

void emit_window_state(Toplevel& toplevel)
{
    StringView title;
    if (toplevel.xdg_toplevel->title)
        title = StringView { toplevel.xdg_toplevel->title, strlen(toplevel.xdg_toplevel->title) };
    for (auto& connection : g_connections) {
        if (connection->manager_window_id() < 0)
            continue;
        // Chrome windows (the panel, applets, the desktop) are layer surfaces,
        // not toplevels, so nothing reported here is chrome.
        connection->async_window_state_changed(connection->manager_window_id(), 0, toplevel.wm_id, toplevel.workspace_row, toplevel.workspace_column,
            toplevel.frame_active && !toplevel.minimized, false, toplevel.minimized, false,
            static_cast<i32>(WindowServer::WindowType::Normal), ByteString { title },
            Gfx::IntRect { 0, 0, 0, 0 }, Optional<i32> { });
    }
}

void emit_window_removed(Toplevel& toplevel)
{
    for (auto& connection : g_connections) {
        if (connection->manager_window_id() >= 0)
            connection->async_window_removed(connection->manager_window_id(), 0, toplevel.wm_id);
    }
}

void tell_wms_applet_area_size_changed(Server&, Gfx::IntSize size)
{
    for (auto& connection : g_connections) {
        if (connection->manager_window_id() >= 0)
            connection->async_applet_area_size_changed(connection->manager_window_id(), size);
    }
}

}
