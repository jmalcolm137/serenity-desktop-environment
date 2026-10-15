/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// The PermissionStore client (see permission-store.h). Raw libdbus, like the
// logind inhibitor in inhibit.cpp: a private session-bus connection and a short
// blocking round trip. The marshalling is kept here rather than in Portal::Value
// so the service's reply-building path is not pressed into outbound a{sas} duty.

#include "permission-store.h"

#include <AK/ScopeGuard.h>

#include <dbus/dbus.h>

namespace Portal::PermissionStore {

namespace {

constexpr char const* service_name = "org.freedesktop.impl.portal.PermissionStore";
constexpr char const* object_path = "/org/freedesktop/impl/portal/PermissionStore";
constexpr char const* interface_name = "org.freedesktop.impl.portal.PermissionStore";
constexpr int call_timeout_ms = 1000;

// A private session-bus connection, or null when there is no session bus.
DBusConnection* open_bus()
{
    DBusError error;
    dbus_error_init(&error);
    DBusConnection* bus = dbus_bus_get_private(DBUS_BUS_SESSION, &error);
    if (bus)
        dbus_connection_set_exit_on_disconnect(bus, FALSE);
    dbus_error_free(&error);
    return bus;
}

void close_bus(DBusConnection* bus)
{
    dbus_connection_close(bus);
    dbus_connection_unref(bus);
}

void append_string(DBusMessageIter* iter, StringView value)
{
    ByteString owned = value.to_byte_string();
    char const* pointer = owned.characters();
    dbus_message_iter_append_basic(iter, DBUS_TYPE_STRING, &pointer);
}

}

Optional<Permissions> lookup(StringView table, StringView id)
{
    DBusConnection* bus = open_bus();
    if (!bus)
        return {};
    ScopeGuard close_guard { [&] { close_bus(bus); } };

    DBusMessage* message = dbus_message_new_method_call(service_name, object_path, interface_name, "Lookup");
    if (!message)
        return {};
    DBusMessageIter args;
    dbus_message_iter_init_append(message, &args);
    append_string(&args, table);
    append_string(&args, id);

    DBusError error;
    dbus_error_init(&error);
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(bus, message, call_timeout_ms, &error);
    dbus_message_unref(message);

    if (!reply) {
        // No store service at all -> unreachable; any other error (e.g. the id is
        // unknown: NotFound) means the store answered, so there is simply no entry.
        bool unreachable = dbus_error_is_set(&error) && dbus_error_has_name(&error, DBUS_ERROR_SERVICE_UNKNOWN);
        dbus_error_free(&error);
        if (unreachable)
            return {};
        return Permissions {};
    }
    dbus_error_free(&error);
    ScopeGuard reply_guard { [&] { dbus_message_unref(reply); } };

    // Reply is (a{sas} permissions, v data); we only read the permissions map.
    Permissions permissions;
    DBusMessageIter iter;
    if (!dbus_message_iter_init(reply, &iter) || dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY)
        return permissions;

    DBusMessageIter entries;
    dbus_message_iter_recurse(&iter, &entries);
    while (dbus_message_iter_get_arg_type(&entries) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry;
        dbus_message_iter_recurse(&entries, &entry);

        char const* app = nullptr;
        if (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_STRING)
            dbus_message_iter_get_basic(&entry, &app);
        dbus_message_iter_next(&entry);

        Vector<ByteString> values;
        if (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_ARRAY) {
            DBusMessageIter value_iter;
            dbus_message_iter_recurse(&entry, &value_iter);
            while (dbus_message_iter_get_arg_type(&value_iter) == DBUS_TYPE_STRING) {
                char const* value = nullptr;
                dbus_message_iter_get_basic(&value_iter, &value);
                if (value)
                    values.append(ByteString { value });
                dbus_message_iter_next(&value_iter);
            }
        }
        if (app)
            permissions.set(ByteString { app }, move(values));
        dbus_message_iter_next(&entries);
    }
    return permissions;
}

bool set_permission(StringView table, StringView id, StringView app, Vector<ByteString> const& permissions)
{
    DBusConnection* bus = open_bus();
    if (!bus)
        return false;
    ScopeGuard close_guard { [&] { close_bus(bus); } };

    DBusMessage* message = dbus_message_new_method_call(service_name, object_path, interface_name, "SetPermission");
    if (!message)
        return false;
    DBusMessageIter args;
    dbus_message_iter_init_append(message, &args);
    append_string(&args, table);
    dbus_bool_t create = TRUE;
    dbus_message_iter_append_basic(&args, DBUS_TYPE_BOOLEAN, &create);
    append_string(&args, id);
    append_string(&args, app);

    DBusMessageIter values;
    dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "s", &values);
    for (auto const& permission : permissions) {
        char const* pointer = permission.characters();
        dbus_message_iter_append_basic(&values, DBUS_TYPE_STRING, &pointer);
    }
    dbus_message_iter_close_container(&args, &values);

    DBusError error;
    dbus_error_init(&error);
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(bus, message, call_timeout_ms, &error);
    dbus_message_unref(message);
    bool ok = reply != nullptr;
    if (reply)
        dbus_message_unref(reply);
    dbus_error_free(&error);
    return ok;
}

bool remove(StringView table, StringView id)
{
    DBusConnection* bus = open_bus();
    if (!bus)
        return false;
    ScopeGuard close_guard { [&] { close_bus(bus); } };

    DBusMessage* message = dbus_message_new_method_call(service_name, object_path, interface_name, "Delete");
    if (!message)
        return false;
    DBusMessageIter args;
    dbus_message_iter_init_append(message, &args);
    append_string(&args, table);
    append_string(&args, id);

    DBusError error;
    dbus_error_init(&error);
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(bus, message, call_timeout_ms, &error);
    dbus_message_unref(message);
    bool ok = reply != nullptr;
    if (reply)
        dbus_message_unref(reply);
    dbus_error_free(&error);
    return ok;
}

}
