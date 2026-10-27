/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Compositor.h"
#include "Focus.h"
#include "InputMethod.h"
#include "Keyboard.h"
#include "LayerShell.h"
#include "Pointer.h"
#include "Resources.h"
#include "Trace.h"
#include "WindowManager.h"
#include "XWayland.h"
#include "XWaylandFrame.h"

#include <stdlib.h>
#include <string.h>

#include "Wlroots.h"

static void x11_place_frame(X11Toplevel& toplevel);
static void x11_update_class(X11Toplevel& toplevel);
static void x11_update_hints(X11Toplevel& toplevel);
static void x11_fetch_icon(X11Toplevel& toplevel);

// --- XWayland (X11 clients) ------------------------------------------------
// X11 clients draw their own decorations, so the compositor manages content,
// position, focus and stacking only. Enabled by [Compositor] XWayland.

X11Toplevel* x11_toplevel_for_wlr_surface(Server& server, wlr_surface* surface)
{
    auto* xsurface = wlr_xwayland_surface_try_from_wlr_surface(surface);
    if (!xsurface)
        return nullptr;

    // Some X11 clients (VirtualBox is one) render their content into child X11
    // windows.  The scene buffer under a click can therefore belong to a child
    // rather than to the managed top-level.  Walk the X11 parent chain so focus
    // and raise still target the top-level; otherwise the click leaves the
    // previously focused native window active, which then raises over the X11
    // client.  Override-redirect children (menus/tooltips) are not focusable,
    // so keep walking to their managed ancestor.
    for (auto* candidate = xsurface; candidate; candidate = candidate->parent) {
        for (auto& toplevel : server.x11_toplevels) {
            if (toplevel->xsurface != candidate)
                continue;
            if (!toplevel->override_redirect)
                return toplevel.ptr();
            break;
        }
    }
    return nullptr;
}

// The X11 (Xwayland) surface under a layout point, if any. Layer surfaces and
// xdg toplevels are handled elsewhere, so only X11 surfaces are considered.
bool x11_surface_at(Server& server, double lx, double ly, wlr_surface** surface, double* sx, double* sy)
{
    if (!server.xwayland.wlr)
        return false;
    auto* node = wlr_scene_node_at(&server.scene->tree.node, lx, ly, sx, sy);
    if (!node || node->type != WLR_SCENE_NODE_BUFFER)
        return false;
    auto* scene_surface = wlr_scene_surface_try_from_buffer(wlr_scene_buffer_from_node(node));
    if (!scene_surface || !wlr_xwayland_surface_try_from_wlr_surface(scene_surface->surface))
        return false;
    *surface = scene_surface->surface;
    // Input behind a blocking modal is blocked.
    if (auto* x11 = x11_toplevel_for_wlr_surface(server, *surface); x11 && WindowManager::is_x11_toplevel_blocked(server, *x11))
        return false;
    return true;
}

// Override-redirect X11 windows (menus, tooltips) are transient children; the X
// server's stacking is not mirrored here, so keep them above their parent when
// the parent is raised. docs/20-xwayland-next-steps.md item 4.
static void x11_raise_overrides_above(X11Toplevel& parent)
{
    for (auto& child : parent.server->x11_toplevels) {
        if (!child->mapped || !child->xsurface)
            continue;
        // Override-redirect menus/tooltips follow X's parent; managed transients
        // (dialogs) use the tracked transient_parent.
        bool const is_child = child->override_redirect
            ? (parent.xsurface && child->xsurface->parent == parent.xsurface)
            : (child->transient_parent == &parent);
        if (is_child)
            wlr_scene_node_raise_to_top(&child->scene_tree->node);
    }
}

// Keep _NET_WM_STATE_ABOVE windows on top after another window is raised.
static void x11_raise_above_windows(Server& server)
{
    for (auto& toplevel : server.x11_toplevels) {
        if (toplevel->mapped && toplevel->always_on_top)
            wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);
    }
}

