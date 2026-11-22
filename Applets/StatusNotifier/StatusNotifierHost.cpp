/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "StatusNotifierHost.h"

#include <AK/String.h>
#include <LibGUI/Event.h>
#include <LibGUI/ImageWidget.h>
#include <LibGfx/Bitmap.h>
#include <LibXDG/IconTheme.h>

namespace SDE {

namespace {

constexpr StringView watcher_bus_name = "org.kde.StatusNotifierWatcher"sv;
constexpr StringView watcher_object_path = "/StatusNotifierWatcher"sv;
constexpr StringView watcher_interface = "org.kde.StatusNotifierWatcher"sv;
constexpr StringView sni_interface = "org.kde.StatusNotifierItem"sv;

constexpr StringView watcher_introspection_xml = R"(<interface name="org.kde.StatusNotifierWatcher">
  <method name="RegisterStatusNotifierItem">
    <arg name="service" type="s" direction="in"/>
  </method>
  <method name="RegisterStatusNotifierHost">
    <arg name="service" type="s" direction="in"/>
  </method>
  <signal name="StatusNotifierItemRegistered">
    <arg name="service" type="s"/>
  </signal>
  <signal name="StatusNotifierItemUnregistered">
    <arg name="service" type="s"/>
  </signal>
  <signal name="StatusNotifierHostRegistered"/>
  <property name="RegisteredStatusNotifierItems" type="as" access="read"/>
  <property name="IsStatusNotifierHostRegistered" type="b" access="read"/>
  <property name="ProtocolVersion" type="i" access="read"/>
</interface>)"sv;

class TrayIconWidget final : public GUI::ImageWidget {
    C_OBJECT(TrayIconWidget);

public:
    Function<void(int x, int y)> on_activate;
    Function<void(int x, int y)> on_secondary_activate;
    Function<void(int x, int y)> on_context_menu;

private:
    TrayIconWidget() = default;

    virtual void mousedown_event(GUI::MouseEvent& event) override
    {
        auto screen_pos = screen_relative_rect().location() + event.position();
        if (event.button() == GUI::MouseButton::Primary && on_activate) {
            on_activate(screen_pos.x(), screen_pos.y());
            return;
        }
        if (event.button() == GUI::MouseButton::Middle && on_secondary_activate) {
            on_secondary_activate(screen_pos.x(), screen_pos.y());
            return;
        }
        if (event.button() == GUI::MouseButton::Secondary && on_context_menu) {
            on_context_menu(screen_pos.x(), screen_pos.y());
            return;
        }
        GUI::ImageWidget::mousedown_event(event);
    }
};

static Optional<ByteString> get_string_property(DBusConnection* conn, ByteString const& dest, ByteString const& path, char const* prop_name)
{
    DBusMessage* msg = dbus_message_new_method_call(dest.characters(), path.characters(), "org.freedesktop.DBus.Properties", "Get");
    if (!msg)
        return {};
    char const* iface = "org.kde.StatusNotifierItem";
    dbus_message_append_args(msg, DBUS_TYPE_STRING, &iface, DBUS_TYPE_STRING, &prop_name, DBUS_TYPE_INVALID);

    DBusError err;
    dbus_error_init(&err);
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(conn, msg, 150, &err);
    dbus_message_unref(msg);
    if (!reply) {
        dbus_error_free(&err);
        return {};
    }

    Optional<ByteString> result;
    DBusMessageIter iter;
    if (dbus_message_iter_init(reply, &iter) && dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_VARIANT) {
        DBusMessageIter sub;
        dbus_message_iter_recurse(&iter, &sub);
        if (dbus_message_iter_get_arg_type(&sub) == DBUS_TYPE_STRING) {
            char const* val = nullptr;
            dbus_message_iter_get_basic(&sub, &val);
            if (val && *val)
                result = ByteString(val);
        }
    }
    dbus_message_unref(reply);
    return result;
}

}

