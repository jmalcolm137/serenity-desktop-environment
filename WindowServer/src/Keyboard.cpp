/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Compositor.h"
#include "CursorHighlight.h"
#include "Idle.h"
#include "InputCapture.h"
#include "InputDevices.h"
#include "InputMethod.h"
#include "Keyboard.h"
#include "Pointer.h"
#include "Resources.h"
#include "SessionLock.h"
#include "WindowManager.h"
#include "WindowMenu.h"
#include "WindowSwitcher.h"

#include <LibCore/Timer.h>

#include <ctime>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#include "Wlroots.h"

// --- keyboard -------------------------------------------------------------

void keyboard_modifiers(wl_listener* listener, void*)
{
    Keyboard* keyboard = nullptr;
    keyboard = wl_container_of(listener, keyboard, modifiers);
    // R5 IME: while an input method holds the keyboard grab, modifiers go to it
    // instead of the focused client.
    if (InputMethod::handle_modifiers(*keyboard->server, keyboard->keyboard))
        return;
    wlr_seat_set_keyboard(keyboard->server->seat, keyboard->keyboard);
    wlr_seat_keyboard_notify_modifiers(keyboard->server->seat, &keyboard->keyboard->modifiers);
}

// Map wlroots modifier bits to Config's neutral modifier mask.
static u32 config_modifiers(uint32_t wlr_modifiers)
{
    u32 modifiers = 0;
    if (wlr_modifiers & WLR_MODIFIER_CTRL)
        modifiers |= Config::ModCtrl;
    if (wlr_modifiers & WLR_MODIFIER_ALT)
        modifiers |= Config::ModAlt;
    if (wlr_modifiers & WLR_MODIFIER_SHIFT)
        modifiers |= Config::ModShift;
    if (wlr_modifiers & WLR_MODIFIER_LOGO)
        modifiers |= Config::ModSuper;
    return modifiers;
}

// Run a configured keybinding action (R4.4).
static bool perform_key_action(Server& server, Config::Action action)
{
    if (action == Config::Action::Lock) {
        spawn_lock_client();
        return true;
    }
    return WindowManager::perform_action(server, action);
}

