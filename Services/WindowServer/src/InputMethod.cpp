/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "InputMethod.h"

#include <AK/Format.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/StdLibExtras.h>
#include <AK/Vector.h>

#include <LibGfx/Rect.h>

#include "Server.h"
#include "wlr_input_method_v2_cxx.h"

extern "C" {
#include <wayland-server-core.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_text_input_v3.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/box.h>
// wlroots' xwayland.h declares a C field named `class`, a C++ keyword; rename it
// for the duration of the include (we never read that field).
#define class class_
#include <wlr/xwayland/xwayland.h>
#undef class
}

// The keycodes whose press was forwarded to the input method's keyboard grab, so
// a release is only forwarded when its press was (a compositor keybinding that
// consumed the press must not leak a stray release to the input method). The
// compositor serves a single seat, so one process-wide set is enough.
static Vector<uint32_t> s_grab_pressed_keys;

static void remove_owned_ptr(Vector<NonnullOwnPtr<TextInput>>& list, TextInput* object)
{
    for (size_t i = 0; i < list.size(); ++i) {
        if (list[i].ptr() == object) {
            list.remove(i);
            return;
        }
    }
}

static void remove_owned_ptr(Vector<NonnullOwnPtr<InputPopup>>& list, InputPopup* object)
{
    for (size_t i = 0; i < list.size(); ++i) {
        if (list[i].ptr() == object) {
            list.remove(i);
            return;
        }
    }
}

static void remove_owned_ptr(Vector<NonnullOwnPtr<InputMethodClient>>& list, InputMethodClient* object)
{
    for (size_t i = 0; i < list.size(); ++i) {
        if (list[i].ptr() == object) {
            list.remove(i);
            return;
        }
    }
}

static void set_pending_focused_surface(TextInput& text_input, wlr_surface* surface);

static wlr_input_method_keyboard_grab_v2* active_keyboard_grab(Server& server)
{
    if (!server.ime.input_method)
        return nullptr;
    return server.ime.input_method->keyboard_grab;
}

// The text input whose field is currently focused (wlroots set focused_surface
// when we sent `enter`).
static TextInput* focused_text_input(Server& server)
{
    for (auto& text_input : server.ime.text_inputs) {
        if (text_input->input->focused_surface)
            return text_input.ptr();
    }
    return nullptr;
}

// The text input waiting for an input method to become available (focus is on a
// field, but there was no input method to send `enter` to yet).
static TextInput* focusable_text_input(Server& server)
{
    for (auto& text_input : server.ime.text_inputs) {
        if (text_input->pending_focused_surface)
            return text_input.ptr();
    }
    return nullptr;
}

// --- input-method popups (the candidate/preedit window) ---------------------

// The scene node holding a focused surface's content, so a popup can be placed
// relative to the text cursor. Null for a surface the shell does not manage.
static wlr_scene_node* surface_scene_node(Server& server, wlr_surface* surface)
{
    for (auto& toplevel : server.toplevels) {
        if (toplevel->xdg_toplevel && toplevel->xdg_toplevel->base->surface == surface && toplevel->content_tree)
            return &toplevel->content_tree->node;
    }
    for (auto& toplevel : server.x11_toplevels) {
        if (toplevel->xsurface && toplevel->xsurface->surface == surface && toplevel->scene)
            return &toplevel->scene->buffer->node;
    }
    for (auto& layer : server.layer_surfaces) {
        if (layer->layer_surface && layer->layer_surface->surface == surface && layer->scene)
            return &layer->scene->tree->node;
    }
    return nullptr;
}