ErrorOr<NonnullRefPtr<StatusNotifierHost>> StatusNotifierHost::try_create()
{
    auto service = TRY(Portal::Service::create(watcher_bus_name.to_byte_string(), watcher_object_path.to_byte_string()));
    return adopt_ref(*new StatusNotifierHost(move(service)));
}

StatusNotifierHost::StatusNotifierHost(NonnullRefPtr<Portal::Service> service)
    : m_service(move(service))
{
    m_service->register_interface(
        watcher_interface.to_byte_string(),
        [this](Portal::MethodCall& call) {
            if (call.member() == "RegisterStatusNotifierItem"sv) {
                auto arg = call.argument(0) ? call.argument(0)->as_string() : ByteString {};
                register_item(arg, call.sender());
                call.reply();
            } else if (call.member() == "RegisterStatusNotifierHost"sv) {
                auto host = call.argument(0) ? call.argument(0)->as_string() : ByteString {};
                if (!host.is_empty() && !m_hosts.contains_slow(host))
                    m_hosts.append(move(host));
                m_service->emit_signal(watcher_interface, "StatusNotifierHostRegistered"sv);
                call.reply();
            } else {
                call.reply_error(Portal::ErrorName::UnknownMethod, "unknown method"sv);
            }
        },
        [this](StringView property) -> Optional<Portal::Value> {
            if (property == "RegisteredStatusNotifierItems"sv) {
                Vector<Portal::Value> items;
                for (auto const& it : m_items)
                    items.append(Portal::Value::make_string(it.key));
                return Portal::Value::make_array(move(items), "s");
            }
            if (property == "IsStatusNotifierHostRegistered"sv)
                return Portal::Value::make_bool(true);
            if (property == "ProtocolVersion"sv)
                return Portal::Value::make_int32(0);
            return {};
        },
        { "RegisteredStatusNotifierItems", "IsStatusNotifierHostRegistered", "ProtocolVersion" },
        watcher_introspection_xml);

    // Watch NameOwnerChanged so tray windows disappear cleanly when a client exits.
    if (auto* conn = m_service->connection()) {
        dbus_bus_add_match(conn, "type='signal',interface='org.freedesktop.DBus',member='NameOwnerChanged'", nullptr);
        dbus_connection_add_filter(conn, dbus_signal_filter, this, nullptr);
    }

    dbgln("sde-applet-host: StatusNotifierWatcher ready on {}", watcher_bus_name);
}

StatusNotifierHost::~StatusNotifierHost()
{
    if (auto* conn = m_service->connection())
        dbus_connection_remove_filter(conn, dbus_signal_filter, this);
}