// Report a GlobalShortcuts activation to the portal backend over the session FIFO
// it watches (R5).
static void report_global_shortcut(StringView session, StringView id)
{
    ByteString path;
    if (auto const* runtime = getenv("XDG_RUNTIME_DIR"); runtime && *runtime)
        path = ByteString::formatted("{}/sde-global-shortcuts.fifo", runtime);
    else
        path = ByteString { "/tmp/sde-global-shortcuts.fifo" };
    int fd = ::open(path.characters(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return;
    auto message = ByteString::formatted("{}\t{}\n", session, id);
    (void)::write(fd, message.characters(), message.length());
    ::close(fd);
    dbgln("WindowServer: global shortcut activated ('{}' {})", id, session);
}

static bool handle_keybinding(Server& server, xkb_keysym_t sym, uint32_t modifiers)
{
    // Alt+Escape quits the compositor (development convenience).
    if (modifiers & WLR_MODIFIER_ALT) {
        switch (sym) {
        case XKB_KEY_Escape:
            wl_display_terminate(server.display);
            return true;
        default:
            break;
        }
    }
    // SerenityOS: Super+H toggles the cursor highlight. (Without Shift the
    // keysym is the lowercase 'h', as with Super+D above.)
    if (config_modifiers(modifiers) == Config::ModSuper && (sym == XKB_KEY_H || sym == XKB_KEY_h)) {
        // A Super+H is not a Super tap, so do not open the Taskbar menu.
        WindowManager::clear_super_tap();
        toggle_cursor_highlight(server);
        return true;
    }
    // SerenityOS: Ctrl+Super+Shift+I reloads the icon bitmaps after a scale change.
    if (config_modifiers(modifiers) == (Config::ModCtrl | Config::ModSuper | Config::ModShift) && (sym == XKB_KEY_I || sym == XKB_KEY_i)) {
        reload_icon_bitmaps_after_scale_change(server);
        return true;
    }
    return false;
}

// --- key repeat -----------------------------------------------------------
//
// wlroots 0.20 no longer runs a key-repeat timer for the compositor:
// wlr_keyboard_set_repeat_info() only advertises the rate/delay to clients
// (wl_keyboard.repeat_info) and does not synthesise repeats. So the compositor
// owns repeat: after [Keyboard] RepeatDelay it re-sends the held key to the
// focused surface, then again every 1000/RepeatRate ms, until the key is
// released. A fresh forwarded press becomes the repeating key; keys the
// compositor consumes as keybindings, and keys the keymap marks non-repeating
// (modifiers), never repeat.
static RefPtr<Core::Timer> s_key_repeat_timer;

static Keyboard* s_key_repeat_keyboard { nullptr };

static uint32_t s_key_repeat_keycode { 0 };

u32 monotonic_time_msec()
{
    timespec now { };
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<u32>(static_cast<u64>(now.tv_sec) * 1000 + static_cast<u64>(now.tv_nsec) / 1000000);
}

void stop_key_repeat()
{
    if (s_key_repeat_timer)
        s_key_repeat_timer->stop();
    s_key_repeat_keyboard = nullptr;
}

// A single-shot timer: it first fires after the delay, then re-arms itself at
// the steady repeat rate.
static void key_repeat_timer_fired()
{
    if (!s_key_repeat_keyboard) {
        stop_key_repeat();
        return;
    }
    auto& server = *s_key_repeat_keyboard->server;
    // A held key is input activity: keep the seat from going idle mid-key.
    notify_activity(server);
    wlr_seat_set_keyboard(server.seat, s_key_repeat_keyboard->keyboard);
    wlr_seat_keyboard_notify_key(server.seat, monotonic_time_msec(), s_key_repeat_keycode, WL_KEYBOARD_KEY_STATE_PRESSED);
    // Repeat is generated outside the Wayland event dispatch, so flush the
    // queued key event to the client now instead of waiting for the next
    // unrelated Wayland event (which would deliver repeats in bursts).
    wl_display_flush_clients(server.display);

    auto rate = Config::input().keyboard.repeat_rate;
    if (rate <= 0) {
        stop_key_repeat();
        return;
    }
    s_key_repeat_timer->start(max(1000 / rate, 1));
}

static void start_key_repeat(Keyboard& keyboard, uint32_t keycode)
{
    auto const& settings = Config::input().keyboard;
    auto* keymap = keyboard.keyboard->keymap;
    if (settings.repeat_rate <= 0 || !keymap || !xkb_keymap_key_repeats(keymap, keycode + 8)) {
        stop_key_repeat();
        return;
    }
    if (!s_key_repeat_timer)
        s_key_repeat_timer = Core::Timer::create_single_shot(max(settings.repeat_delay, 1), key_repeat_timer_fired);
    s_key_repeat_keyboard = &keyboard;
    s_key_repeat_keycode = keycode;
    s_key_repeat_timer->restart(max(settings.repeat_delay, 1));
}

// A client (a game, a VM viewer) can inhibit the compositor's keybindings for its
// focused surface (zwp_keyboard_shortcuts_inhibit_v1). While active, keys are
// forwarded to the client without triggering compositor actions.
static bool keyboard_shortcuts_inhibited(Server& server, wlr_surface* surface)
{
    if (!server.keyboard_shortcuts_inhibit || !surface)
        return false;
    wlr_keyboard_shortcuts_inhibitor_v1* inhibitor = nullptr;
    wl_list_for_each(inhibitor, &server.keyboard_shortcuts_inhibit->inhibitors, link) {
        if (inhibitor->active && inhibitor->surface == surface)
            return true;
    }
    return false;
}

// R5 accessibility (docs/29-accessibility.md): the modifier a keysym is, for sticky
// keys. 0 for anything that is not a modifier.
static uint32_t sticky_modifier_for_keysym(xkb_keysym_t keysym)
{
    switch (keysym) {
    case XKB_KEY_Shift_L:
    case XKB_KEY_Shift_R:
        return WLR_MODIFIER_SHIFT;
    case XKB_KEY_Control_L:
    case XKB_KEY_Control_R:
        return WLR_MODIFIER_CTRL;
    case XKB_KEY_Alt_L:
    case XKB_KEY_Alt_R:
        return WLR_MODIFIER_ALT;
    case XKB_KEY_Super_L:
    case XKB_KEY_Super_R:
    case XKB_KEY_Meta_L:
    case XKB_KEY_Meta_R:
        return WLR_MODIFIER_LOGO;
    default:
        return 0;
    }
}

void keyboard_key(wl_listener* listener, void* data)
{
    Keyboard* keyboard = nullptr;
    keyboard = wl_container_of(listener, keyboard, key);
    auto& server = *keyboard->server;
    auto* event = static_cast<wlr_keyboard_key_event*>(data);

    notify_activity(server);
    uint32_t keycode = event->keycode + 8;

    // R5 accessibility: a latched modifier is used up by one key press, and that
    // press has already been delivered with it, so drop the latch before this event
    // recomputes the masks below.
    if (keyboard->sticky_consumed) {
        keyboard->sticky_consumed = false;
        keyboard->sticky_mask = 0;
    }

    // Releasing the held key ends its repeat; unrelated releases are ignored.
    if (event->state == WL_KEYBOARD_KEY_STATE_RELEASED && s_key_repeat_keyboard && event->keycode == s_key_repeat_keycode)
        stop_key_repeat();

    // Update the keyboard's xkb state ourselves (we handle keys before the seat
    // does), so the modifier mask is correct for keybindings and for the client's
    // wl_keyboard.modifiers.
    enum xkb_key_direction direction = event->state == WL_KEYBOARD_KEY_STATE_PRESSED ? XKB_KEY_DOWN : XKB_KEY_UP;
    auto* xkb_state = keyboard->keyboard->xkb_state;
    xkb_state_update_key(xkb_state, keycode, direction);
    keyboard->keyboard->modifiers.depressed = xkb_state_serialize_mods(xkb_state, XKB_STATE_MODS_DEPRESSED);
    keyboard->keyboard->modifiers.latched = xkb_state_serialize_mods(xkb_state, XKB_STATE_MODS_LATCHED);
    keyboard->keyboard->modifiers.locked = xkb_state_serialize_mods(xkb_state, XKB_STATE_MODS_LOCKED);
    keyboard->keyboard->modifiers.group = xkb_state_serialize_layout(xkb_state, XKB_STATE_LAYOUT_EFFECTIVE);

    // R5 accessibility: sticky keys (docs/29-accessibility.md). Wayland clients
    // translate keycodes themselves, so a latched modifier only has to be in the
    // modifier mask they are told about -- the notification just below carries it,
    // and a client that sees it applies it to the following key. Settling it here
    // means keybindings, InputCapture and the input method see it too.
    if (Config::accessibility().sticky_keys) {
        bool const pressed = event->state == WL_KEYBOARD_KEY_STATE_PRESSED;
        uint32_t const tapped_bit = sticky_modifier_for_keysym(xkb_state_key_get_one_sym(xkb_state, keycode));
        if (tapped_bit != 0) {
            if (pressed) {
                keyboard->sticky_pending = true;
                keyboard->sticky_pending_bit = tapped_bit;
            } else if (keyboard->sticky_pending && keyboard->sticky_pending_bit == tapped_bit) {
                // Tapping the same modifier again cancels, so a latch made by
                // accident is not a trap.
                keyboard->sticky_mask ^= tapped_bit;
                keyboard->sticky_pending = false;
                keyboard->sticky_pending_bit = 0;
                dbgln("WindowServer: sticky keys {} (mask {:#x})", keyboard->sticky_mask != 0 ? "latched" : "cleared", keyboard->sticky_mask);
            }
        } else {
            keyboard->sticky_pending = false;
            keyboard->sticky_pending_bit = 0;
            if (pressed && keyboard->sticky_mask != 0) {
                keyboard->keyboard->modifiers.depressed |= keyboard->sticky_mask;
                keyboard->sticky_consumed = true;
                dbgln("WindowServer: sticky keys applied (mask {:#x})", keyboard->sticky_mask);
            }
        }
    }

    if (!InputMethod::handle_modifiers(server, keyboard->keyboard))
        wlr_seat_keyboard_notify_modifiers(server.seat, &keyboard->keyboard->modifiers);

    // R5: InputCapture — while keyboard capture is active, keys go to the EIS client.
    if (InputCapture::handle_keyboard_key(server, event->keycode, event->state == WL_KEYBOARD_KEY_STATE_PRESSED,
            keyboard->keyboard->modifiers.depressed, keyboard->keyboard->modifiers.latched,
            keyboard->keyboard->modifiers.locked, keyboard->keyboard->modifiers.group))
        return;

    // While the session is locked, keys go to the lock surface, not the shell.
    if (server.locked) {
        if (auto* lock = lock_surface_at_cursor(server)) {
            wlr_seat_set_keyboard(server.seat, keyboard->keyboard);
            wlr_seat_keyboard_notify_enter(server.seat, lock->lock_surface->surface, keyboard->keyboard->keycodes, keyboard->keyboard->num_keycodes, &keyboard->keyboard->modifiers);
            wlr_seat_keyboard_notify_key(server.seat, event->time_msec, event->keycode, event->state);
            if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED)
                start_key_repeat(*keyboard, event->keycode);
        }
        return;
    }

    xkb_keysym_t const* syms = nullptr;
    int syms_count = xkb_state_key_get_syms(xkb_state, keycode, &syms);

    bool handled = false;
    uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard->keyboard);
    if (server.window_menu.open) {
        // The open window menu grabs the keyboard.
        if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
            stop_key_repeat();
            for (int i = 0; i < syms_count; ++i)
                window_menu_handle_key(server, syms[i]);
        }
        return;
    }
    bool inhibited = keyboard_shortcuts_inhibited(server, server.seat->keyboard_state.focused_surface);
    if (!inhibited && event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        // R4.4: configured keybindings (defaults match the historical bindings).
        u32 const config_mods = config_modifiers(modifiers);
        // SerenityOS: Alt+Shift switches the keyboard layout (KeymapSwitcher).
        if (config_mods == (Config::ModAlt | Config::ModShift)) {
            for (int i = 0; i < syms_count; ++i) {
                if (syms[i] == XKB_KEY_Shift_L || syms[i] == XKB_KEY_Shift_R || syms[i] == XKB_KEY_Alt_L || syms[i] == XKB_KEY_Alt_R) {
                    handled |= cycle_keyboard_layout(server);
                    break;
                }
            }
        }
        for (int i = 0; i < syms_count; ++i) {
            if (auto action = Config::action_for(syms[i], config_mods); action.has_value())
                handled |= perform_key_action(server, action.value());
        }
        // R5: app global shortcuts (GlobalShortcuts portal).
        for (int i = 0; i < syms_count; ++i) {
            for (auto const& shortcut : Config::global_shortcuts()) {
                if (shortcut.keysym == syms[i] && shortcut.modifiers == config_mods) {
                    report_global_shortcut(shortcut.session, shortcut.id);
                    handled = true;
                    break;
                }
            }
        }
    }
    if (!inhibited) {
        // The window switcher (Alt+Tab / Super+Tab) grabs the keyboard while open.
        bool switcher_handled = false;
        for (int i = 0; i < syms_count; ++i)
            switcher_handled |= WindowSwitcher::handle_key(server, syms[i], modifiers, event->state == WL_KEYBOARD_KEY_STATE_PRESSED);
        // SerenityOS: while the window switcher is open, fragile (popup/menu)
        // windows are closed.
        if (WindowSwitcher::is_visible())
            dismiss_fragile_windows(server, "window switcher"sv);
        if (switcher_handled) {
            // A Super+Tab/Super+Shift+Tab shortcut is not a Super tap.
            WindowManager::clear_super_tap();
            return;
        }
        for (int i = 0; i < syms_count; ++i)
            handled |= WindowManager::handle_super_key(server, syms[i], modifiers, event->state == WL_KEYBOARD_KEY_STATE_PRESSED);
        if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
            for (int i = 0; i < syms_count; ++i)
                handled |= handle_keybinding(server, syms[i], modifiers);
        }
    }

    if (!handled && InputMethod::handle_key(server, event->time_msec, event->keycode, event->state)) {
        // R5 IME: the input method's keyboard grab consumed the key.
        if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED)
            stop_key_repeat();
        return;
    }

    if (!handled) {
        wlr_seat_set_keyboard(server.seat, keyboard->keyboard);
        wlr_seat_keyboard_notify_key(server.seat, event->time_msec, event->keycode, event->state);
        if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED)
            start_key_repeat(*keyboard, event->keycode);
    } else if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        // A key the compositor consumed (a keybinding) does not repeat.
        stop_key_repeat();
    }
}