static void position_popup(Server& server, InputPopup& popup, TextInput& text_input)
{
    auto* surface = text_input.input->focused_surface;
    if (!surface || !popup.scene_tree || !popup.popup_surface->surface)
        return;
    auto* node = surface_scene_node(server, surface);
    int lx = 0;
    int ly = 0;
    if (!node || !wlr_scene_node_coords(node, &lx, &ly))
        return;

    // The text cursor, in surface-local coordinates (the client reports it).
    Gfx::IntRect cursor;
    if (text_input.input->active_features & WLR_TEXT_INPUT_V3_FEATURE_CURSOR_RECTANGLE) {
        auto const& rect = text_input.input->current.cursor_rectangle;
        cursor = { rect.x, rect.y, rect.width, rect.height };
    }

    int const popup_width = popup.popup_surface->surface->current.width;
    int const popup_height = popup.popup_surface->surface->current.height;
    int x = lx + cursor.x();
    int y = ly + cursor.y() + cursor.height();

    // Keep the popup on the output: shift it left and/or above the cursor when it
    // would run off the edge.
    wlr_box output_box { };
    wlr_output_layout_get_box(server.output_layout, server.primary_output, &output_box);
    if (!wlr_box_empty(&output_box)) {
        if (x + popup_width > output_box.x + output_box.width)
            x = output_box.x + output_box.width - popup_width;
        if (x < output_box.x)
            x = output_box.x;
        if (y + popup_height > output_box.y + output_box.height)
            y = ly + cursor.y() - popup_height;
        if (y < output_box.y)
            y = output_box.y;
    }
    wlr_scene_node_set_position(&popup.scene_tree->node, x, y);

    // Tell the input method where the text cursor is, relative to the popup, so
    // it can draw its candidate list near the caret.
    wlr_box rect {
        .x = lx + cursor.x() - x,
        .y = ly + cursor.y() - y,
        .width = cursor.width(),
        .height = cursor.height(),
    };
    wlr_input_popup_surface_v2_send_text_input_rectangle(popup.popup_surface, &rect);
}

static void popup_focused_surface_unmap(wl_listener* listener, void*);

static void set_popup_focus(Server& server, InputPopup& popup, wlr_surface* surface)
{
    if (popup.focused_surface == surface && popup.scene_tree && popup.scene_tree->node.enabled) {
        // Already attached: just refresh the position (the caret may have moved).
        if (auto* text_input = focused_text_input(server))
            position_popup(server, popup, *text_input);
        return;
    }
    // Detach the old surface's unmap listener (initialised so removal is safe
    // whether or not it was attached).
    wl_list_remove(&popup.focused_surface_unmap.link);
    wl_list_init(&popup.focused_surface_unmap.link);
    popup.focused_surface = surface;
    if (!popup.scene_tree)
        return;
    if (!surface) {
        wlr_scene_node_set_enabled(&popup.scene_tree->node, false);
        return;
    }
    wl_signal_add(&surface->events.unmap, &popup.focused_surface_unmap);
    if (auto* text_input = focused_text_input(server))
        position_popup(server, popup, *text_input);
    wlr_scene_node_set_enabled(&popup.scene_tree->node, true);
}

static void popup_focused_surface_unmap(wl_listener* listener, void*)
{
    InputPopup* popup = nullptr;
    popup = wl_container_of(listener, popup, focused_surface_unmap);
    set_popup_focus(*popup->server, *popup, nullptr);
}

static void popup_destroy(wl_listener* listener, void*)
{
    InputPopup* popup = nullptr;
    popup = wl_container_of(listener, popup, destroy);
    auto& server = *popup->server;
    wl_list_remove(&popup->surface_commit.link);
    wl_list_remove(&popup->surface_map.link);
    wl_list_remove(&popup->surface_unmap.link);
    wl_list_remove(&popup->focused_surface_unmap.link);
    if (popup->scene_tree) {
        wlr_scene_node_destroy(&popup->scene_tree->node);
        popup->scene_tree = nullptr;
    }
    wl_list_remove(&popup->destroy.link);
    remove_owned_ptr(server.ime.input_popups, popup);
}

static void popup_surface_map(wl_listener* listener, void*)
{
    InputPopup* popup = nullptr;
    popup = wl_container_of(listener, popup, surface_map);
    auto& server = *popup->server;
    if (auto* text_input = focused_text_input(server))
        set_popup_focus(server, *popup, text_input->input->focused_surface);
    else
        set_popup_focus(server, *popup, nullptr);
}

static void popup_surface_unmap(wl_listener* listener, void*)
{
    InputPopup* popup = nullptr;
    popup = wl_container_of(listener, popup, surface_unmap);
    set_popup_focus(*popup->server, *popup, nullptr);
}

