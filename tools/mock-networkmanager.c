/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// mock-networkmanager — a minimal stand-in for NetworkManager's D-Bus API, so
// the Wi-Fi applet can be tested deterministically (a private bus, fixed
// networks) without a real NetworkManager or radio. It serves only what the
// applet reads: org.freedesktop.DBus.Properties.GetAll on the manager, the Wi-Fi
// device and its access points, Settings.ListConnections, and the AgentManager
// Register call. It owns the org.freedesktop.NetworkManager name on whatever bus
// DBUS_SYSTEM_BUS_ADDRESS points at.
//
// Usage: mock-networkmanager

#include <dbus/dbus.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NM_NAME "org.freedesktop.NetworkManager"
#define MGR_PATH "/org/freedesktop/NetworkManager"
#define DEV_PATH "/org/freedesktop/NetworkManager/Devices/1"
#define AP1_PATH "/org/freedesktop/NetworkManager/AccessPoints/1"
#define AP2_PATH "/org/freedesktop/NetworkManager/AccessPoints/2"
#define SETTINGS_PATH "/org/freedesktop/NetworkManager/Settings"
#define AGENT_MGR_PATH "/org/freedesktop/NetworkManager/AgentManager"

#define IFACE_NM "org.freedesktop.NetworkManager"
#define IFACE_DEV "org.freedesktop.NetworkManager.Device"
#define IFACE_WIFI "org.freedesktop.NetworkManager.Device.Wireless"
#define IFACE_AP "org.freedesktop.NetworkManager.AccessPoint"
#define IFACE_SETTINGS "org.freedesktop.NetworkManager.Settings"
#define IFACE_AGENT_MGR "org.freedesktop.NetworkManager.AgentManager"
#define IFACE_PROPS "org.freedesktop.DBus.Properties"

static DBusConnection* s_connection;

static int append_bool(DBusMessageIter* array, char const* key, int value)
{
    DBusMessageIter entry;
    if (!dbus_message_iter_open_container(array, DBUS_TYPE_DICT_ENTRY, NULL, &entry))
        return 0;
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
    DBusMessageIter variant;
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "b", &variant);
    dbus_bool_t v = value ? TRUE : FALSE;
    dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &v);
    dbus_message_iter_close_container(&entry, &variant);
    dbus_message_iter_close_container(array, &entry);
    return 1;
}

static int append_u32(DBusMessageIter* array, char const* key, unsigned value)
{
    DBusMessageIter entry;
    if (!dbus_message_iter_open_container(array, DBUS_TYPE_DICT_ENTRY, NULL, &entry))
        return 0;
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
    DBusMessageIter variant;
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "u", &variant);
    dbus_uint32_t v = value;
    dbus_message_iter_append_basic(&variant, DBUS_TYPE_UINT32, &v);
    dbus_message_iter_close_container(&entry, &variant);
    dbus_message_iter_close_container(array, &entry);
    return 1;
}

static int append_byte(DBusMessageIter* array, char const* key, unsigned value)
{
    DBusMessageIter entry;
    if (!dbus_message_iter_open_container(array, DBUS_TYPE_DICT_ENTRY, NULL, &entry))
        return 0;
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
    DBusMessageIter variant;
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "y", &variant);
    unsigned char v = (unsigned char)value;
    dbus_message_iter_append_basic(&variant, DBUS_TYPE_BYTE, &v);
    dbus_message_iter_close_container(&entry, &variant);
    dbus_message_iter_close_container(array, &entry);
    return 1;
}

static int append_string(DBusMessageIter* array, char const* key, char const* value)
{
    DBusMessageIter entry;
    if (!dbus_message_iter_open_container(array, DBUS_TYPE_DICT_ENTRY, NULL, &entry))
        return 0;
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
    DBusMessageIter variant;
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &variant);
    dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &value);
    dbus_message_iter_close_container(&entry, &variant);
    dbus_message_iter_close_container(array, &entry);
    return 1;
}

static int append_object_path(DBusMessageIter* array, char const* key, char const* value)
{
    DBusMessageIter entry;
    if (!dbus_message_iter_open_container(array, DBUS_TYPE_DICT_ENTRY, NULL, &entry))
        return 0;
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
    DBusMessageIter variant;
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "o", &variant);
    dbus_message_iter_append_basic(&variant, DBUS_TYPE_OBJECT_PATH, &value);
    dbus_message_iter_close_container(&entry, &variant);
    dbus_message_iter_close_container(array, &entry);
    return 1;
}

