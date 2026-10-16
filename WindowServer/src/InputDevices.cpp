/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Compositor.h"
#include "Focus.h"
#include "Idle.h"
#include "InputDevices.h"
#include "Keyboard.h"
#include "Tablet.h"

#include <AK/StringBuilder.h>

#include "Wlroots.h"

// --- input device configuration (R4.6) ------------------------------------

// The configured layouts rotated so `offset` is first, so Alt+Shift can switch
// the active layout by rebuilding the keymap (xkb's group order follows the
// layout list). A single layout is returned unchanged.
static ByteString rotated_layouts(ByteString const& layouts, size_t offset)
{
    auto parts = layouts.split(',');
    if (parts.size() < 2 || offset == 0)
        return layouts;
    offset %= parts.size();
    StringBuilder builder;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0)
            builder.append(',');
        builder.append(parts[(i + offset) % parts.size()].trim_whitespace());
    }
    return builder.to_byte_string();
}

// Build the xkb keymap from the configured layouts/variant/model/options and
// apply it (with repeat info) to every keyboard, falling back to the default
// keymap when the configuration does not compile.
static void apply_keyboard_input(Server& server)
{
    auto const& settings = Config::input().keyboard;
    auto model = settings.model;
    auto layout = rotated_layouts(settings.layouts, s_layout_offset);
    auto variant = settings.variant;
    auto options = settings.options;
    xkb_rule_names names {
        .rules = nullptr,
        .model = model.is_empty() ? nullptr : model.characters(),
        .layout = layout.is_empty() ? nullptr : layout.characters(),
        .variant = variant.is_empty() ? nullptr : variant.characters(),
        .options = options.is_empty() ? nullptr : options.characters(),
    };
    auto* context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    auto* keymap = xkb_keymap_new_from_names(context, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!keymap) {
        warnln("WindowServer: could not compile keymap for layouts '{}', using the default", settings.layouts);
        keymap = xkb_keymap_new_from_names(context, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS);
    }
    if (keymap) {
        for (auto& keyboard : server.input.keyboards)
            wlr_keyboard_set_keymap(keyboard->keyboard, keymap);
        xkb_keymap_unref(keymap);
    }
    xkb_context_unref(context);
    for (auto& keyboard : server.input.keyboards)
        wlr_keyboard_set_repeat_info(keyboard->keyboard, settings.repeat_rate, settings.repeat_delay);
    // Repeat is compositor-owned (wlroots 0.20 removed the timer); drop any
    // in-flight repeat if the rate was turned off.
    if (settings.repeat_rate <= 0)
        stop_key_repeat();
}

// Alt+Shift: advance to the next configured keyboard layout (SerenityOS's
// KeymapSwitcher). Rebuilds the keymap with the layout list rotated; a no-op with
// a single layout (so the chord still reaches the focused client).
bool cycle_keyboard_layout(Server& server)
{
    auto layouts = Config::input().keyboard.layouts.split(',');
    if (layouts.size() < 2)
        return false;
    s_layout_offset = (s_layout_offset + 1) % layouts.size();
    apply_keyboard_input(server);
    dbgln("WindowServer: keyboard layout -> '{}'", layouts[s_layout_offset].trim_whitespace());
    return true;
}

// Apply the mouse settings to every libinput pointer: acceleration profile and
// speed, tap-to-click, natural scroll, left-handed and disable-while-typing.
// Virtual/headless pointers have no libinput handle and are skipped.
static void apply_mouse_input(Server& server)
{
    auto const& mouse = Config::input().mouse;
    for (auto* device : server.input.pointer_devices) {
        if (!wlr_input_device_is_libinput(device))
            continue;
        auto* libinput_device = wlr_libinput_get_device_handle(device);
        if (!libinput_device)
            continue;
        if (libinput_device_config_accel_is_available(libinput_device)) {
            libinput_device_config_accel_set_speed(libinput_device, mouse.acceleration);
            auto profile = mouse.accel_profile == "flat"sv ? LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT : LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE;
            libinput_device_config_accel_set_profile(libinput_device, profile);
        }
        if (libinput_device_config_tap_get_finger_count(libinput_device) > 0)
            libinput_device_config_tap_set_enabled(libinput_device, mouse.tap_to_click ? LIBINPUT_CONFIG_TAP_ENABLED : LIBINPUT_CONFIG_TAP_DISABLED);
        if (libinput_device_config_scroll_has_natural_scroll(libinput_device))
            libinput_device_config_scroll_set_natural_scroll_enabled(libinput_device, mouse.natural_scroll);
        if (libinput_device_config_left_handed_is_available(libinput_device))
            libinput_device_config_left_handed_set(libinput_device, mouse.left_handed);
        if (libinput_device_config_dwt_is_available(libinput_device))
            libinput_device_config_dwt_set_enabled(libinput_device, mouse.disable_while_typing ? LIBINPUT_CONFIG_DWT_ENABLED : LIBINPUT_CONFIG_DWT_DISABLED);
    }
}