static void popup_surface_commit(wl_listener* listener, void*)
{
    InputPopup* popup = nullptr;
    popup = wl_container_of(listener, popup, surface_commit);
    auto& server = *popup->server;
    // The candidate list grew/shrank: re-constrain it to the output.
    if (auto* text_input = focused_text_input(server))
        position_popup(server, *popup, *text_input);
}

// --- input method (zwp_input_method_v2) -------------------------------------

static void new_popup_surface(wl_listener* listener, void* data)
{
    InputMethodClient* client = nullptr;
    client = wl_container_of(listener, client, new_popup_surface);
    auto& server = *client->server;
    auto* popup_surface = static_cast<wlr_input_popup_surface_v2*>(data);

    auto popup = make<InputPopup>();
    popup->server = &server;
    popup->popup_surface = popup_surface;
    popup->scene_tree = wlr_scene_tree_create(server.ime.popup_tree);
    if (!popup->scene_tree)
        return;
    if (!wlr_scene_subsurface_tree_create(popup->scene_tree, popup_surface->surface)) {
        wlr_scene_node_destroy(&popup->scene_tree->node);
        return;
    }
    wlr_scene_node_set_enabled(&popup->scene_tree->node, false);
    popup->destroy.notify = popup_destroy;
    wl_signal_add(&popup_surface->events.destroy, &popup->destroy);
    popup->surface_commit.notify = popup_surface_commit;
    wl_signal_add(&popup_surface->surface->events.commit, &popup->surface_commit);
    popup->surface_map.notify = popup_surface_map;
    wl_signal_add(&popup_surface->surface->events.map, &popup->surface_map);
    popup->surface_unmap.notify = popup_surface_unmap;
    wl_signal_add(&popup_surface->surface->events.unmap, &popup->surface_unmap);
    popup->focused_surface_unmap.notify = popup_focused_surface_unmap;
    wl_list_init(&popup->focused_surface_unmap.link);
    server.ime.input_popups.append(move(popup));
    dbgln("WindowServer: IME: input method popup surface created");
}

// Apply the input method's committed state to the focused text input.
static void input_method_commit(wl_listener* listener, void*)
{
    InputMethodClient* client = nullptr;
    client = wl_container_of(listener, client, commit);
    auto& server = *client->server;
    auto* input_method = client->input_method;
    auto* text_input = focused_text_input(server);
    if (!text_input)
        return;
    if (input_method->current.preedit.text) {
        wlr_text_input_v3_send_preedit_string(text_input->input, input_method->current.preedit.text,
            input_method->current.preedit.cursor_begin, input_method->current.preedit.cursor_end);
    }
    if (input_method->current.commit_text) {
        wlr_text_input_v3_send_commit_string(text_input->input, input_method->current.commit_text);
        dbgln("WindowServer: IME: committed \"{}\"", input_method->current.commit_text);
    }
    if (input_method->current.delete_surrounding.before_length || input_method->current.delete_surrounding.after_length) {
        wlr_text_input_v3_send_delete_surrounding_text(text_input->input,
            input_method->current.delete_surrounding.before_length, input_method->current.delete_surrounding.after_length);
    }
    wlr_text_input_v3_send_done(text_input->input);
}

// wlroots emits the grab's destroy signal with a null argument, so recover the
// grab from the input method (its keyboard_grab is cleared only after the emit).
static void input_method_grab_destroy(wl_listener* listener, void*)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, ime.grab_destroy);
    wl_list_remove(&server->ime.grab_destroy.link);
    wl_list_init(&server->ime.grab_destroy.link);
    s_grab_pressed_keys.clear();
    auto* keyboard_grab = server->ime.input_method ? server->ime.input_method->keyboard_grab : nullptr;
    // Hand the keyboard back to the focused client, with the current modifiers.
    if (keyboard_grab && keyboard_grab->keyboard) {
        wlr_seat_set_keyboard(server->seat, keyboard_grab->keyboard);
        wlr_seat_keyboard_notify_modifiers(server->seat, &keyboard_grab->keyboard->modifiers);
    }
}