DBusHandlerResult StatusNotifierHost::dbus_signal_filter(DBusConnection*, DBusMessage* message, void* user_data)
{
    auto* self = static_cast<StatusNotifierHost*>(user_data);
    if (!self || !dbus_message_is_signal(message, "org.freedesktop.DBus", "NameOwnerChanged"))
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    char const* name = nullptr;
    char const* old_owner = nullptr;
    char const* new_owner = nullptr;
    if (dbus_message_get_args(message, nullptr,
            DBUS_TYPE_STRING, &name,
            DBUS_TYPE_STRING, &old_owner,
            DBUS_TYPE_STRING, &new_owner,
            DBUS_TYPE_INVALID)) {
        if (name && *name && (!new_owner || !*new_owner))
            self->remove_items_for_owner({ name, __builtin_strlen(name) });
    }
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

void StatusNotifierHost::register_item(ByteString const& service_arg, ByteString const& sender)
{
    ByteString bus_name = sender;
    ByteString object_path = "/StatusNotifierItem";

    if (service_arg.starts_with('/')) {
        object_path = service_arg;
    } else if (!service_arg.is_empty()) {
        bus_name = service_arg;
    }

    if (bus_name.is_empty())
        return;

    auto key = ByteString::formatted("{}{}", bus_name, object_path);
    if (auto existing = m_items.get(key); existing.has_value()) {
        refresh_item_metadata(*existing.value());
        return;
    }

    auto window = GUI::Window::construct();
    window->set_title(ByteString::formatted("Tray:{}", bus_name));
    window->set_window_type(GUI::WindowType::Applet);
    window->set_has_alpha_channel(true);
    window->resize(16, 16);

    auto widget = window->set_main_widget<TrayIconWidget>();
    widget->on_activate = [this, bus_name, object_path](int x, int y) {
        send_item_action(bus_name, object_path, "Activate", x, y);
    };
    widget->on_secondary_activate = [this, bus_name, object_path](int x, int y) {
        send_item_action(bus_name, object_path, "SecondaryActivate", x, y);
    };
    widget->on_context_menu = [this, bus_name, object_path](int x, int y) {
        send_item_action(bus_name, object_path, "ContextMenu", x, y);
    };

    auto item = make<TrayItem>(TrayItem {
        .key = key,
        .bus_name = bus_name,
        .object_path = object_path,
        .title = {},
        .icon_name = {},
        .window = move(window),
    });

    refresh_item_metadata(*item);
    item->window->show();
    m_items.set(key, move(item));

    m_service->emit_signal(watcher_interface, "StatusNotifierItemRegistered"sv, { Portal::Value::make_string(key) });
    dbgln("sde-applet-host: registered StatusNotifierItem '{}'", key);
}

void StatusNotifierHost::remove_items_for_owner(StringView owner)
{
    Vector<ByteString> to_remove;
    for (auto const& it : m_items) {
        if (it.value->bus_name == owner)
            to_remove.append(it.key);
    }
    for (auto const& key : to_remove) {
        if (auto removed = m_items.take(key); removed.has_value()) {
            removed.value()->window->close();
            m_service->emit_signal(watcher_interface, "StatusNotifierItemUnregistered"sv, { Portal::Value::make_string(key) });
            dbgln("sde-applet-host: unregistered StatusNotifierItem '{}'", key);
        }
    }
}

void StatusNotifierHost::refresh_item_metadata(TrayItem& item)
{
    auto* conn = m_service->connection();
    if (!conn)
        return;

    auto title = get_string_property(conn, item.bus_name, item.object_path, "Title");
    if (!title.has_value())
        title = get_string_property(conn, item.bus_name, item.object_path, "Id");
    if (title.has_value())
        item.title = title.release_value();

    auto icon_name = get_string_property(conn, item.bus_name, item.object_path, "IconName");
    if (icon_name.has_value())
        item.icon_name = icon_name.release_value();

    auto* widget = static_cast<TrayIconWidget*>(item.window->main_widget());
    if (!widget)
        return;

    if (!item.title.is_empty()) {
        if (auto s = String::from_byte_string(item.title); !s.is_error())
            widget->set_tooltip(s.release_value());
    }

    RefPtr<Gfx::Bitmap> bitmap;
    if (!item.icon_name.is_empty()) {
        if (auto path = XDG::find_icon(item.icon_name, 16); path.has_value()) {
            if (auto loaded = Gfx::Bitmap::load_from_file(path.value(), 1, Gfx::IntSize { 16, 16 }); !loaded.is_error())
                bitmap = loaded.release_value();
        }
    }
    if (bitmap)
        widget->set_bitmap(bitmap);
    else
        widget->load_from_file("/res/icons/16x16/app-system-monitor.png"sv);
}

void StatusNotifierHost::send_item_action(ByteString const& bus_name, ByteString const& object_path, char const* method, int x, int y)
{
    auto* conn = m_service->connection();
    if (!conn)
        return;
    DBusMessage* msg = dbus_message_new_method_call(bus_name.characters(), object_path.characters(), sni_interface.characters_without_null_termination(), method);
    if (!msg)
        return;
    dbus_int32_t dx = x;
    dbus_int32_t dy = y;
    dbus_message_append_args(msg, DBUS_TYPE_INT32, &dx, DBUS_TYPE_INT32, &dy, DBUS_TYPE_INVALID);
    dbus_connection_send(conn, msg, nullptr);
    dbus_connection_flush(conn);
    dbus_message_unref(msg);
}

}