void focus_x11_toplevel(Server& server, X11Toplevel* toplevel)
{
    if (!toplevel || !toplevel->xsurface || !toplevel->xsurface->surface)
        return;
    // Override-redirect windows (menus, tooltips, drag icons) never take focus.
    if (toplevel->override_redirect)
        return;
    wlr_surface* surface = toplevel->xsurface->surface;

    // Exactly one window is active (see deactivate_other_windows).
    deactivate_other_windows(server, nullptr, toplevel);

    if (server.seat->keyboard_state.focused_surface == surface && toplevel->frame_active)
        return;

    if (Trace::enabled())
        Trace::event("focus"sv, ByteString::formatted("x11 wm_id={} title='{}'", toplevel->wm_id, toplevel->title));

    server.xwayland.focused = toplevel;
    // Feed the Alt-Tab switcher's most-recently-focused ordering (see
    // Toplevel::focus_order; X11Toplevel gained the same field).
    toplevel->focus_order = ++g_focus_order_counter;

    server.keyboard_focus_layer = nullptr;
    stop_key_repeat();
    wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);
    // Keep this window's override-redirect children (menus/tooltips) above it,
    // and any always-on-top windows at the very top.
    x11_raise_overrides_above(*toplevel);
    x11_raise_above_windows(server);
    toplevel->frame_active = true;
    wlr_xwayland_surface_activate(toplevel->xsurface, true);
    if (toplevel->frame_decorated)
        x11_update_frame(*toplevel);
    // Focusing a window clears its demands-attention state.
    toplevel->demands_attention = false;
    if (toplevel->wm_id != 0)
        WindowManager::emit_window_state(*toplevel);
    auto* keyboard = wlr_seat_get_keyboard(server.seat);
    if (keyboard)
        wlr_seat_keyboard_notify_enter(server.seat, surface, keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
    // R5 IME: hand the new focus to text-input-v3 and the input method.
    InputMethod::set_focus(server, surface);
}

// The client surface (xdg x11 content or X11) under a layout point.
wlr_surface* window_surface_at(Server& server, double lx, double ly, double* sx, double* sy)
{
    wlr_surface* surface = nullptr;
    toplevel_at(server, lx, ly, &surface, sx, sy);
    if (!surface)
        x11_surface_at(server, lx, ly, &surface, sx, sy);
    return surface;
}

// Focus the window (xdg or X11) under a layout point.
void focus_window_at(Server& server, double lx, double ly)
{
    double sx = 0;
    double sy = 0;
    wlr_surface* surface = nullptr;
    if (auto* toplevel = toplevel_at(server, lx, ly, &surface, &sx, &sy)) {
        focus_toplevel(server, toplevel);
        return;
    }
    if (x11_surface_at(server, lx, ly, &surface, &sx, &sy)) {
        if (auto* x11 = x11_toplevel_for_wlr_surface(server, surface))
            focus_x11_toplevel(server, x11);
    }
}

static void x11_surface_map(wl_listener* listener, void*)
{
    X11Toplevel* toplevel = nullptr;
    toplevel = wl_container_of(listener, toplevel, surface_map);
    auto& server = *toplevel->server;
    toplevel->mapped = true;
    toplevel->title = ByteString { toplevel->xsurface->title ? StringView { toplevel->xsurface->title, strlen(toplevel->xsurface->title) } : StringView { } };
    // A newly mapped X11 window joins the current workspace stack (its scene node
    // is enabled only if that stack is the current one).
    WindowManager::on_x11_toplevel_mapped(*toplevel);
    wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);
    x11_update_frame(*toplevel);
    // Place it (frame at the requested position) once the frame size is known, so
    // the whole window — titlebar included — is on-screen.
    x11_place_frame(*toplevel);
    // WM_CLASS / _NET_WM_ICON / _NET_WM_STATE may have been set before the
    // listeners were attached.
    x11_update_class(*toplevel);
    x11_fetch_icon(*toplevel);
    x11_update_hints(*toplevel);
    dbgln("WindowServer: X11 window mapped '{}' at {},{}", toplevel->xsurface->title ? toplevel->xsurface->title : "", toplevel->xsurface->x, toplevel->xsurface->y);
    // Override-redirect windows (menus, tooltips) are unmanaged: no frame, no
    // focus, no Taskbar entry.
    if (!toplevel->override_redirect) {
        // _NET_WM_STATE_SKIP_TASKBAR: a normal window, but no Taskbar entry.
        if (!toplevel->skip_taskbar && toplevel->wm_id == 0)
            toplevel->wm_id = g_next_wm_id++;
        focus_x11_toplevel(server, toplevel);
        if (toplevel->always_on_top)
            x11_raise_above_windows(server);
        if (toplevel->wm_id != 0)
            WindowManager::emit_window_state(*toplevel);
    } else {
        // They are transient and never workspace-hid (docs/20 item 4).
        dbgln("WindowServer: X11 override-redirect window mapped '{}' at {},{}", toplevel->title, toplevel->xsurface->x, toplevel->xsurface->y);
    }
}