static void input_method_grab_keyboard(wl_listener* listener, void* data)
{
    InputMethodClient* client = nullptr;
    client = wl_container_of(listener, client, grab_keyboard);
    auto& server = *client->server;
    auto* keyboard_grab = static_cast<wlr_input_method_keyboard_grab_v2*>(data);
    // set_keyboard sends the keymap and the current modifiers to the grab.
    auto* keyboard = wlr_seat_get_keyboard(server.seat);
    wlr_input_method_keyboard_grab_v2_set_keyboard(keyboard_grab, keyboard);
    server.ime.grab_destroy.notify = input_method_grab_destroy;
    wl_signal_add(&keyboard_grab->events.destroy, &server.ime.grab_destroy);
    dbgln("WindowServer: IME: input method grabbed the keyboard");
}

static void input_method_destroy(wl_listener* listener, void*)
{
    InputMethodClient* client = nullptr;
    client = wl_container_of(listener, client, destroy);
    auto& server = *client->server;
    wl_list_remove(&client->commit.link);
    wl_list_remove(&client->grab_keyboard.link);
    wl_list_remove(&client->new_popup_surface.link);
    wl_list_remove(&client->destroy.link);
    // The grab (if any) is destroyed before the input method, so its listener is
    // already detached; detach defensively (removing a self-linked node is safe).
    wl_list_remove(&server.ime.grab_destroy.link);
    wl_list_init(&server.ime.grab_destroy.link);
    server.ime.input_method = nullptr;
    s_grab_pressed_keys.clear();
    // Keyboard focus may still be on the field: capture the surface, tell the
    // client it lost its input, and remember it so a later input method can pick
    // it up (a leave clears wlr's focused_surface, so read it first).
    if (auto* text_input = focused_text_input(server)) {
        auto* surface = text_input->input->focused_surface;
        wlr_text_input_v3_send_leave(text_input->input);
        set_pending_focused_surface(*text_input, surface);
    }
    for (auto& popup : server.ime.input_popups)
        set_popup_focus(server, *popup, nullptr);
    dbgln("WindowServer: IME: input method disconnected");
    remove_owned_ptr(server.ime.input_methods, client);
}

// --- text input (wp_text_input_v3) ------------------------------------------

static void text_input_send_state(Server& server, wlr_text_input_v3* input)
{
    auto* input_method = server.ime.input_method;
    if (!input_method)
        return;
    if (input->active_features & WLR_TEXT_INPUT_V3_FEATURE_SURROUNDING_TEXT)
        wlr_input_method_v2_send_surrounding_text(input_method, input->current.surrounding.text,
            input->current.surrounding.cursor, input->current.surrounding.anchor);
    wlr_input_method_v2_send_text_change_cause(input_method, input->current.text_change_cause);
    if (input->active_features & WLR_TEXT_INPUT_V3_FEATURE_CONTENT_TYPE)
        wlr_input_method_v2_send_content_type(input_method, input->current.content_type.hint,
            input->current.content_type.purpose);
    for (auto& popup : server.ime.input_popups)
        set_popup_focus(server, *popup, input->focused_surface);
    wlr_input_method_v2_send_done(input_method);
}

static void text_input_disable_im(Server& server, TextInput& text_input)
{
    if (!server.ime.input_method)
        return;
    wlr_input_method_v2_send_deactivate(server.ime.input_method);
    text_input_send_state(server, text_input.input);
}

static void text_input_enable(wl_listener* listener, void*)
{
    TextInput* text_input = nullptr;
    text_input = wl_container_of(listener, text_input, enable);
    auto& server = *text_input->server;
    if (!text_input->input->focused_surface)
        return;
    if (!server.ime.input_method)
        return;
    dbgln("WindowServer: IME: text input enabled");
    wlr_input_method_v2_send_activate(server.ime.input_method);
    text_input_send_state(server, text_input->input);
}

static void text_input_commit(wl_listener* listener, void*)
{
    TextInput* text_input = nullptr;
    text_input = wl_container_of(listener, text_input, commit);
    auto& server = *text_input->server;
    if (!text_input->input->focused_surface)
        return;
    if (!text_input->input->current_enabled)
        return;
    if (!server.ime.input_method)
        return;
    text_input_send_state(server, text_input->input);
}

