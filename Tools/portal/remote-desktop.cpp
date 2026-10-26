/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// R5 RemoteDesktop backend for xdg-desktop-portal-serenity.
//
// Input injection has two paths:
//   * the Notify* D-Bus methods, using wlr-virtual-pointer/virtual-keyboard;
//   * ConnectToEIS, a sender libei connection served by a libeis implementation
//     in this backend. The EIS events are translated to the same virtual input
//     path, so both routes have identical compositor behaviour.
//
// Video is the paired ScreenCast session (its stream is returned by Start).
// Clipboard access is tracked per session and served by clipboard.cpp.

#ifndef _GNU_SOURCE
#    define _GNU_SOURCE
#endif

#include "remote-desktop.h"
#include "clipboard.h"
#include "introspection.h"

#include <AK/Format.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <LibCore/Notifier.h>

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#ifdef SDE_HAVE_LIBEIS
#    include <libeis.h>
#endif

#include "screencast.h"
#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

namespace {

constexpr StringView remote_desktop_interface = "org.freedesktop.impl.portal.RemoteDesktop"sv;

enum {
    rd_device_keyboard = 1,
    rd_device_pointer = 2,
    rd_device_touchscreen = 4,
};

struct Session {
    ByteString handle;
    ByteString app_id;
    u32 devices { rd_device_keyboard | rd_device_pointer };
    u32 persist_mode { 0 };
    bool clipboard_enabled { false };
    int width { 0 };
    int height { 0 };
    bool started { false };
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
Vector<NonnullOwnPtr<Session>> s_sessions;

u32 now_ms()
{
    return static_cast<u32>(MonotonicTime::now().milliseconds());
}

#if defined(SDE_HAVE_LIBEIS)
#endif

Session* find_session(ByteString const& handle)
{
    for (auto& session : s_sessions) {
        if (session->handle == handle)
            return session.ptr();
    }
    return nullptr;
}

void rd_registry_global(void*, struct wl_registry* registry, uint32_t name, char const* interface, uint32_t version)
{
    (void)version;
    if (strcmp(interface, wl_seat_interface.name) == 0)
        g_rd.seat = static_cast<struct wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, 1));
    else if (strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name) == 0)
        g_rd.pointer_manager = static_cast<struct zwlr_virtual_pointer_manager_v1*>(wl_registry_bind(registry, name, &zwlr_virtual_pointer_manager_v1_interface, 1));
    else if (strcmp(interface, zwp_virtual_keyboard_manager_v1_interface.name) == 0)
        g_rd.keyboard_manager = static_cast<struct zwp_virtual_keyboard_manager_v1*>(wl_registry_bind(registry, name, &zwp_virtual_keyboard_manager_v1_interface, 1));
}

void rd_registry_global_remove(void*, struct wl_registry*, uint32_t) { }

struct wl_registry_listener const rd_registry_listener = {
    rd_registry_global,
    rd_registry_global_remove,
};

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

#ifdef SDE_HAVE_LIBEIS
// --- sender EIS -------------------------------------------------------------
//
// The backend is the EIS implementation for RemoteDesktop. A sender libei
// client requests devices; we advertise pointer/keyboard seats, accept those
// devices, and translate their events into the same wlr-virtual-pointer and
// virtual-keyboard objects the Notify* methods use.

struct EisClient {
    struct eis_client* client { nullptr };
    struct eis_seat* seat { nullptr };
    struct eis_device* pointer { nullptr };
    struct eis_device* keyboard { nullptr };
};

static struct eis* s_eis { nullptr };
static RefPtr<Core::Notifier> s_eis_notifier;
static Vector<NonnullOwnPtr<EisClient>> s_eis_clients;

int make_keymap_fd(size_t* size_out)
{
    auto* context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    auto* keymap = context ? xkb_keymap_new_from_names(context, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS) : nullptr;
    if (!keymap) {
        if (context)
            xkb_context_unref(context);
        return -1;
    }
    char* text = xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
    xkb_keymap_unref(keymap);
    xkb_context_unref(context);
    if (!text)
        return -1;
    size_t length = strlen(text) + 1;
    int fd = memfd_create("sde-rd-eis-keymap", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, static_cast<off_t>(length)) < 0) {
        if (fd >= 0)
            close(fd);
        free(text);
        return -1;
    }
    (void)write(fd, text, length);
    free(text);
    *size_out = length;
    return fd;
}