static void x11_surface_unmap(wl_listener* listener, void*)
{
    X11Toplevel* toplevel = nullptr;
    toplevel = wl_container_of(listener, toplevel, surface_unmap);
    toplevel->mapped = false;
    wlr_scene_node_set_enabled(&toplevel->scene_tree->node, false);
    if (toplevel->wm_id != 0)
        WindowManager::emit_window_state(*toplevel);
}

static void x11_scene_destroy(wl_listener* listener, void*)
{
    X11Toplevel* toplevel = nullptr;
    toplevel = wl_container_of(listener, toplevel, scene_destroy);
    remove_listener(toplevel->scene_destroy);
    toplevel->scene = nullptr;
}

static void x11_surface_associate(wl_listener* listener, void*)
{
    X11Toplevel* toplevel = nullptr;
    toplevel = wl_container_of(listener, toplevel, associate);
    auto* xsurface = toplevel->xsurface;
    if (!xsurface->surface)
        return;
    // Re-associating would add the same listeners twice (corrupting the list), so
    // drop any previous ones first.
    if (toplevel->surface_listeners_attached) {
        wl_list_remove(&toplevel->surface_map.link);
        wl_list_remove(&toplevel->surface_unmap.link);
        toplevel->surface_listeners_attached = false;
    }
    toplevel->scene = wlr_scene_surface_create(toplevel->scene_tree, xsurface->surface);
    remove_listener(toplevel->scene_destroy);
    if (toplevel->scene) {
        toplevel->scene_destroy.notify = x11_scene_destroy;
        wl_signal_add(&toplevel->scene->buffer->node.events.destroy, &toplevel->scene_destroy);
    }
    toplevel->surface_map.notify = x11_surface_map;
    wl_signal_add(&xsurface->surface->events.map, &toplevel->surface_map);
    toplevel->surface_unmap.notify = x11_surface_unmap;
    wl_signal_add(&xsurface->surface->events.unmap, &toplevel->surface_unmap);
    toplevel->listened_surface = xsurface->surface;
    toplevel->surface_listeners_attached = true;
}

static void x11_surface_dissociate(wl_listener* listener, void*)
{
    X11Toplevel* toplevel = nullptr;
    toplevel = wl_container_of(listener, toplevel, dissociate);
    toplevel->mapped = false;
    // The surface is going away: detach our map/unmap listeners from it.
    if (toplevel->surface_listeners_attached) {
        wl_list_remove(&toplevel->surface_map.link);
        wl_list_remove(&toplevel->surface_unmap.link);
        toplevel->surface_listeners_attached = false;
        toplevel->listened_surface = nullptr;
    }
    // When the wl_surface itself is being destroyed first (Xwayland's Wayland
    // connection closing while its window manager is still up, as at shutdown),
    // wlroots has already destroyed the scene surface and x11_scene_destroy()
    // cleared `scene`; destroying it again here was a use-after-free.
    if (toplevel->scene)
        wlr_scene_node_destroy(&toplevel->scene->buffer->node);
}