static int append_object_paths(DBusMessageIter* array, char const* key, char const* const* values, int count)
{
    DBusMessageIter entry, variant, list;
    if (!dbus_message_iter_open_container(array, DBUS_TYPE_DICT_ENTRY, NULL, &entry))
        return 0;
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "ao", &variant);
    dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY, "o", &list);
    for (int i = 0; i < count; ++i)
        dbus_message_iter_append_basic(&list, DBUS_TYPE_OBJECT_PATH, &values[i]);
    dbus_message_iter_close_container(&variant, &list);
    dbus_message_iter_close_container(&entry, &variant);
    dbus_message_iter_close_container(array, &entry);
    return 1;
}

static int append_byte_array(DBusMessageIter* array, char const* key, char const* value)
{
    DBusMessageIter entry, variant, bytes;
    if (!dbus_message_iter_open_container(array, DBUS_TYPE_DICT_ENTRY, NULL, &entry))
        return 0;
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "ay", &variant);
    dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY, "y", &bytes);
    for (char const* p = value; *p; ++p) {
        unsigned char byte = (unsigned char)*p;
        dbus_message_iter_append_basic(&bytes, DBUS_TYPE_BYTE, &byte);
    }
    dbus_message_iter_close_container(&variant, &bytes);
    dbus_message_iter_close_container(&entry, &variant);
    dbus_message_iter_close_container(array, &entry);
    return 1;
}

static DBusMessage* build_get_all_reply(DBusMessage* call, char const* path, char const* interface)
{
    DBusMessage* reply = dbus_message_new_method_return(call);
    DBusMessageIter iter, array;
    dbus_message_iter_init_append(reply, &iter);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &array);

    if (!strcmp(path, MGR_PATH) && !strcmp(interface, IFACE_NM)) {
        char const* devices[] = { DEV_PATH };
        char const* active[] = { "/org/freedesktop/NetworkManager/ActiveConnection/1" };
        append_bool(&array, "NetworkingEnabled", 1);
        append_bool(&array, "WirelessEnabled", 1);
        append_bool(&array, "WirelessHardwareEnabled", 1);
        append_u32(&array, "Connectivity", 4);
        append_u32(&array, "State", 70);
        append_object_paths(&array, "Devices", devices, 1);
        append_object_paths(&array, "ActiveConnections", active, 1);
    } else if (!strcmp(path, DEV_PATH) && !strcmp(interface, IFACE_DEV)) {
        append_u32(&array, "DeviceType", 2); // NM_DEVICE_TYPE_WIFI
        append_u32(&array, "State", 100);    // ACTIVATED
        append_string(&array, "Interface", "wlan0");
        append_object_path(&array, "ActiveConnection", "/org/freedesktop/NetworkManager/ActiveConnection/1");
    } else if (!strcmp(path, DEV_PATH) && !strcmp(interface, IFACE_WIFI)) {
        char const* access_points[] = { AP1_PATH, AP2_PATH };
        append_object_paths(&array, "AccessPoints", access_points, 2);
        append_object_path(&array, "ActiveAccessPoint", AP1_PATH);
        append_string(&array, "HwAddress", "00:11:22:33:44:55");
        append_u32(&array, "Mode", 2);
    } else if (!strcmp(path, AP1_PATH) && !strcmp(interface, IFACE_AP)) {
        append_byte_array(&array, "Ssid", "MockNet");
        append_byte(&array, "Strength", 82);
        append_u32(&array, "Frequency", 2412);
        append_u32(&array, "Flags", 1); // privacy
        append_u32(&array, "WpaFlags", 0);
        append_u32(&array, "RsnFlags", 0x200);
        append_u32(&array, "Mode", 2);
    } else if (!strcmp(path, AP2_PATH) && !strcmp(interface, IFACE_AP)) {
        append_byte_array(&array, "Ssid", "OpenGuest");
        append_byte(&array, "Strength", 45);
        append_u32(&array, "Frequency", 2437);
        append_u32(&array, "Flags", 1); // privacy: secured, not connected
        append_u32(&array, "WpaFlags", 0);
        append_u32(&array, "RsnFlags", 0x200);
        append_u32(&array, "Mode", 2);
    }

    dbus_message_iter_close_container(&iter, &array);
    return reply;
}