// Re-apply all input settings: at startup, on device hotplug and on input.ini
// change.
void apply_input_settings(Server& server)
{
    s_layout_offset = 0;
    apply_keyboard_input(server);
    apply_mouse_input(server);
    dbgln("WindowServer: applied input settings (accel {}, natural-scroll {}, buttons-switched {}, left-handed {}, layouts '{}')",
        Config::input().mouse.acceleration, Config::input().mouse.natural_scroll ? "on" : "off",
        Config::input().mouse.buttons_switched ? "on" : "off", Config::input().mouse.left_handed ? "on" : "off", Config::input().keyboard.layouts);
}

// wp-pointer-gestures-v1: forward touchpad swipe/pinch/hold to clients. libinput
// reports them per pointer device; the compositor relays them on the seat.
static void pointer_swipe_begin(wl_listener* listener, void* data)
{
    Pointer* pointer = nullptr;
    pointer = wl_container_of(listener, pointer, swipe_begin);
    auto* event = static_cast<wlr_pointer_swipe_begin_event*>(data);
    if (pointer->server->input.pointer_gestures)
        wlr_pointer_gestures_v1_send_swipe_begin(pointer->server->input.pointer_gestures, pointer->server->seat, event->time_msec, event->fingers);
    notify_activity(*pointer->server);
}

static void pointer_swipe_update(wl_listener* listener, void* data)
{
    Pointer* pointer = nullptr;
    pointer = wl_container_of(listener, pointer, swipe_update);
    auto* event = static_cast<wlr_pointer_swipe_update_event*>(data);
    if (pointer->server->input.pointer_gestures)
        wlr_pointer_gestures_v1_send_swipe_update(pointer->server->input.pointer_gestures, pointer->server->seat, event->time_msec, event->dx, event->dy);
    notify_activity(*pointer->server);
}

static void pointer_swipe_end(wl_listener* listener, void* data)
{
    Pointer* pointer = nullptr;
    pointer = wl_container_of(listener, pointer, swipe_end);
    auto* event = static_cast<wlr_pointer_swipe_end_event*>(data);
    if (pointer->server->input.pointer_gestures)
        wlr_pointer_gestures_v1_send_swipe_end(pointer->server->input.pointer_gestures, pointer->server->seat, event->time_msec, event->cancelled);
    notify_activity(*pointer->server);
}

static void pointer_pinch_begin(wl_listener* listener, void* data)
{
    Pointer* pointer = nullptr;
    pointer = wl_container_of(listener, pointer, pinch_begin);
    auto* event = static_cast<wlr_pointer_pinch_begin_event*>(data);
    if (pointer->server->input.pointer_gestures)
        wlr_pointer_gestures_v1_send_pinch_begin(pointer->server->input.pointer_gestures, pointer->server->seat, event->time_msec, event->fingers);
    notify_activity(*pointer->server);
}

static void pointer_pinch_update(wl_listener* listener, void* data)
{
    Pointer* pointer = nullptr;
    pointer = wl_container_of(listener, pointer, pinch_update);
    auto* event = static_cast<wlr_pointer_pinch_update_event*>(data);
    if (pointer->server->input.pointer_gestures)
        wlr_pointer_gestures_v1_send_pinch_update(pointer->server->input.pointer_gestures, pointer->server->seat, event->time_msec, event->dx, event->dy, event->scale, event->rotation);
    notify_activity(*pointer->server);
}

static void pointer_pinch_end(wl_listener* listener, void* data)
{
    Pointer* pointer = nullptr;
    pointer = wl_container_of(listener, pointer, pinch_end);
    auto* event = static_cast<wlr_pointer_pinch_end_event*>(data);
    if (pointer->server->input.pointer_gestures)
        wlr_pointer_gestures_v1_send_pinch_end(pointer->server->input.pointer_gestures, pointer->server->seat, event->time_msec, event->cancelled);
    notify_activity(*pointer->server);
}

static void pointer_hold_begin(wl_listener* listener, void* data)
{
    Pointer* pointer = nullptr;
    pointer = wl_container_of(listener, pointer, hold_begin);
    auto* event = static_cast<wlr_pointer_hold_begin_event*>(data);
    if (pointer->server->input.pointer_gestures)
        wlr_pointer_gestures_v1_send_hold_begin(pointer->server->input.pointer_gestures, pointer->server->seat, event->time_msec, event->fingers);
    notify_activity(*pointer->server);
}

