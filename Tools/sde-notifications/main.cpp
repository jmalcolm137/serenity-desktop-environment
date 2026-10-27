/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// sde-notifications — the freedesktop notification daemon for SDE.
//
// Host (GTK/Qt/X11) applications talk to org.freedesktop.Notifications; SDE's
// own applications talk to the session NotificationServer through LibGUI. This
// service owns the freedesktop bus name and forwards each notification to the
// same NotificationServer (via GUI::Notification), so both worlds share one
// banner stack and one look.
//
// It is an ordinary Serenity-style service: AK types over the `dbus-1` transport
// (Tools/portal/dbus.h, no GLib/GIO) and a LibCore event loop. The notification
// window itself is the unmodified SerenityOS NotificationServer.
//
// Capabilities are deliberately limited to what the Serenity banner can show:
// plain text (`body`) and a static icon (`icon-static`). Actions, markup,
// hyperlinks and sounds are not advertised, so well-behaved clients send plain
// text. Expiry is handled here: the freedesktop `expire_timeout` (or a default,
// except for critical urgency) closes the banner and emits NotificationClosed.

#include <AK/HashMap.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibCore/EventLoop.h>
#include <LibCore/SessionManagement.h>
#include <LibCore/Socket.h>
#include <LibCore/Timer.h>
#include <LibGfx/Bitmap.h>
#include <LibGUI/Notification.h>
#include <LibMain/Main.h>

#include "dbus.h"

namespace {

constexpr StringView notifications_bus_name = "org.freedesktop.Notifications"sv;
constexpr StringView notifications_object_path = "/org/freedesktop/Notifications"sv;
constexpr StringView notifications_interface = "org.freedesktop.Notifications"sv;

constexpr int default_expiry_ms = 6000;

// Reason codes from the Desktop Notifications Specification.
constexpr u32 reason_expired = 1;
constexpr u32 reason_dismissed = 2;
constexpr u32 reason_closed_by_call = 3;

constexpr StringView introspection_xml = R"(<interface name="org.freedesktop.Notifications">
  <method name="GetCapabilities">
    <arg type="as" name="capabilities" direction="out"/>
  </method>
  <method name="Notify">
    <arg type="s" name="app_name" direction="in"/>
    <arg type="u" name="replaces_id" direction="in"/>
    <arg type="s" name="app_icon" direction="in"/>
    <arg type="s" name="summary" direction="in"/>
    <arg type="s" name="body" direction="in"/>
    <arg type="as" name="actions" direction="in"/>
    <arg type="a{sv}" name="hints" direction="in"/>
    <arg type="i" name="expire_timeout" direction="in"/>
    <arg type="u" name="id" direction="out"/>
  </method>
  <method name="CloseNotification">
    <arg type="u" name="id" direction="in"/>
  </method>
  <method name="GetServerInformation">
    <arg type="s" name="name" direction="out"/>
    <arg type="s" name="vendor" direction="out"/>
    <arg type="s" name="version" direction="out"/>
    <arg type="s" name="spec_version" direction="out"/>
  </method>
  <signal name="NotificationClosed">
    <arg type="u" name="id"/>
    <arg type="u" name="reason"/>
  </signal>
  <signal name="ActionInvoked">
    <arg type="u" name="id"/>
    <arg type="s" name="action_key"/>
  </signal>
  <signal name="ActivationToken">
    <arg type="u" name="id"/>
    <arg type="s" name="activation_token"/>
  </signal>
</interface>)"sv;

struct NotificationEntry {
    NotificationEntry(u32 notification_id, ByteString sender_name, NonnullRefPtr<GUI::Notification> notification_object)
        : id(notification_id)
        , sender(move(sender_name))
        , notification(move(notification_object))
    {
    }

    u32 id { 0 };
    ByteString sender;
    NonnullRefPtr<GUI::Notification> notification;
    RefPtr<Core::Timer> expiry;
};

HashMap<u32, OwnPtr<NotificationEntry>> s_notifications;
u32 s_next_id { 1 };

RefPtr<Gfx::Bitmap> load_icon_path(ByteString const& path)
{
    if (path.is_empty())
        return {};

    auto const ideal_size = Gfx::IntSize { 32, 32 };
    ErrorOr<NonnullRefPtr<Gfx::Bitmap>> bitmap = Error::from_string_literal("unresolved icon");
    if (path.starts_with("file://"sv))
        bitmap = Gfx::Bitmap::load_from_file(path.substring_view(7), 1, ideal_size);
    else if (path.contains("://"sv))
        bitmap = Gfx::Bitmap::load_from_uri(path, 1, ideal_size);
    else
        bitmap = Gfx::Bitmap::load_from_file(path, 1, ideal_size);

    if (bitmap.is_error())
        return {};
    return bitmap.release_value();
}

