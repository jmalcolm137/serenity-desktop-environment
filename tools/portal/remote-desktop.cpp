/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// R5 RemoteDesktop backend for xdg-desktop-portal-serenity.
//
// Input injection uses the compositor's standard virtual input: wlr-virtual-
// pointer/unstable-v1 and virtual-keyboard/unstable-v1. Video is the paired
// ScreenCast session (its stream is returned by Start). ConnectToEIS (libei) is
// not implemented; apps fall back to the Notify* methods.
//
// This is a Serenity-style rewrite of the old GLib/GIO backend: the D-Bus
// plumbing goes through Portal:: (see dbus.h) over the `dbus-1` transport, and
// the ScreenCast/restore-token state is shared with the ScreenCast module
// (screencast.h). Everything else — Wayland, xkbcommon and the virtual input
// protocols — is unchanged.

#ifndef _GNU_SOURCE
#    define _GNU_SOURCE
#endif

#include "remote-desktop.h"
#include "introspection.h"

#include <AK/Format.h>
#include <AK/Random.h>
#include <AK/Time.h>

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "screencast.h"
#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

namespace {

constexpr StringView remote_desktop_interface = "org.freedesktop.impl.portal.RemoteDesktop"sv;

// Device bitmask (org.freedesktop.portal.RemoteDesktop.DeviceType).
enum {
    rd_device_keyboard = 1,
    rd_device_pointer = 2,
    rd_device_touchscreen = 4,
};

struct rd_state {
    int connected { 0 };
    struct wl_display* display { nullptr };
    struct wl_registry* registry { nullptr };
    struct wl_seat* seat { nullptr };
    struct zwlr_virtual_pointer_manager_v1* pointer_manager { nullptr };
    struct zwp_virtual_keyboard_manager_v1* keyboard_manager { nullptr };
    struct zwlr_virtual_pointer_v1* pointer { nullptr };
    struct zwp_virtual_keyboard_v1* keyboard { nullptr };
    int width { 0 };
    int height { 0 };
};

rd_state g_rd;
ByteString g_rd_restore_token;

u32 now_ms()
{
    return static_cast<u32>(MonotonicTime::now().milliseconds());
}

void rd_registry_global(void* data, struct wl_registry* registry, uint32_t name, char const* interface, uint32_t version)
{
    (void)data;
    (void)version;
    if (strcmp(interface, wl_seat_interface.name) == 0)
        g_rd.seat = static_cast<struct wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, 1));
    else if (strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name) == 0)
        g_rd.pointer_manager = static_cast<struct zwlr_virtual_pointer_manager_v1*>(wl_registry_bind(registry, name, &zwlr_virtual_pointer_manager_v1_interface, 1));
    else if (strcmp(interface, zwp_virtual_keyboard_manager_v1_interface.name) == 0)
        g_rd.keyboard_manager = static_cast<struct zwp_virtual_keyboard_manager_v1*>(wl_registry_bind(registry, name, &zwp_virtual_keyboard_manager_v1_interface, 1));
}

void rd_registry_global_remove(void* data, struct wl_registry* registry, uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}

struct wl_registry_listener const rd_registry_listener = {
    rd_registry_global,
    rd_registry_global_remove,
};

// Send an xkb keymap to the virtual keyboard (us layout), as the protocol
// requires before key events.
void rd_send_keymap()
{
    struct xkb_context* context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap* keymap = context ? xkb_keymap_new_from_names(context, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS) : nullptr;
    if (!keymap) {
        if (context)
            xkb_context_unref(context);
        return;
    }
    char* text = xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
    if (text) {
        size_t size = strlen(text) + 1;
        int fd = memfd_create("sde-vkbd", MFD_CLOEXEC);
        if (fd >= 0 && ftruncate(fd, static_cast<off_t>(size)) == 0) {
            (void)write(fd, text, size);
            zwp_virtual_keyboard_v1_keymap(g_rd.keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, static_cast<uint32_t>(size));
            wl_display_flush(g_rd.display);
        }
        if (fd >= 0)
            close(fd);
        free(text);
    }
    xkb_keymap_unref(keymap);
    xkb_context_unref(context);
}

void rd_ensure()
{
    if (g_rd.connected)
        return;
    g_rd.connected = 1;
    g_rd.display = wl_display_connect(nullptr);
    if (!g_rd.display) {
        warnln("sde-remote-desktop: cannot connect to Wayland");
        return;
    }
    g_rd.registry = wl_display_get_registry(g_rd.display);
    wl_registry_add_listener(g_rd.registry, &rd_registry_listener, nullptr);
    wl_display_roundtrip(g_rd.display);
    wl_display_roundtrip(g_rd.display);

    if (g_rd.pointer_manager)
        g_rd.pointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(g_rd.pointer_manager, g_rd.seat);
    if (g_rd.keyboard_manager) {
        g_rd.keyboard = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(g_rd.keyboard_manager, g_rd.seat);
        if (g_rd.keyboard)
            rd_send_keymap();
    }
    wl_display_flush(g_rd.display);
}