static void pointer_hold_end(wl_listener* listener, void* data)
{
    Pointer* pointer = nullptr;
    pointer = wl_container_of(listener, pointer, hold_end);
    auto* event = static_cast<wlr_pointer_hold_end_event*>(data);
    if (pointer->server->input.pointer_gestures)
        wlr_pointer_gestures_v1_send_hold_end(pointer->server->input.pointer_gestures, pointer->server->seat, event->time_msec, event->cancelled);
    notify_activity(*pointer->server);
}

static void pointer_destroy(wl_listener* listener, void*)
{
    Pointer* pointer = nullptr;
    pointer = wl_container_of(listener, pointer, destroy);
    wl_list_remove(&pointer->swipe_begin.link);
    wl_list_remove(&pointer->swipe_update.link);
    wl_list_remove(&pointer->swipe_end.link);
    wl_list_remove(&pointer->pinch_begin.link);
    wl_list_remove(&pointer->pinch_update.link);
    wl_list_remove(&pointer->pinch_end.link);
    wl_list_remove(&pointer->hold_begin.link);
    wl_list_remove(&pointer->hold_end.link);
    wl_list_remove(&pointer->destroy.link);
    remove_owned(pointer->server->input.pointer_objects, pointer);
}

// Switch devices: lid, tablet-mode, keypad-slide. The compositor forwards the
// state (a lid close is a natural place to idle/lock) rather than owning policy.
static void switch_toggle(wl_listener* listener, void* data)
{
    SwitchDevice* device = nullptr;
    device = wl_container_of(listener, device, toggle);
    auto* event = static_cast<wlr_switch_toggle_event*>(data);
    char const* type = event->switch_type == WLR_SWITCH_TYPE_LID ? "lid"
        : event->switch_type == WLR_SWITCH_TYPE_TABLET_MODE ? "tablet-mode"
                                                            : "keypad-slide";
    dbgln("WindowServer: switch {} {}", type, event->switch_state == WLR_SWITCH_STATE_ON ? "on" : "off");
    notify_activity(*device->server);
}

static void switch_destroy(wl_listener* listener, void*)
{
    SwitchDevice* device = nullptr;
    device = wl_container_of(listener, device, destroy);
    wl_list_remove(&device->toggle.link);
    wl_list_remove(&device->destroy.link);
    remove_owned(device->server->input.switch_objects, device);
}