EisClient* find_eis_client(struct eis_seat* seat)
{
    for (auto& client : s_eis_clients) {
        if (client->seat == seat)
            return client.ptr();
    }
    return nullptr;
}

void ensure_eis_pointer(EisClient& client, uint32_t capabilities)
{
    if (client.pointer)
        return;
    auto* device = eis_seat_new_device(client.seat);
    if (!device)
        return;
    eis_device_configure_type(device, EIS_DEVICE_TYPE_VIRTUAL);
    eis_device_configure_name(device, "SDE RemoteDesktop pointer");
    // A device may only carry capabilities the client requested. In particular,
    // a client that asks for a relative pointer must not receive an absolute
    // capability (and vice versa); libeis rejects that device.
    if (capabilities & EIS_DEVICE_CAP_POINTER)
        eis_device_configure_capability(device, EIS_DEVICE_CAP_POINTER);
    if (capabilities & EIS_DEVICE_CAP_POINTER_ABSOLUTE)
        eis_device_configure_capability(device, EIS_DEVICE_CAP_POINTER_ABSOLUTE);
    if (capabilities & EIS_DEVICE_CAP_BUTTON)
        eis_device_configure_capability(device, EIS_DEVICE_CAP_BUTTON);
    if (capabilities & EIS_DEVICE_CAP_SCROLL)
        eis_device_configure_capability(device, EIS_DEVICE_CAP_SCROLL);
    if ((capabilities & EIS_DEVICE_CAP_POINTER_ABSOLUTE) && (capabilities & EIS_DEVICE_CAP_POINTER)) {
        if (auto* region = eis_device_new_region(device)) {
            eis_region_set_size(region, g_rd.width > 0 ? static_cast<u32>(g_rd.width) : 1920u,
                g_rd.height > 0 ? static_cast<u32>(g_rd.height) : 1080u);
            eis_region_set_offset(region, 0, 0);
            eis_region_add(region);
            eis_region_unref(region);
        }
    }
    eis_device_add(device);
    eis_device_resume(device);
    client.pointer = device;
    dbgln("sde-remote-desktop: EIS pointer device ready (caps {})", capabilities);
}

void ensure_eis_keyboard(EisClient& client)
{
    if (client.keyboard)
        return;
    auto* device = eis_seat_new_device(client.seat);
    if (!device)
        return;
    eis_device_configure_type(device, EIS_DEVICE_TYPE_VIRTUAL);
    eis_device_configure_name(device, "SDE RemoteDesktop keyboard");
    eis_device_configure_capability(device, EIS_DEVICE_CAP_KEYBOARD);
    size_t size = 0;
    int fd = make_keymap_fd(&size);
    if (fd >= 0) {
        if (auto* keymap = eis_device_new_keymap(device, EIS_KEYMAP_TYPE_XKB, fd, size)) {
            eis_keymap_add(keymap);
            eis_keymap_unref(keymap);
        }
        close(fd);
    }
    eis_device_add(device);
    eis_device_resume(device);
    client.keyboard = device;
    dbgln("sde-remote-desktop: EIS keyboard device ready");
}