// Advertise the seat's current capabilities to clients. Kept in one place so a
// hotplug adds the keyboard capability and an unplug removes it again; otherwise
// the seat keeps advertising a keyboard that is no longer there.
void update_seat_capabilities(Server& server)
{
    uint32_t capabilities = WL_SEAT_CAPABILITY_POINTER;
    if (!server.keyboards.is_empty())
        capabilities |= WL_SEAT_CAPABILITY_KEYBOARD;
    if (!server.touch_devices.is_empty() || server.virtual_touch_count > 0)
        capabilities |= WL_SEAT_CAPABILITY_TOUCH;
    wlr_seat_set_capabilities(server.seat, capabilities);
}

void keyboard_destroy(wl_listener* listener, void*)
{
    Keyboard* keyboard = nullptr;
    keyboard = wl_container_of(listener, keyboard, destroy);
    auto& server = *keyboard->server;
    wl_list_remove(&keyboard->modifiers.link);
    wl_list_remove(&keyboard->key.link);
    wl_list_remove(&keyboard->destroy.link);
    if (s_key_repeat_keyboard == keyboard)
        stop_key_repeat();
    remove_owned(server.keyboards, keyboard);
    // Drop the keyboard capability once the last keyboard is gone, so clients do
    // not keep a stale wl_keyboard (a later client would otherwise get one with
    // no keymap). Symmetric with server_new_input().
    update_seat_capabilities(server);
}