static void server_new_input(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, input.new_input);
    auto* device = static_cast<wlr_input_device*>(data);

    switch (device->type) {
    case WLR_INPUT_DEVICE_KEYBOARD: {
        auto keyboard = make<Keyboard>();
        keyboard->server = server;
        keyboard->keyboard = wlr_keyboard_from_input_device(device);

        keyboard->modifiers.notify = keyboard_modifiers;
        wl_signal_add(&keyboard->keyboard->events.modifiers, &keyboard->modifiers);
        keyboard->key.notify = keyboard_key;
        wl_signal_add(&keyboard->keyboard->events.key, &keyboard->key);
        keyboard->destroy.notify = keyboard_destroy;
        wl_signal_add(&device->events.destroy, &keyboard->destroy);

        wlr_seat_set_keyboard(server->seat, keyboard->keyboard);
        server->input.keyboards.append(move(keyboard));
        // Keymap and repeat come from the input config (R4.6).
        apply_keyboard_input(*server);

        // If a window is already mapped but has no keyboard focus (e.g. the first
        // keyboard just appeared), focus the topmost one so keybindings act on it.
        if (!server->seat->keyboard_state.focused_surface) {
            for (auto it = server->toplevels.rbegin(); it != server->toplevels.rend(); ++it) {
                if (!(*it)->minimized) {
                    focus_toplevel(*server, it->ptr());
                    break;
                }
            }
        }
        break;
    }
    case WLR_INPUT_DEVICE_POINTER: {
        wlr_cursor_attach_input_device(server->cursor, device);
        server->input.pointer_devices.append(device);
        apply_mouse_input(*server);
        // Track the pointer so its touchpad gestures can be forwarded (R5).
        auto pointer = make<Pointer>();
        pointer->server = server;
        pointer->device = device;
        pointer->pointer = wlr_pointer_from_input_device(device);
        pointer->swipe_begin.notify = pointer_swipe_begin;
        wl_signal_add(&pointer->pointer->events.swipe_begin, &pointer->swipe_begin);
        pointer->swipe_update.notify = pointer_swipe_update;
        wl_signal_add(&pointer->pointer->events.swipe_update, &pointer->swipe_update);
        pointer->swipe_end.notify = pointer_swipe_end;
        wl_signal_add(&pointer->pointer->events.swipe_end, &pointer->swipe_end);
        pointer->pinch_begin.notify = pointer_pinch_begin;
        wl_signal_add(&pointer->pointer->events.pinch_begin, &pointer->pinch_begin);
        pointer->pinch_update.notify = pointer_pinch_update;
        wl_signal_add(&pointer->pointer->events.pinch_update, &pointer->pinch_update);
        pointer->pinch_end.notify = pointer_pinch_end;
        wl_signal_add(&pointer->pointer->events.pinch_end, &pointer->pinch_end);
        pointer->hold_begin.notify = pointer_hold_begin;
        wl_signal_add(&pointer->pointer->events.hold_begin, &pointer->hold_begin);
        pointer->hold_end.notify = pointer_hold_end;
        wl_signal_add(&pointer->pointer->events.hold_end, &pointer->hold_end);
        pointer->destroy.notify = pointer_destroy;
        wl_signal_add(&device->events.destroy, &pointer->destroy);
        server->input.pointer_objects.append(move(pointer));
        break;
    }
    case WLR_INPUT_DEVICE_SWITCH: {
        auto switch_device = make<SwitchDevice>();
        switch_device->server = server;
        switch_device->device = wlr_switch_from_input_device(device);
        switch_device->toggle.notify = switch_toggle;
        wl_signal_add(&switch_device->device->events.toggle, &switch_device->toggle);
        switch_device->destroy.notify = switch_destroy;
        wl_signal_add(&device->events.destroy, &switch_device->destroy);
        server->input.switch_objects.append(move(switch_device));
        break;
    }
    case WLR_INPUT_DEVICE_TABLET:
        // Route tablet tool events through the cursor; the v2 wrappers are made
        // lazily when the first event for a tool arrives.
        wlr_cursor_attach_input_device(server->cursor, device);
        break;
    case WLR_INPUT_DEVICE_TABLET_PAD:
        add_tablet_pad(*server, device);
        break;
    case WLR_INPUT_DEVICE_TOUCH:
        // Touch is routed through the cursor (touch_down/motion/up events), like
        // pointers; the compositor converts them to wlr_seat touch notifications.
        wlr_cursor_attach_input_device(server->cursor, device);
        server->input.touch_devices.append(device);
        break;
    default:
        break;
    }

    update_seat_capabilities(*server);
}

// Virtual input (wlr virtual-keyboard / virtual-pointer). Both embed a real
// wlr_input_device, so route them through the same setup as physical input - this
// is what lets the headless harness script keyboard and pointer events.
static void server_new_virtual_keyboard(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, input.new_virtual_keyboard);
    auto* keyboard = static_cast<wlr_virtual_keyboard_v1*>(data);
    dbgln("WindowServer: virtual keyboard connected");
    server_new_input(&server->input.new_input, &keyboard->keyboard.base);
}

static void server_new_virtual_pointer(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, input.new_virtual_pointer);
    auto* event = static_cast<wlr_virtual_pointer_v1_new_pointer_event*>(data);
    dbgln("WindowServer: virtual pointer connected");
    server_new_input(&server->input.new_input, &event->new_pointer->pointer.base);
}

void initialize_input_devices(Server& server)
{
    // Virtual input for the headless harness (docs/09-r3-implementation-plan.md §2.4).
    server.input.virtual_keyboard_manager = wlr_virtual_keyboard_manager_v1_create(server.display);
    server.input.new_virtual_keyboard.notify = server_new_virtual_keyboard;
    wl_signal_add(&server.input.virtual_keyboard_manager->events.new_virtual_keyboard, &server.input.new_virtual_keyboard);
    server.input.virtual_pointer_manager = wlr_virtual_pointer_manager_v1_create(server.display);
    server.input.new_virtual_pointer.notify = server_new_virtual_pointer;
    wl_signal_add(&server.input.virtual_pointer_manager->events.new_virtual_pointer, &server.input.new_virtual_pointer);

    server.input.new_input.notify = server_new_input;
    wl_signal_add(&server.backend->events.new_input, &server.input.new_input);

    // R5 native-app glue: relative motion (sent from process_cursor_motion), touchpad
    // gestures (forwarded by the Pointer listeners) and keyboard-shortcuts-inhibit
    // (honoured in keyboard_key()).
    server.input.pointer_gestures = wlr_pointer_gestures_v1_create(server.display);
    server.input.keyboard_shortcuts_inhibit = wlr_keyboard_shortcuts_inhibit_v1_create(server.display);
}

void finish_input_devices(Server& server)
{
    remove_listener(server.input.new_input);
    remove_listener(server.input.new_virtual_keyboard);
    remove_listener(server.input.new_virtual_pointer);
}