static void x11_surface_destroy(wl_listener* listener, void*)
{
    X11Toplevel* toplevel = nullptr;
    toplevel = wl_container_of(listener, toplevel, destroy);
    auto& server = *toplevel->server;
    // Detach every listener we added to the Xwayland surface: wlroots' destroy
    // asserts that all of these event listener lists are empty, so leaving any
    // attached aborts the whole compositor (e.g. when an xterm exits).
    if (toplevel->surface_listeners_attached) {
        wl_list_remove(&toplevel->surface_map.link);
        wl_list_remove(&toplevel->surface_unmap.link);
        toplevel->surface_listeners_attached = false;
        toplevel->listened_surface = nullptr;
    }
    wl_list_remove(&toplevel->associate.link);
    wl_list_remove(&toplevel->dissociate.link);
    wl_list_remove(&toplevel->request_configure.link);
    wl_list_remove(&toplevel->request_close.link);
    wl_list_remove(&toplevel->request_activate.link);
    wl_list_remove(&toplevel->set_title.link);
    wl_list_remove(&toplevel->set_class.link);
    wl_list_remove(&toplevel->set_icon.link);
    wl_list_remove(&toplevel->set_hints.link);
    wl_list_remove(&toplevel->request_skip_taskbar.link);
    wl_list_remove(&toplevel->request_demands_attention.link);
    wl_list_remove(&toplevel->destroy.link);
    if (server.xwayland.focused == toplevel)
        server.xwayland.focused = nullptr;
    forget_grab_window(server, *toplevel);
    forget_frame_press(server, *toplevel);
    if (toplevel->wm_id != 0)
        WindowManager::emit_window_removed(*toplevel);
    toplevel->frame_buffers = {}; // destroyed with the scene tree
    if (toplevel->scene_tree) {
        wlr_scene_node_destroy(&toplevel->scene_tree->node);
        toplevel->scene_tree = nullptr;
    }
    // Avoid dangling transient-parent pointers (and our surface back-reference).
    for (auto& other : server.x11_toplevels) {
        if (other->transient_parent == toplevel)
            other->transient_parent = nullptr;
    }
    if (toplevel->xsurface)
        toplevel->xsurface->data = nullptr;
    remove_owned(server.x11_toplevels, toplevel);
}

// Place the window's frame origin at the client's requested position, keeping the
// whole window — compositor frame included — on the primary output. The scene
// tree is the frame origin (as for xdg toplevels): the frame is drawn at (0,0)
// and the client content is inset by the frame inset.
static void x11_place_frame(X11Toplevel& toplevel)
{
    if (!toplevel.scene_tree || !toplevel.xsurface)
        return;
    auto& server = *toplevel.server;
    int x = toplevel.xsurface->x;
    int y = toplevel.xsurface->y;
    int const w = toplevel.frame_size.width() > 0 ? toplevel.frame_size.width() : toplevel.xsurface->width;
    int const h = toplevel.frame_size.height() > 0 ? toplevel.frame_size.height() : toplevel.xsurface->height;
    wlr_box workarea { };
    wlr_output_layout_get_box(server.output_layout, server.primary_output, &workarea);
    if (!wlr_box_empty(&workarea)) {
        x = max(x, workarea.x);
        y = max(y, workarea.y);
        if (w <= workarea.width)
            x = min(x, workarea.x + workarea.width - w);
        if (h <= workarea.height)
            y = min(y, workarea.y + workarea.height - h);
    }
    wlr_scene_node_set_position(&toplevel.scene_tree->node, x, y);
}

static void x11_request_configure(wl_listener* listener, void* data)
{
    X11Toplevel* toplevel = nullptr;
    toplevel = wl_container_of(listener, toplevel, request_configure);
    auto* event = static_cast<wlr_xwayland_surface_configure_event*>(data);
    wlr_xwayland_surface_configure(toplevel->xsurface, event->x, event->y, event->width, event->height);
    x11_update_frame(*toplevel);
    if (toplevel->mapped)
        x11_place_frame(*toplevel);
}

static void x11_request_close(wl_listener* listener, void*)
{
    X11Toplevel* toplevel = nullptr;
    toplevel = wl_container_of(listener, toplevel, request_close);
    wlr_xwayland_surface_close(toplevel->xsurface);
}

static void x11_request_activate(wl_listener* listener, void*)
{
    X11Toplevel* toplevel = nullptr;
    toplevel = wl_container_of(listener, toplevel, request_activate);
    if (!toplevel->override_redirect)
        focus_x11_toplevel(*toplevel->server, toplevel);
}

static void x11_set_title(wl_listener* listener, void*)
{
    X11Toplevel* toplevel = nullptr;
    toplevel = wl_container_of(listener, toplevel, set_title);
    dbgln("WindowServer: X11 window '{}'", toplevel->xsurface->title ? toplevel->xsurface->title : "");
    toplevel->title = ByteString { toplevel->xsurface->title ? StringView { toplevel->xsurface->title, strlen(toplevel->xsurface->title) } : StringView { } };
    if (toplevel->frame_decorated)
        x11_update_frame(*toplevel);
    if (toplevel->wm_id != 0)
        WindowManager::emit_window_state(*toplevel);
}