static DBusHandlerResult handle_method_call(DBusMessage* message)
{
    char const* path = dbus_message_get_path(message);
    char const* interface = dbus_message_get_interface(message);
    char const* member = dbus_message_get_member(message);
    if (!path || !interface || !member)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    // Log the interesting (non-property) calls so the test can assert that a
    // passphrase reached AddAndActivateConnection.
    if (strcmp(interface, IFACE_PROPS) != 0) {
        fprintf(stderr, "mock-networkmanager: %s.%s\n", interface, member);
        fflush(stderr);
    }

    DBusMessage* reply = NULL;

    if (!strcmp(interface, IFACE_PROPS) && !strcmp(member, "GetAll")) {
        DBusMessageIter iter;
        char const* requested_interface = NULL;
        if (dbus_message_iter_init(message, &iter))
            dbus_message_iter_get_basic(&iter, &requested_interface);
        if (requested_interface)
            reply = build_get_all_reply(message, path, requested_interface);
    } else if (!strcmp(interface, IFACE_PROPS) && !strcmp(member, "Set")) {
        reply = dbus_message_new_method_return(message);
    } else if (!strcmp(interface, IFACE_SETTINGS) && !strcmp(member, "ListConnections")) {
        reply = dbus_message_new_method_return(message);
        DBusMessageIter iter, array;
        dbus_message_iter_init_append(reply, &iter);
        dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "o", &array);
        dbus_message_iter_close_container(&iter, &array);
    } else if (!strcmp(interface, IFACE_NM) && !strcmp(member, "GetDevices")) {
        reply = dbus_message_new_method_return(message);
        DBusMessageIter iter, array;
        dbus_message_iter_init_append(reply, &iter);
        dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "o", &array);
        char const* device = DEV_PATH;
        dbus_message_iter_append_basic(&array, DBUS_TYPE_OBJECT_PATH, &device);
        dbus_message_iter_close_container(&iter, &array);
    } else if (!strcmp(interface, IFACE_AGENT_MGR) && !strcmp(member, "Register")) {
        reply = dbus_message_new_method_return(message);
    } else if (!strcmp(member, "ActivateConnection") || !strcmp(member, "DeactivateConnection") || !strcmp(member, "Disconnect") || !strcmp(member, "RequestScan") || !strcmp(member, "Enable")) {
        reply = dbus_message_new_method_return(message);
    } else if (!strcmp(member, "AddAndActivateConnection")) {
        reply = dbus_message_new_method_return(message);
        DBusMessageIter iter;
        char const* active = "/org/freedesktop/NetworkManager/ActiveConnection/2";
        char const* connection = "/org/freedesktop/NetworkManager/Settings/1";
        dbus_message_iter_init_append(reply, &iter);
        dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &active);
        dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &connection);
    } else {
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    if (reply) {
        dbus_connection_send(s_connection, reply, NULL);
        dbus_message_unref(reply);
    }
    return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult filter(DBusConnection* connection, DBusMessage* message, void* user_data)
{
    (void)connection;
    (void)user_data;
    if (dbus_message_get_type(message) == DBUS_MESSAGE_TYPE_METHOD_CALL)
        return handle_method_call(message);
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

int main(void)
{
    DBusError error;
    dbus_error_init(&error);
    s_connection = dbus_bus_get(DBUS_BUS_SYSTEM, &error);
    if (!s_connection) {
        fprintf(stderr, "mock-networkmanager: cannot connect to the bus: %s\n", error.message);
        return 1;
    }

    dbus_bus_request_name(s_connection, NM_NAME, 0, &error);
    if (dbus_error_is_set(&error)) {
        fprintf(stderr, "mock-networkmanager: cannot own %s: %s\n", NM_NAME, error.message);
        return 1;
    }

    dbus_connection_add_filter(s_connection, filter, NULL, NULL);
    fprintf(stderr, "mock-networkmanager: serving %s\n", NM_NAME);

    while (dbus_connection_read_write_dispatch(s_connection, -1))
        ;
    return 0;
}