static void text_input_disable(wl_listener* listener, void*)
{
    TextInput* text_input = nullptr;
    text_input = wl_container_of(listener, text_input, disable);
    auto& server = *text_input->server;
    if (!text_input->input->focused_surface)
        return;
    text_input_disable_im(server, *text_input);
}

static void text_input_destroy(wl_listener* listener, void*)
{
    TextInput* text_input = nullptr;
    text_input = wl_container_of(listener, text_input, destroy);
    auto& server = *text_input->server;
    if (text_input->input->current_enabled)
        text_input_disable_im(server, *text_input);
    wl_list_remove(&text_input->pending_focused_surface_destroy.link);
    wl_list_remove(&text_input->enable.link);
    wl_list_remove(&text_input->commit.link);
    wl_list_remove(&text_input->disable.link);
    wl_list_remove(&text_input->destroy.link);
    remove_owned_ptr(server.ime.text_inputs, text_input);
}

static void pending_focused_surface_destroy(wl_listener* listener, void*)
{
    TextInput* text_input = nullptr;
    text_input = wl_container_of(listener, text_input, pending_focused_surface_destroy);
    text_input->pending_focused_surface = nullptr;
    wl_list_remove(&text_input->pending_focused_surface_destroy.link);
    wl_list_init(&text_input->pending_focused_surface_destroy.link);
}

static void set_pending_focused_surface(TextInput& text_input, wlr_surface* surface)
{
    wl_list_remove(&text_input.pending_focused_surface_destroy.link);
    text_input.pending_focused_surface = surface;
    if (surface) {
        text_input.pending_focused_surface_destroy.notify = pending_focused_surface_destroy;
        wl_signal_add(&surface->events.destroy, &text_input.pending_focused_surface_destroy);
    } else {
        wl_list_init(&text_input.pending_focused_surface_destroy.link);
    }
}

static void new_text_input(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, ime.text_input_new);
    auto* input = static_cast<wlr_text_input_v3*>(data);
    if (input->seat && input->seat != server->seat)
        return;
    auto text_input = make<TextInput>();
    text_input->server = server;
    text_input->input = input;
    text_input->enable.notify = text_input_enable;
    wl_signal_add(&input->events.enable, &text_input->enable);
    text_input->commit.notify = text_input_commit;
    wl_signal_add(&input->events.commit, &text_input->commit);
    text_input->disable.notify = text_input_disable;
    wl_signal_add(&input->events.disable, &text_input->disable);
    text_input->destroy.notify = text_input_destroy;
    wl_signal_add(&input->events.destroy, &text_input->destroy);
    text_input->pending_focused_surface_destroy.notify = pending_focused_surface_destroy;
    wl_list_init(&text_input->pending_focused_surface_destroy.link);
    server->ime.text_inputs.append(move(text_input));
}

static void new_input_method(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, ime.input_method_new);
    auto* input_method = static_cast<wlr_input_method_v2*>(data);
    if (server->seat != input_method->seat)
        return;
    if (server->ime.input_method) {
        // Only one input method per seat: tell the newcomer it is second.
        wlr_input_method_v2_send_unavailable(input_method);
        return;
    }
    auto client = make<InputMethodClient>();
    client->server = server;
    client->input_method = input_method;
    client->commit.notify = input_method_commit;
    wl_signal_add(&input_method->events.commit, &client->commit);
    client->grab_keyboard.notify = input_method_grab_keyboard;
    wl_signal_add(&input_method->events.grab_keyboard, &client->grab_keyboard);
    client->new_popup_surface.notify = new_popup_surface;
    wl_signal_add(&input_method->events.new_popup_surface, &client->new_popup_surface);
    client->destroy.notify = input_method_destroy;
    wl_signal_add(&input_method->events.destroy, &client->destroy);
    server->ime.input_method = input_method;
    server->ime.input_methods.append(move(client));
    dbgln("WindowServer: IME: input method connected");
    // A field may have been focused while no input method existed: send it the
    // pending enter now.
    if (auto* text_input = focusable_text_input(*server)) {
        auto* surface = text_input->pending_focused_surface;
        set_pending_focused_surface(*text_input, nullptr);
        wlr_text_input_v3_send_enter(text_input->input, surface);
    }
}