// WM_CLASS (res_name/res_class): the app identity, used for grouping. Kept on the
// toplevel so the WM layer needs no wlroots/xcb types.
static void x11_update_class(X11Toplevel& toplevel)
{
    auto* xsurface = toplevel.xsurface;
    char const* instance = xsurface->instance;
    char const* cls = xwayland_surface_class(xsurface);
    toplevel.wm_instance = ByteString { instance ? StringView { instance, strlen(instance) } : StringView { } };
    toplevel.wm_class = ByteString { cls ? StringView { cls, strlen(cls) } : StringView { } };
    dbgln("WindowServer: X11 window {} class='{}' instance='{}'", toplevel.wm_id, toplevel.wm_class, toplevel.wm_instance);
}

static void x11_set_class(wl_listener* listener, void*)
{
    X11Toplevel* toplevel = nullptr;
    toplevel = wl_container_of(listener, toplevel, set_class);
    auto const previous = ByteString::formatted("{}:{}", toplevel->wm_class, toplevel->wm_instance);
    x11_update_class(*toplevel);
    if (previous == ByteString::formatted("{}:{}", toplevel->wm_class, toplevel->wm_instance))
        return;
    // WM_CLASS arrives asynchronously, exactly like the title, and the Taskbar
    // groups X11 windows by it (WindowManager::emit_window_state passes it as the
    // app id). Without re-emitting, the button keeps the empty app id the window
    // was announced with — the compositor knows the class and the Taskbar never
    // does, so X11 windows never group. Found by run-taskbar-grouping-test.sh.
    if (toplevel->wm_id != 0)
        WindowManager::emit_window_state(*toplevel);
}

// _NET_WM_STATE hints and transiency (docs/20 item 7). wlroots parses the
// property into the surface fields; mirror the ones the WM acts on.
static void x11_update_hints(X11Toplevel& toplevel)
{
    auto* xsurface = toplevel.xsurface;
    if (!xsurface)
        return;
    toplevel.skip_taskbar = xsurface->skip_taskbar;
    toplevel.always_on_top = xsurface->above;
    toplevel.demands_attention = xsurface->demands_attention;
    toplevel.modal = xsurface->modal;
    toplevel.transient_parent = nullptr;
    if (xsurface->parent && xsurface->parent->data)
        toplevel.transient_parent = static_cast<X11Toplevel*>(xsurface->parent->data);
    dbgln("WindowServer: X11 window {} hints: skip_taskbar={} above={} demands_attention={} modal={} transient={}",
        toplevel.wm_id, toplevel.skip_taskbar, toplevel.always_on_top, toplevel.demands_attention, toplevel.modal,
        toplevel.transient_parent ? toplevel.transient_parent->wm_id : 0);
}

static void x11_request_skip_taskbar(wl_listener* listener, void*)
{
    X11Toplevel* toplevel = nullptr;
    toplevel = wl_container_of(listener, toplevel, request_skip_taskbar);
    x11_update_hints(*toplevel);
    // A window that now skips the Taskbar leaves the window list.
    if (toplevel->skip_taskbar && toplevel->wm_id != 0)
        WindowManager::emit_window_removed(*toplevel);
}

static void x11_request_demands_attention(wl_listener* listener, void*)
{
    X11Toplevel* toplevel = nullptr;
    toplevel = wl_container_of(listener, toplevel, request_demands_attention);
    x11_update_hints(*toplevel);
    if (toplevel->wm_id != 0)
        WindowManager::emit_window_state(*toplevel);
}

// _NET_WM_ICON: wlroots exposes the property (wlr_xwayland_surface_fetch_icon);
// pick a frame and wrap it in a Gfx::Bitmap so the Taskbar button and the Alt-Tab
// switcher can draw it. _NET_WM_ICON pixels are 0xAARRGGBB, i.e. B,G,R,A in
// memory (little-endian), which is exactly BitmapFormat::BGRA8888.