// `image-data` (and the older `image_data` / `icon_data` spelling) is the
// (iiibiiay) struct from the specification: width, height, rowstride,
// has-alpha, bits-per-sample, channels, then ARGB32 bytes.
RefPtr<Gfx::Bitmap> bitmap_from_image_data(Portal::Value const& value)
{
    auto const& data = value.unwrapped();
    if (!data.is(Portal::Value::Type::Struct))
        return {};
    auto const& fields = data.container();
    if (fields.size() != 7)
        return {};

    int const width = fields[0].as_int32();
    int const height = fields[1].as_int32();
    int const rowstride = fields[2].as_int32();
    bool const has_alpha = fields[3].as_bool();
    int const bits_per_sample = fields[4].as_int32();
    int const channels = fields[5].as_int32();
    auto const& bytes = fields[6].container();

    if (width <= 0 || height <= 0 || width > 4096 || height > 4096)
        return {};
    if (bits_per_sample != 8 || channels != 4 || rowstride < width * 4)
        return {};
    if (bytes.size() < static_cast<size_t>(rowstride) * static_cast<size_t>(height))
        return {};

    auto format = has_alpha ? Gfx::BitmapFormat::BGRA8888 : Gfx::BitmapFormat::BGRx8888;
    auto bitmap = Gfx::Bitmap::create(format, { width, height });
    if (bitmap.is_error())
        return {};

    auto result = bitmap.release_value();
    for (int y = 0; y < height; ++y) {
        auto* destination = result->scanline(y);
        for (int x = 0; x < width * 4; ++x)
            destination[x] = bytes[static_cast<size_t>(y) * rowstride + x].as_byte();
    }
    return result;
}

RefPtr<Gfx::Bitmap> resolve_icon(ByteString const& app_icon, Portal::Value const* hints)
{
    if (hints) {
        for (auto key : { "image-data"sv, "image_data"sv, "icon_data"sv }) {
            if (auto const* value = hints->find(key); value) {
                if (auto bitmap = bitmap_from_image_data(*value))
                    return bitmap;
            }
        }
        for (auto key : { "image-path"sv, "image_path"sv }) {
            if (auto const* value = hints->find(key); value && value->unwrapped().is(Portal::Value::Type::String)) {
                if (auto bitmap = load_icon_path(value->unwrapped().as_string()))
                    return bitmap;
            }
        }
    }
    return load_icon_path(app_icon);
}

String notification_text(ByteString const& value)
{
    return MUST(String::from_utf8(value.view()));
}

u8 urgency_from_hints(Portal::Value const* hints)
{
    if (!hints)
        return 1;
    auto const* value = hints->find("urgency"sv);
    if (!value)
        return 1;
    auto const& urgency = value->unwrapped();
    if (urgency.is(Portal::Value::Type::Byte))
        return urgency.as_byte();
    if (urgency.is(Portal::Value::Type::UInt32))
        return static_cast<u8>(urgency.as_uint32());
    return 1;
}

void remove_notification(Portal::Service& service, u32 id, u32 reason)
{
    auto entry = s_notifications.take(id);
    if (!entry.has_value())
        return;

    (*entry)->notification->close();
    service.emit_signal(notifications_interface, "NotificationClosed"sv,
        { Portal::Value::make_uint32(id), Portal::Value::make_uint32(reason) });
}

void arm_expiry(Portal::Service& service, u32 id, int expire_timeout, u8 urgency)
{
    int milliseconds = expire_timeout;
    if (milliseconds < 0)
        milliseconds = urgency >= 2 ? 0 : default_expiry_ms;
    if (milliseconds == 0)
        return;

    // Defer the removal: it destroys the entry (and this timer) from inside the
    // timer's own callback otherwise.
    auto timer = Core::Timer::create_single_shot(milliseconds, [&service, id] {
        Core::deferred_invoke([&service, id] { remove_notification(service, id, reason_expired); });
    });
    timer->start();
    if (auto entry = s_notifications.get(id); entry.has_value())
        (*entry)->expiry = move(timer);
}

bool notification_server_available()
{
    // Connecting also asks LibWM's notify portal to spawn the NotificationServer,
    // so a failure here means the banner would abort the process inside show().
    auto path = Core::SessionManagement::parse_path_with_sid("/tmp/session/%sid/portal/notify"sv);
    if (path.is_error())
        return false;
    auto socket = Core::LocalSocket::connect(path.release_value());
    return !socket.is_error();
}