// --- D-Bus interface --------------------------------------------------------

double double_argument(Portal::MethodCall const& call, size_t index)
{
    if (auto const* value = call.argument(index); value && value->is(Portal::Value::Type::Double))
        return value->as_double();
    return 0.0;
}

u32 uint32_argument(Portal::MethodCall const& call, size_t index)
{
    if (auto const* value = call.argument(index); value && value->is(Portal::Value::Type::UInt32))
        return value->as_uint32();
    return 0;
}

i32 int32_argument(Portal::MethodCall const& call, size_t index)
{
    if (auto const* value = call.argument(index); value && value->is(Portal::Value::Type::Int32))
        return value->as_int32();
    return 0;
}

// Reads an `o` out of an `a{sv}` options dictionary (the values are variants).
ByteString object_path_or(Portal::Value const& dictionary, StringView key)
{
    if (auto const* value = dictionary.find(key); value && value->unwrapped().is(Portal::Value::Type::ObjectPath))
        return value->unwrapped().as_object_path();
    return {};
}

void handle_remote_desktop(Portal::MethodCall& call)
{
    if (call.member() == "CreateSession"sv) {
        call.reply_response(static_cast<u32>(Portal::Response::Success));
        return;
    }

    if (call.member() == "SelectDevices"sv) {
        if (auto const* options = call.argument(3)) {
            // Restore-token round trip (same store as ScreenCast).
            auto restore = options->string_or("restore_token"sv);
            if (!restore.is_empty()) {
                if (Screencast::load_restore_token(restore).has_value())
                    g_rd_restore_token = restore;
            }
            auto persist_mode = options->uint32_or("persist_mode"sv);
            if (persist_mode != 0 && g_rd_restore_token.is_empty()) {
                char token[33];
                for (int i = 0; i < 32; ++i)
                    token[i] = "0123456789abcdef"[get_random_uniform(16)];
                token[32] = '\0';
                g_rd_restore_token = ByteString { StringView { token, 32 } };
                Screencast::store_restore_token(g_rd_restore_token, rd_device_pointer | rd_device_keyboard, 0, {});
            }
        }
        call.reply_response(static_cast<u32>(Portal::Response::Success));
        return;
    }

    if (call.member() == "Start"sv) {
        rd_ensure();

        u32 devices = rd_device_pointer | rd_device_keyboard;

        // Return the paired ScreenCast stream if the app provided its session.
        Screencast::Stream stream;
        stream.source_type = 1;
        bool have_stream = false;
        if (auto const* options = call.argument(4)) {
            auto screencast_session = object_path_or(*options, "session_handle"sv);
            if (!screencast_session.is_empty() && Screencast::start_for_session(screencast_session, stream) == 0)
                have_stream = true;
        }
        g_rd.width = stream.width;
        g_rd.height = stream.height;

        Portal::Dictionary results;
        results.append(Portal::Value::Entry { "devices"sv, Portal::Value::make_variant(Portal::Value::make_uint32(devices)) });
        if (have_stream) {
            Portal::Value::Dictionary props;
            props.append(Portal::Value::Entry { "position"sv, Portal::Value::make_variant(Portal::Value::make_struct({ Portal::Value::make_int32(0), Portal::Value::make_int32(0) })) });
            props.append(Portal::Value::Entry { "size"sv, Portal::Value::make_variant(Portal::Value::make_struct({ Portal::Value::make_int32(stream.width), Portal::Value::make_int32(stream.height) })) });
            props.append(Portal::Value::Entry { "source_type"sv, Portal::Value::make_variant(Portal::Value::make_uint32(stream.source_type)) });
            Portal::Value::Container streams;
            streams.append(Portal::Value::make_struct({ Portal::Value::make_uint32(stream.node_id), Portal::Value::make_dict(move(props)) }));
            results.append(Portal::Value::Entry { "streams"sv, Portal::Value::make_variant(Portal::Value::make_array(move(streams), "(ua{sv})"sv.to_byte_string())) });
        }
        if (!g_rd_restore_token.is_empty())
            results.append(Portal::Value::Entry { "restore_token"sv, Portal::Value::make_variant(Portal::Value::make_string(g_rd_restore_token)) });
        dbgln("sde-remote-desktop: started (devices {}, stream {})", devices, have_stream ? stream.node_id : 0u);
        call.reply_response(static_cast<u32>(Portal::Response::Success), results);
        return;
    }

    if (call.member() == "NotifyPointerMotion"sv) {
        double dx = double_argument(call, 2);
        double dy = double_argument(call, 3);
        if (g_rd.pointer) {
            zwlr_virtual_pointer_v1_motion(g_rd.pointer, now_ms(), wl_fixed_from_double(dx), wl_fixed_from_double(dy));
            zwlr_virtual_pointer_v1_frame(g_rd.pointer);
            wl_display_flush(g_rd.display);
        }
        call.reply();
        return;
    }

    if (call.member() == "NotifyPointerMotionAbsolute"sv) {
        double x = double_argument(call, 3);
        double y = double_argument(call, 4);
        if (g_rd.pointer) {
            u32 extent_x = g_rd.width > 0 ? static_cast<u32>(g_rd.width) : 1;
            u32 extent_y = g_rd.height > 0 ? static_cast<u32>(g_rd.height) : 1;
            zwlr_virtual_pointer_v1_motion_absolute(g_rd.pointer, now_ms(), static_cast<u32>(x), static_cast<u32>(y), extent_x, extent_y);
            zwlr_virtual_pointer_v1_frame(g_rd.pointer);
            wl_display_flush(g_rd.display);
        }
        call.reply();
        return;
    }

    if (call.member() == "NotifyPointerButton"sv) {
        i32 button = int32_argument(call, 2);
        u32 state = uint32_argument(call, 3);
        if (g_rd.pointer) {
            zwlr_virtual_pointer_v1_button(g_rd.pointer, now_ms(), static_cast<u32>(button), state);
            zwlr_virtual_pointer_v1_frame(g_rd.pointer);
            wl_display_flush(g_rd.display);
        }
        call.reply();
        return;
    }

    if (call.member() == "NotifyPointerAxis"sv) {
        double dx = double_argument(call, 2);
        double dy = double_argument(call, 3);
        if (g_rd.pointer) {
            if (dy != 0.0)
                zwlr_virtual_pointer_v1_axis(g_rd.pointer, now_ms(), WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_double(dy));
            if (dx != 0.0)
                zwlr_virtual_pointer_v1_axis(g_rd.pointer, now_ms(), WL_POINTER_AXIS_HORIZONTAL_SCROLL, wl_fixed_from_double(dx));
            zwlr_virtual_pointer_v1_frame(g_rd.pointer);
            wl_display_flush(g_rd.display);
        }
        call.reply();
        return;
    }

    if (call.member() == "NotifyPointerAxisDiscrete"sv) {
        u32 axis = uint32_argument(call, 2);
        i32 steps = int32_argument(call, 3);
        if (g_rd.pointer) {
            uint32_t wl_axis = axis == 0 ? WL_POINTER_AXIS_VERTICAL_SCROLL : WL_POINTER_AXIS_HORIZONTAL_SCROLL;
            zwlr_virtual_pointer_v1_axis_discrete(g_rd.pointer, now_ms(), wl_axis, wl_fixed_from_double(static_cast<double>(steps)), steps);
            zwlr_virtual_pointer_v1_frame(g_rd.pointer);
            wl_display_flush(g_rd.display);
        }
        call.reply();
        return;
    }

    if (call.member() == "NotifyKeyboardKeycode"sv) {
        i32 keycode = int32_argument(call, 2);
        u32 state = uint32_argument(call, 3);
        if (g_rd.keyboard) {
            zwp_virtual_keyboard_v1_key(g_rd.keyboard, now_ms(), static_cast<u32>(keycode), state);
            wl_display_flush(g_rd.display);
        }
        call.reply();
        return;
    }

    if (call.member() == "NotifyKeyboardKeysym"sv) {
        // The virtual keyboard takes evdev keycodes; keysym injection would need
        // an xkb keymap lookup. Accept and ignore for now.
        call.reply();
        return;
    }

    if (call.member() == "ConnectToEIS"sv) {
        // libei is not used; apps should fall back to the Notify* methods.
        call.reply_error(Portal::ErrorName::NotSupported, "ConnectToEIS is not supported"sv);
        return;
    }

    call.reply_error(Portal::ErrorName::UnknownMethod, ByteString::formatted("unknown method {}", call.member()));
}

}

namespace RemoteDesktop {

void register_interface(Portal::Service& service)
{
    service.register_interface(
        remote_desktop_interface.to_byte_string(),
        [](Portal::MethodCall& call) { handle_remote_desktop(call); },
        [](StringView property) -> Optional<Portal::Value> {
            if (property == "version"sv)
                return Portal::Value::make_uint32(2);
            return {};
        },
        Vector<ByteString> { "version"sv },
        Portal::interface_introspection_xml(remote_desktop_interface).value_or(StringView {}));
}

}