// Older clients (e.g. xterm) set their window icon via the legacy WM_HINTS
// icon_pixmap (+ optional 1-bit mask), not _NET_WM_ICON. Read it over the xwm
// connection (wlr_xwayland_get_xwm_connection) and build a Gfx::Bitmap.
static void x11_fetch_wm_hints_icon(X11Toplevel& toplevel)
{
    auto* xsurface = toplevel.xsurface;
    if (!xsurface || !xsurface->hints)
        return;
    if (!(xsurface->hints->flags & XCB_ICCCM_WM_HINT_ICON_PIXMAP))
        return;
    xcb_pixmap_t const pixmap = xsurface->hints->icon_pixmap;
    if (pixmap == XCB_NONE)
        return;
    xcb_pixmap_t const mask = (xsurface->hints->flags & XCB_ICCCM_WM_HINT_ICON_MASK) ? xsurface->hints->icon_mask : XCB_NONE;

    auto* connection = (toplevel.server && toplevel.server->xwayland.wlr) ? wlr_xwayland_get_xwm_connection(toplevel.server->xwayland.wlr) : nullptr;
    if (!connection)
        return;

    xcb_get_geometry_reply_t* geometry = xcb_get_geometry_reply(connection, xcb_get_geometry(connection, pixmap), nullptr);
    if (!geometry)
        return;
    int const width = geometry->width;
    int const height = geometry->height;
    uint8_t const depth = geometry->depth;
    free(geometry);
    if (width <= 0 || height <= 0 || width > 256 || height > 256)
        return;

    xcb_get_image_reply_t* image = xcb_get_image_reply(connection, xcb_get_image(connection, XCB_IMAGE_FORMAT_Z_PIXMAP, pixmap, 0, 0, static_cast<uint16_t>(width), static_cast<uint16_t>(height), ~0u), nullptr);
    if (!image)
        return;
    uint8_t const* data = xcb_get_image_data(image);

    xcb_get_image_reply_t* mask_image = nullptr;
    uint8_t const* mask_data = nullptr;
    if (mask != XCB_NONE) {
        mask_image = xcb_get_image_reply(connection, xcb_get_image(connection, XCB_IMAGE_FORMAT_Z_PIXMAP, mask, 0, 0, static_cast<uint16_t>(width), static_cast<uint16_t>(height), ~0u), nullptr);
        if (mask_image)
            mask_data = xcb_get_image_data(mask_image);
    }

    if (auto bitmap = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, { width, height }); !bitmap.is_error()) {
        auto& created = *bitmap.value();
        int const one_bit_stride = ((width + 31) / 32) * 4; // 1-bit rows pad to 32 bits
        for (int y = 0; y < height; ++y) {
            auto* dst = created.scanline_u8(y);
            for (int x = 0; x < width; ++x) {
                uint8_t r = 0;
                uint8_t g = 0;
                uint8_t b = 0;
                if (depth == 1) {
                    uint8_t const bit = (data[static_cast<size_t>(y) * one_bit_stride + x / 8] >> (x % 8)) & 1;
                    r = g = b = bit ? 0x10 : 0xf0; // foreground / background of the bitmap
                } else if (depth == 24 || depth == 32) {
                    uint8_t const* p = data + (static_cast<size_t>(y) * width + x) * 4; // 32 bits/pixel
                    b = p[0];
                    g = p[1];
                    r = p[2];
                } else {
                    continue;
                }
                uint8_t alpha = 255;
                if (mask_data) {
                    uint8_t const m = (mask_data[static_cast<size_t>(y) * one_bit_stride + x / 8] >> (x % 8)) & 1;
                    alpha = m ? 255 : 0;
                }
                dst[x * 4 + 0] = b;
                dst[x * 4 + 1] = g;
                dst[x * 4 + 2] = r;
                dst[x * 4 + 3] = alpha;
            }
        }
        toplevel.icon = bitmap.release_value();
        dbgln("WindowServer: X11 window '{}' WM_HINTS icon {}x{} (wm_id={})", toplevel.title, width, height, toplevel.wm_id);
    }
    free(image);
    if (mask_image)
        free(mask_image);
}