void pump_eis()
{
    if (!s_eis)
        return;
    eis_dispatch(s_eis);
    struct eis_event* event = nullptr;
    while ((event = eis_get_event(s_eis))) {
        switch (eis_event_get_type(event)) {
        case EIS_EVENT_CLIENT_CONNECT: {
            auto* client = eis_event_get_client(event);
            eis_client_connect(client);
            auto* seat = eis_client_new_seat(client, "SDE RemoteDesktop");
            eis_seat_configure_capability(seat, EIS_DEVICE_CAP_POINTER);
            eis_seat_configure_capability(seat, EIS_DEVICE_CAP_POINTER_ABSOLUTE);
            eis_seat_configure_capability(seat, EIS_DEVICE_CAP_BUTTON);
            eis_seat_configure_capability(seat, EIS_DEVICE_CAP_SCROLL);
            eis_seat_configure_capability(seat, EIS_DEVICE_CAP_KEYBOARD);
            eis_seat_add(seat);
            auto entry = make<EisClient>();
            entry->client = client;
            entry->seat = seat;
            s_eis_clients.append(move(entry));
            dbgln("sde-remote-desktop: EIS client connected");
            break;
        }
        case EIS_EVENT_SEAT_BIND:
        case EIS_EVENT_SEAT_DEVICE_REQUESTED: {
            auto* seat = eis_event_get_seat(event);
            auto* client = find_eis_client(seat);
            if (!client)
                break;
            uint32_t pointer_caps = 0;
            for (auto cap : { EIS_DEVICE_CAP_POINTER, EIS_DEVICE_CAP_POINTER_ABSOLUTE, EIS_DEVICE_CAP_BUTTON, EIS_DEVICE_CAP_SCROLL }) {
                if (eis_event_seat_has_capability(event, cap))
                    pointer_caps |= cap;
            }
            if (pointer_caps)
                ensure_eis_pointer(*client, pointer_caps);
            if (eis_event_seat_has_capability(event, EIS_DEVICE_CAP_KEYBOARD))
                ensure_eis_keyboard(*client);
            break;
        }
        case EIS_EVENT_POINTER_MOTION:
            if (g_rd.pointer) {
                zwlr_virtual_pointer_v1_motion(g_rd.pointer, now_ms(),
                    wl_fixed_from_double(eis_event_pointer_get_dx(event)),
                    wl_fixed_from_double(eis_event_pointer_get_dy(event)));
                zwlr_virtual_pointer_v1_frame(g_rd.pointer);
                wl_display_flush(g_rd.display);
            }
            break;
        case EIS_EVENT_POINTER_MOTION_ABSOLUTE:
            if (g_rd.pointer) {
                u32 extent_x = g_rd.width > 0 ? static_cast<u32>(g_rd.width) : 1920u;
                u32 extent_y = g_rd.height > 0 ? static_cast<u32>(g_rd.height) : 1080u;
                zwlr_virtual_pointer_v1_motion_absolute(g_rd.pointer, now_ms(),
                    static_cast<u32>(eis_event_pointer_get_absolute_x(event)),
                    static_cast<u32>(eis_event_pointer_get_absolute_y(event)), extent_x, extent_y);
                zwlr_virtual_pointer_v1_frame(g_rd.pointer);
                wl_display_flush(g_rd.display);
            }
            break;
        case EIS_EVENT_BUTTON_BUTTON:
            if (g_rd.pointer) {
                zwlr_virtual_pointer_v1_button(g_rd.pointer, now_ms(),
                    eis_event_button_get_button(event),
                    eis_event_button_get_is_press(event) ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
                zwlr_virtual_pointer_v1_frame(g_rd.pointer);
                wl_display_flush(g_rd.display);
            }
            break;
        case EIS_EVENT_SCROLL_DELTA:
            if (g_rd.pointer) {
                double dx = eis_event_scroll_get_dx(event);
                double dy = eis_event_scroll_get_dy(event);
                if (dx != 0)
                    zwlr_virtual_pointer_v1_axis(g_rd.pointer, now_ms(), WL_POINTER_AXIS_HORIZONTAL_SCROLL, wl_fixed_from_double(dx));
                if (dy != 0)
                    zwlr_virtual_pointer_v1_axis(g_rd.pointer, now_ms(), WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_double(dy));
                zwlr_virtual_pointer_v1_frame(g_rd.pointer);
                wl_display_flush(g_rd.display);
            }
            break;
        case EIS_EVENT_SCROLL_DISCRETE:
            if (g_rd.pointer) {
                int dx = eis_event_scroll_get_discrete_dx(event);
                int dy = eis_event_scroll_get_discrete_dy(event);
                if (dx != 0)
                    zwlr_virtual_pointer_v1_axis_discrete(g_rd.pointer, now_ms(), WL_POINTER_AXIS_HORIZONTAL_SCROLL, wl_fixed_from_double(dx), dx);
                if (dy != 0)
                    zwlr_virtual_pointer_v1_axis_discrete(g_rd.pointer, now_ms(), WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_double(dy), dy);
                zwlr_virtual_pointer_v1_frame(g_rd.pointer);
                wl_display_flush(g_rd.display);
            }
            break;
        case EIS_EVENT_KEYBOARD_KEY:
            if (g_rd.keyboard) {
                zwp_virtual_keyboard_v1_key(g_rd.keyboard, now_ms(),
                    eis_event_keyboard_get_key(event),
                    eis_event_keyboard_get_key_is_press(event) ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED);
                wl_display_flush(g_rd.display);
            }
            break;
        case EIS_EVENT_CLIENT_DISCONNECT: {
            auto* client = eis_event_get_client(event);
            s_eis_clients.remove_first_matching([&](auto const& entry) { return entry->client == client; });
            dbgln("sde-remote-desktop: EIS client disconnected");
            break;
        }
        default:
            break;
        }
        eis_event_unref(event);
    }
}

bool ensure_eis()
{
    if (s_eis)
        return true;
    rd_ensure();
    s_eis = eis_new(nullptr);
    if (!s_eis)
        return false;
    if (eis_setup_backend_fd(s_eis) < 0) {
        warnln("sde-remote-desktop: eis_setup_backend_fd failed: {}", strerror(errno));
        eis_unref(s_eis);
        s_eis = nullptr;
        return false;
    }
    int fd = eis_get_fd(s_eis);
    s_eis_notifier = Core::Notifier::construct(fd, Core::Notifier::Type::Read);
    s_eis_notifier->on_activation = [] { pump_eis(); };
    dbgln("sde-remote-desktop: EIS implementation ready");
    return true;
}
#endif

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

ByteString object_path_or(Portal::Value const& dictionary, StringView key)
{
    if (auto const* value = dictionary.find(key); value && value->unwrapped().is(Portal::Value::Type::ObjectPath))
        return value->unwrapped().as_object_path();
    return {};
}

void handle_remote_desktop(Portal::MethodCall& call)
{
    if (call.member() == "CreateSession"sv) {
        auto handle = call.object_path_argument(1);
        if (handle.is_empty()) {
            call.reply_error(Portal::ErrorName::InvalidArgs, "missing session handle"sv);
            return;
        }
        auto* existing = find_session(handle);
        if (!existing) {
            auto session = make<Session>();
            session->handle = handle;
            session->app_id = call.string_argument(2);
            s_sessions.append(move(session));
        }
        call.reply_response(static_cast<u32>(Portal::Response::Success));
        return;
    }

    bool const session_is_argument_zero = call.member() == "ConnectToEIS"sv || call.member().starts_with("Notify"sv);
    auto session_handle = call.object_path_argument(session_is_argument_zero ? 0 : 1);
    auto* session = session_handle.is_empty() ? nullptr : find_session(session_handle);
    if (!session) {
        call.reply_error(Portal::ErrorName::InvalidArgs, "unknown RemoteDesktop session"sv);
        return;
    }

    if (call.member() == "SelectDevices"sv) {
        auto const* options = call.argument(3);
        if (options)
            session->devices = options->uint32_or("types"sv, session->devices);
        session->persist_mode = options ? Screencast::clamp_persist_mode(options->uint32_or("persist_mode"sv, 0)) : 0;
        if (options && Screencast::parse_restore_data(*options).has_value())
            dbgln("sde-remote-desktop: restored from restore_data");
        call.reply_response(static_cast<u32>(Portal::Response::Success));
        return;
    }

    if (call.member() == "Start"sv) {
        rd_ensure();
        session->started = true;

        Screencast::Stream stream;
        stream.source_type = 1;
        bool have_stream = false;
        if (auto const* options = call.argument(4)) {
            auto screencast_session = object_path_or(*options, "session_handle"sv);
            if (!screencast_session.is_empty() && Screencast::start_for_session(screencast_session, stream) == 0)
                have_stream = true;
        }
        session->width = stream.width;
        session->height = stream.height;
        g_rd.width = stream.width;
        g_rd.height = stream.height;

        Portal::Dictionary results;
        results.append(Portal::Value::Entry { "devices"sv, Portal::Value::make_variant(Portal::Value::make_uint32(session->devices)) });
        results.append(Portal::Value::Entry { "clipboard_enabled"sv, Portal::Value::make_variant(Portal::Value::make_bool(session->clipboard_enabled)) });
        if (have_stream) {
            Portal::Value::Dictionary props;
            props.append(Portal::Value::Entry { "position"sv, Portal::Value::make_variant(Portal::Value::make_struct({ Portal::Value::make_int32(0), Portal::Value::make_int32(0) })) });
            props.append(Portal::Value::Entry { "size"sv, Portal::Value::make_variant(Portal::Value::make_struct({ Portal::Value::make_int32(stream.width), Portal::Value::make_int32(stream.height) })) });
            props.append(Portal::Value::Entry { "source_type"sv, Portal::Value::make_variant(Portal::Value::make_uint32(stream.source_type)) });
            Portal::Value::Container streams;
            streams.append(Portal::Value::make_struct({ Portal::Value::make_uint32(stream.node_id), Portal::Value::make_dict(move(props)) }));
            results.append(Portal::Value::Entry { "streams"sv, Portal::Value::make_variant(Portal::Value::make_array(move(streams), "(ua{sv})"sv.to_byte_string())) });
        }
        if (session->persist_mode != 0) {
            Screencast::RestoreData restore;
            restore.types = session->devices;
            results.append(Portal::Value::Entry { "persist_mode"sv, Portal::Value::make_variant(Portal::Value::make_uint32(session->persist_mode)) });
            results.append(Portal::Value::Entry { "restore_data"sv, Portal::Value::make_variant(Screencast::make_restore_data(restore)) });
        }
        dbgln("sde-remote-desktop: started (devices {}, stream {}, clipboard {})",
            session->devices, have_stream ? stream.node_id : 0u, session->clipboard_enabled);
        call.reply_response(static_cast<u32>(Portal::Response::Success), results);
        return;
    }

    if (call.member() == "ConnectToEIS"sv) {
#ifdef SDE_HAVE_LIBEIS
        if (!ensure_eis()) {
            call.reply_error(Portal::ErrorName::NotSupported, "EIS setup failed"sv);
            return;
        }
        int fd = eis_backend_fd_add_client(s_eis);
        if (fd < 0) {
            call.reply_error(Portal::ErrorName::Failed, "could not create an EIS client connection"sv);
            return;
        }
        call.reply({ Portal::Value::make_unix_fd(fd) });
        close(fd);
        dbgln("sde-remote-desktop: handed an EIS fd to app '{}'", session->app_id);
#else
        call.reply_error(Portal::ErrorName::NotSupported, "libeis support is not compiled in"sv);
#endif
        return;
    }

    // Device notifications use argument 0 for the session, unlike the
    // configuration methods above.
    auto notification_session = call.object_path_argument(0);
    auto* notification = notification_session.is_empty() ? nullptr : find_session(notification_session);
    if (!notification) {
        call.reply_error(Portal::ErrorName::InvalidArgs, "unknown RemoteDesktop session"sv);
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
            u32 extent_x = notification->width > 0 ? static_cast<u32>(notification->width) : 1;
            u32 extent_y = notification->height > 0 ? static_cast<u32>(notification->height) : 1;
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

    call.reply_error(Portal::ErrorName::UnknownMethod, ByteString::formatted("unknown method {}", call.member()));
}

}

namespace RemoteDesktop {

bool has_session(ByteString const& handle)
{
    return find_session(handle) != nullptr;
}

void set_clipboard_enabled(ByteString const& handle, bool enabled)
{
    if (auto* session = find_session(handle))
        session->clipboard_enabled = enabled;
}

bool clipboard_enabled(ByteString const& handle)
{
    auto* session = find_session(handle);
    return session && session->clipboard_enabled;
}

void register_interface(Portal::Service& service)
{
    service.register_interface(
        remote_desktop_interface.to_byte_string(),
        [](Portal::MethodCall& call) { handle_remote_desktop(call); },
        [](StringView property) -> Optional<Portal::Value> {
            if (property == "version"sv)
                return Portal::Value::make_uint32(2);
            if (property == "AvailableDeviceTypes"sv)
                return Portal::Value::make_uint32(rd_device_keyboard | rd_device_pointer);
            return {};
        },
        Vector<ByteString> { "version"sv, "AvailableDeviceTypes"sv },
        Portal::interface_introspection_xml(remote_desktop_interface).value_or(StringView {}));
}

}