void handle_get_capabilities(Portal::MethodCall& call)
{
    Vector<Portal::Value> capabilities;
    capabilities.append(Portal::Value::make_string("body"));
    capabilities.append(Portal::Value::make_string("icon-static"));
    call.reply({ Portal::Value::make_array(move(capabilities), "s") });
}

void handle_get_server_information(Portal::MethodCall& call)
{
    call.reply({
        Portal::Value::make_string("SDE Notification Server"),
        Portal::Value::make_string("Serenity Desktop Environment"),
        Portal::Value::make_string("1.0"),
        Portal::Value::make_string("1.2"),
    });
}

void handle_notify(Portal::Service& service, Portal::MethodCall& call)
{
    auto app_name = call.argument(0) ? call.argument(0)->as_string() : ByteString {};
    auto replaces_id = call.argument(1) ? call.argument(1)->as_uint32() : 0;
    auto app_icon = call.argument(2) ? call.argument(2)->as_string() : ByteString {};
    auto summary = call.argument(3) ? call.argument(3)->as_string() : ByteString {};
    auto body = call.argument(4) ? call.argument(4)->as_string() : ByteString {};
    auto const* hints = call.argument(6);
    auto expire_timeout = call.argument(7) ? call.argument(7)->as_int32() : -1;

    if (!notification_server_available()) {
        call.reply_error(Portal::ErrorName::Failed, "the SDE notification server is not available"sv);
        return;
    }

    if (summary.is_empty())
        summary = app_name;
    auto icon = resolve_icon(app_icon, hints);
    auto urgency = urgency_from_hints(hints);
    auto sender = call.sender();

    NotificationEntry* entry = nullptr;
    if (replaces_id != 0) {
        if (auto existing = s_notifications.get(replaces_id); existing.has_value() && (*existing)->sender == sender)
            entry = *existing;
    }

    u32 id;
    if (entry) {
        // Replace in place (same id, same banner).
        id = entry->id;
        entry->expiry = nullptr;
        entry->notification->set_title(notification_text(summary));
        entry->notification->set_text(notification_text(body));
        entry->notification->set_icon(icon.ptr());
        entry->notification->update();
    } else {
        id = s_next_id++;
        if (s_next_id == 0)
            s_next_id = 1;

        auto notification = GUI::Notification::construct();
        notification->set_title(notification_text(summary));
        notification->set_text(notification_text(body));
        if (icon)
            notification->set_icon(icon.ptr());
        notification->show();

        auto stored = make<NotificationEntry>(id, sender, move(notification));
        s_notifications.set(id, move(stored));
        entry = s_notifications.get(id).value();
    }

    arm_expiry(service, id, expire_timeout, urgency);
    call.reply({ Portal::Value::make_uint32(id) });
}

void handle_close_notification(Portal::Service& service, Portal::MethodCall& call)
{
    auto id = call.argument(0) ? call.argument(0)->as_uint32() : 0;
    remove_notification(service, id, reason_closed_by_call);
    call.reply();
}

}

ErrorOr<int> serenity_main(Main::Arguments)
{
    Core::EventLoop event_loop;

    auto service_or_error = Portal::Service::create(
        notifications_bus_name.to_byte_string(), notifications_object_path.to_byte_string());
    if (service_or_error.is_error()) {
        // Another daemon already owns the name: stay out of its way.
        warnln("sde-notifications: {}; another notification daemon is active", service_or_error.error());
        return 0;
    }
    auto service = service_or_error.release_value();

    auto& notifications = *service;
    notifications.register_interface(
        notifications_interface.to_byte_string(),
        [&notifications](Portal::MethodCall& call) {
            if (call.member() == "GetCapabilities"sv)
                handle_get_capabilities(call);
            else if (call.member() == "GetServerInformation"sv)
                handle_get_server_information(call);
            else if (call.member() == "Notify"sv)
                handle_notify(notifications, call);
            else if (call.member() == "CloseNotification"sv)
                handle_close_notification(notifications, call);
            else
                call.reply_error(Portal::ErrorName::UnknownMethod, "unknown method"sv);
        },
        {},
        {},
        introspection_xml);

    // The window has no dismiss affordance, but the connection can still go away
    // (the server exiting, or a future close gesture): report those as dismissed.
    auto poll_timer = Core::Timer::create_repeating(500, [&notifications] {
        Vector<u32> gone;
        for (auto& it : s_notifications) {
            if (!it.value->notification->is_showing())
                gone.append(it.key);
        }
        for (auto id : gone)
            remove_notification(notifications, id, reason_dismissed);
    });
    poll_timer->start();

    dbgln("sde-notifications: serving {} on {}", notifications_object_path, notifications_bus_name);
    return event_loop.exec();
}