static void x11_fetch_icon(X11Toplevel& toplevel)
{
    if (!toplevel.xsurface)
        return;

    bool have_icon = false;
    xcb_ewmh_get_wm_icon_reply_t reply;
    if (wlr_xwayland_surface_fetch_icon(toplevel.xsurface, &reply)) {
        unsigned best_width = 0;
        unsigned best_height = 0;
        uint32_t const* best_pixels = nullptr;
        int best_rank = -1;
        auto it = xcb_ewmh_get_wm_icon_iterator(&reply);
        while (it.rem > 0) {
            unsigned const w = it.width;
            unsigned const h = it.height;
            int rank;
            if (w == 16 && h == 16)
                rank = 1 << 20; // the Taskbar/switcher size
            else if (w <= 64 && h <= 64)
                rank = static_cast<int>(w * h);
            else
                rank = 0;
            if (it.data && rank > best_rank) {
                best_rank = rank;
                best_width = w;
                best_height = h;
                best_pixels = it.data;
            }
            xcb_ewmh_get_wm_icon_next(&it);
        }

        if (best_pixels && best_width > 0 && best_height > 0) {
            if (auto bitmap = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, { static_cast<int>(best_width), static_cast<int>(best_height) }); !bitmap.is_error()) {
                auto& created = *bitmap.value();
                for (unsigned y = 0; y < best_height; ++y)
                    memcpy(created.scanline(static_cast<int>(y)), best_pixels + static_cast<size_t>(y) * best_width, static_cast<size_t>(best_width) * 4);
                toplevel.icon = bitmap.release_value();
                have_icon = true;
                dbgln("WindowServer: X11 window '{}' icon {}x{} (wm_id={})", toplevel.title, best_width, best_height, toplevel.wm_id);
            }
        }
        xcb_ewmh_get_wm_icon_reply_wipe(&reply);
    }

    // Fall back to the legacy WM_HINTS icon (xterm and friends).
    if (!have_icon && toplevel.server && toplevel.xsurface && toplevel.xsurface->hints
        && (toplevel.xsurface->hints->flags & XCB_ICCCM_WM_HINT_ICON_PIXMAP)
        && toplevel.xsurface->hints->icon_pixmap != XCB_NONE)
        x11_fetch_wm_hints_icon(toplevel);

    if (toplevel.icon) {
        WindowManager::emit_window_icon(toplevel);
        // Repaint the titlebar so it shows the icon.
        if (toplevel.frame_decorated)
            x11_update_frame(toplevel);
    }
}

static void x11_set_icon(wl_listener* listener, void*)
{
    X11Toplevel* toplevel = nullptr;
    toplevel = wl_container_of(listener, toplevel, set_icon);
    x11_fetch_icon(*toplevel);
}

// WM_HINTS changed (e.g. the legacy icon pixmap): (re)read the icon.
static void x11_set_hints(wl_listener* listener, void*)
{
    X11Toplevel* toplevel = nullptr;
    toplevel = wl_container_of(listener, toplevel, set_hints);
    x11_fetch_icon(*toplevel);
}

static void server_new_xwayland_surface(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, xwayland.new_surface);
    auto* xsurface = static_cast<wlr_xwayland_surface*>(data);

    auto toplevel = make<X11Toplevel>();
    toplevel->server = server;
    toplevel->xsurface = xsurface;
    toplevel->override_redirect = xsurface->override_redirect;
    // The default icon (a big "X") until the client supplies its own: it shows on the
    // titlebar, the Alt-Tab switcher and the Taskbar button.
    toplevel->icon = g_x11_default_icon;
    toplevel->scene_tree = wlr_scene_tree_create(server->windows_tree);
    wlr_scene_node_set_enabled(&toplevel->scene_tree->node, false);
    xsurface->data = toplevel.ptr();

    toplevel->associate.notify = x11_surface_associate;
    wl_signal_add(&xsurface->events.associate, &toplevel->associate);
    toplevel->dissociate.notify = x11_surface_dissociate;
    wl_signal_add(&xsurface->events.dissociate, &toplevel->dissociate);
    toplevel->destroy.notify = x11_surface_destroy;
    wl_signal_add(&xsurface->events.destroy, &toplevel->destroy);
    toplevel->request_configure.notify = x11_request_configure;
    wl_signal_add(&xsurface->events.request_configure, &toplevel->request_configure);
    toplevel->request_close.notify = x11_request_close;
    wl_signal_add(&xsurface->events.request_close, &toplevel->request_close);
    toplevel->request_activate.notify = x11_request_activate;
    wl_signal_add(&xsurface->events.request_activate, &toplevel->request_activate);
    toplevel->set_title.notify = x11_set_title;
    wl_signal_add(&xsurface->events.set_title, &toplevel->set_title);
    toplevel->set_class.notify = x11_set_class;
    wl_signal_add(&xsurface->events.set_class, &toplevel->set_class);
    toplevel->set_icon.notify = x11_set_icon;
    wl_signal_add(&xsurface->events.set_icon, &toplevel->set_icon);
    toplevel->set_hints.notify = x11_set_hints;
    wl_signal_add(&xsurface->events.set_hints, &toplevel->set_hints);
    toplevel->request_skip_taskbar.notify = x11_request_skip_taskbar;
    wl_signal_add(&xsurface->events.request_skip_taskbar, &toplevel->request_skip_taskbar);
    toplevel->request_demands_attention.notify = x11_request_demands_attention;
    wl_signal_add(&xsurface->events.request_demands_attention, &toplevel->request_demands_attention);

    // WM_CLASS may already be set by the time we associate.
    x11_update_class(*toplevel);

    server->x11_toplevels.append(move(toplevel));
}