// --- public API -------------------------------------------------------------

void InputMethod::initialize(Server& server)
{
    // The input-method grab destroy listener is added/removed as grabs come and
    // go; initialise it so a conditional removal is always safe.
    wl_list_init(&server.ime.grab_destroy.link);

    // Candidate/preedit popups draw above the shell.
    server.ime.popup_tree = wlr_scene_tree_create(&server.scene->tree);
    if (server.ime.popup_tree)
        wlr_scene_node_raise_to_top(&server.ime.popup_tree->node);

    server.ime.text_input_manager = wlr_text_input_manager_v3_create(server.display);
    if (server.ime.text_input_manager) {
        server.ime.text_input_new.notify = new_text_input;
        wl_signal_add(&server.ime.text_input_manager->events.new_text_input, &server.ime.text_input_new);
    }
    server.ime.input_method_manager = wlr_input_method_manager_v2_create(server.display);
    if (server.ime.input_method_manager) {
        server.ime.input_method_new.notify = new_input_method;
        wl_signal_add(&server.ime.input_method_manager->events.new_input_method, &server.ime.input_method_new);
    }
}

void InputMethod::finish(Server& server)
{
    remove_listener(server.ime.text_input_new);
    remove_listener(server.ime.input_method_new);
    remove_listener(server.ime.grab_destroy);
}

void InputMethod::set_focus(Server& server, wlr_surface* surface)
{
    for (auto& text_input : server.ime.text_inputs) {
        if (text_input->pending_focused_surface) {
            if (surface != text_input->pending_focused_surface)
                set_pending_focused_surface(*text_input, nullptr);
        } else if (text_input->input->focused_surface) {
            if (surface != text_input->input->focused_surface) {
                wlr_text_input_v3_send_leave(text_input->input);
                text_input_disable_im(server, *text_input);
            } else {
                continue;
            }
        }
        // text-input-v3 is per-surface: only the client owning the focused
        // surface gets `enter`.
        if (surface && wl_resource_get_client(text_input->input->resource) == wl_resource_get_client(surface->resource)) {
            if (server.ime.input_method)
                wlr_text_input_v3_send_enter(text_input->input, surface);
            else
                set_pending_focused_surface(*text_input, surface);
        }
    }
    if (auto* text_input = focused_text_input(server)) {
        for (auto& popup : server.ime.input_popups)
            set_popup_focus(server, *popup, text_input->input->focused_surface);
    } else {
        for (auto& popup : server.ime.input_popups)
            set_popup_focus(server, *popup, nullptr);
    }
}

bool InputMethod::handle_key(Server& server, uint32_t time_msec, uint32_t keycode, uint32_t state)
{
    auto* grab = active_keyboard_grab(server);
    if (!grab)
        return false;
    if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        auto* keyboard = wlr_seat_get_keyboard(server.seat);
        if (keyboard)
            wlr_input_method_keyboard_grab_v2_set_keyboard(grab, keyboard);
        wlr_input_method_keyboard_grab_v2_send_key(grab, time_msec, keycode, state);
        if (!s_grab_pressed_keys.contains_slow(keycode))
            s_grab_pressed_keys.append(keycode);
        return true;
    }
    // Only forward a release whose press went to the grab.
    for (size_t i = 0; i < s_grab_pressed_keys.size(); ++i) {
        if (s_grab_pressed_keys[i] == keycode) {
            s_grab_pressed_keys.remove(i);
            wlr_input_method_keyboard_grab_v2_send_key(grab, time_msec, keycode, state);
            return true;
        }
    }
    return false;
}

bool InputMethod::handle_modifiers(Server& server, wlr_keyboard* keyboard)
{
    auto* grab = active_keyboard_grab(server);
    if (!grab)
        return false;
    if (keyboard)
        wlr_input_method_keyboard_grab_v2_send_modifiers(grab, &keyboard->modifiers);
    return true;
}