static void xwayland_ready(wl_listener* listener, void*)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, xwayland.ready);
    server->xwayland.started = true;
    // Now that xwm exists, report the workarea (panels' exclusive zones).
    update_xwayland_workareas(*server);
    dbgln("WindowServer: Xwayland ready (DISPLAY={})", server->xwayland.wlr->display_name);
}

// Tell Xwayland the usable area(s) (_NET_WORKAREA), so X11 clients (panels,
// maximized windows, fullscreen) respect SDE's panels/docks. docs/20 item 8.
void update_xwayland_workareas(Server& server)
{
    if (!server.xwayland.wlr || !server.xwayland.started)
        return;
    // `xwayland_started` only records that the X server became ready once — it may
    // have died since. `wlr_xwayland_set_workareas()` dereferences the XWM
    // connection, which wlroots drops when the server goes away
    // (xwm_destroy → xwayland->xwm = nullptr), so asking for it here is a
    // nullptr dereference and a SIGSEGV in the compositor's event loop. This is
    // not hypothetical: a test run that disturbed the session's Xwayland took the
    // whole session down from this line, via arrange_layers → a layer-surface
    // commit. Report only to a live X server.
    if (!wlr_xwayland_get_xwm_connection(server.xwayland.wlr))
        return;
    Vector<wlr_box> areas;
    for (auto& output : server.outputs) {
        wlr_box box = output->usable_area;
        if (wlr_box_empty(&box))
            wlr_output_layout_get_box(server.output_layout, output->output, &box);
        areas.append(box);
    }
    if (areas.is_empty())
        return;
    wlr_xwayland_set_workareas(server.xwayland.wlr, areas.data(), areas.size());
    dbgln("WindowServer: reported X11 workarea ({} area(s); first {}x{} at {},{}).",
        areas.size(), areas[0].width, areas[0].height, areas[0].x, areas[0].y);
}

void initialize_xwayland(Server& server)
{
    // XWayland (R5): X11 client support. [Compositor] XWayland=false disables it
    // entirely (no server, no DISPLAY). Lazy: the X server starts on first use.
    if (Config::compositor().xwayland && server.compositor) {
        server.xwayland.wlr = wlr_xwayland_create(server.display, server.compositor, true);
        if (server.xwayland.wlr) {
            wlr_xwayland_set_seat(server.xwayland.wlr, server.seat);
            server.xwayland.new_surface.notify = server_new_xwayland_surface;
            wl_signal_add(&server.xwayland.wlr->events.new_surface, &server.xwayland.new_surface);
            server.xwayland.ready.notify = xwayland_ready;
            wl_signal_add(&server.xwayland.wlr->events.ready, &server.xwayland.ready);
            // Export DISPLAY for the session's apps (state file + our own env).
            setenv("DISPLAY", server.xwayland.wlr->display_name, 1);
            StringView display { server.xwayland.wlr->display_name, strlen(server.xwayland.wlr->display_name) };
            Config::write_state("xwayland-display"sv, display.bytes());
            dbgln("WindowServer: Xwayland enabled (DISPLAY={})", server.xwayland.wlr->display_name);
        } else {
            warnln("WindowServer: could not create Xwayland");
        }
    }
}

void finish_xwayland(Server& server)
{
    remove_listener(server.xwayland.new_surface);
    remove_listener(server.xwayland.ready);
    // The X server and its windows go first, while the scene they draw into and
    // the seat they take focus from still exist.
    if (server.xwayland.wlr) {
        wlr_xwayland_destroy(server.xwayland.wlr);
        server.xwayland.wlr = nullptr;
    }
}
