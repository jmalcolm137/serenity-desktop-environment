/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "NetworkManagerClient.h"

#include <AK/HashMap.h>
#include <AK/QuickSort.h>
#include <AK/String.h>
#include <AK/StringBuilder.h>
#include <LibCore/EventLoop.h>

#include <dbus/dbus.h>
#include <string.h>

namespace {

constexpr char const* nm_name = "org.freedesktop.NetworkManager";
constexpr char const* nm_path = "/org/freedesktop/NetworkManager";
constexpr char const* nm_iface = "org.freedesktop.NetworkManager";
constexpr char const* props_iface = "org.freedesktop.DBus.Properties";
constexpr char const* settings_path = "/org/freedesktop/NetworkManager/Settings";
constexpr char const* settings_iface = "org.freedesktop.NetworkManager.Settings";
constexpr char const* connection_iface = "org.freedesktop.NetworkManager.Settings.Connection";
constexpr char const* device_iface = "org.freedesktop.NetworkManager.Device";
constexpr char const* wireless_iface = "org.freedesktop.NetworkManager.Device.Wireless";
constexpr char const* ap_iface = "org.freedesktop.NetworkManager.AccessPoint";
constexpr char const* active_iface = "org.freedesktop.NetworkManager.Connection.Active";
constexpr char const* agent_manager_iface = "org.freedesktop.NetworkManager.AgentManager";
constexpr char const* secret_agent_iface = "org.freedesktop.NetworkManager.SecretAgent";
constexpr char const* agent_manager_path = "/org/freedesktop/NetworkManager/AgentManager";
constexpr char const* secret_agent_path = "/org/freedesktop/NetworkManager/SecretAgent";

constexpr unsigned device_type_wifi = 2;
constexpr unsigned ap_flag_privacy = 0x1;
constexpr unsigned active_state_activating = 1;
constexpr unsigned active_state_activated = 2;

String ssid_to_string(ByteString const& ssid)
{
    if (auto decoded = String::from_utf8(ssid.view()); !decoded.is_error())
        return decoded.release_value();
    return MUST(String::formatted("<{} bytes>", ssid.length()));
}

DBusMessage* send_and_block(DBusConnection* connection, DBusMessage* message)
{
    if (!connection || !message)
        return nullptr;

    DBusError error;
    dbus_error_init(&error);
    auto* reply = dbus_connection_send_with_reply_and_block(connection, message, 5000, &error);
    dbus_message_unref(message);

    if (!reply) {
        if (dbus_error_is_set(&error)) {
            dbgln("WifiApplet: NetworkManager call failed: {} ({})", error.name, error.message);
            dbus_error_free(&error);
        }
        return nullptr;
    }
    if (dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR) {
        dbgln("WifiApplet: NetworkManager error: {}", dbus_message_get_error_name(reply));
        dbus_message_unref(reply);
        return nullptr;
    }
    return reply;
}

DBusMessage* new_call(char const* path, char const* interface, char const* method)
{
    return dbus_message_new_method_call(nm_name, path, interface, method);
}

DBusMessage* get_all(DBusConnection* connection, char const* path, char const* interface)
{
    auto* message = dbus_message_new_method_call(nm_name, path, props_iface, "GetAll");
    DBusMessageIter iter;
    dbus_message_iter_init_append(message, &iter);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &interface);
    return send_and_block(connection, message);
}

// A {sv} dictionary reader. `iter` must point at the a{sv} array.
template<typename Callback>
bool dict_entry(DBusMessageIter* iter, char const* wanted, Callback&& callback)
{
    DBusMessageIter dict;
    dbus_message_iter_recurse(iter, &dict);
    while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry;
        dbus_message_iter_recurse(&dict, &entry);
        char const* key = nullptr;
        dbus_message_iter_get_basic(&entry, &key);
        dbus_message_iter_next(&entry);
        if (key && strcmp(key, wanted) == 0) {
            // `entry` now points at the variant.
            callback(&entry);
            return true;
        }
        dbus_message_iter_next(&dict);
    }
    return false;
}

bool variant_bool(DBusMessageIter* variant, bool fallback = false)
{
    if (dbus_message_iter_get_arg_type(variant) != DBUS_TYPE_VARIANT)
        return fallback;
    DBusMessageIter value;
    dbus_message_iter_recurse(variant, &value);
    if (dbus_message_iter_get_arg_type(&value) != DBUS_TYPE_BOOLEAN)
        return fallback;
    dbus_bool_t result = 0;
    dbus_message_iter_get_basic(&value, &result);
    return result;
}

unsigned variant_u32(DBusMessageIter* variant, unsigned fallback = 0)
{
    if (dbus_message_iter_get_arg_type(variant) != DBUS_TYPE_VARIANT)
        return fallback;
    DBusMessageIter value;
    dbus_message_iter_recurse(variant, &value);
    auto type = dbus_message_iter_get_arg_type(&value);
    if (type == DBUS_TYPE_UINT32 || type == DBUS_TYPE_INT32) {
        dbus_uint32_t result = 0;
        dbus_message_iter_get_basic(&value, &result);
        return result;
    }
    if (type == DBUS_TYPE_BYTE) {
        unsigned char result = 0;
        dbus_message_iter_get_basic(&value, &result);
        return result;
    }
    return fallback;
}

ByteString variant_bytes(DBusMessageIter* variant)
{
    if (dbus_message_iter_get_arg_type(variant) != DBUS_TYPE_VARIANT)
        return {};
    DBusMessageIter value;
    dbus_message_iter_recurse(variant, &value);
    if (dbus_message_iter_get_arg_type(&value) != DBUS_TYPE_ARRAY)
        return {};
    DBusMessageIter element;
    dbus_message_iter_recurse(&value, &element);
    ByteBuffer buffer;
    while (dbus_message_iter_get_arg_type(&element) == DBUS_TYPE_BYTE) {
        unsigned char byte = 0;
        dbus_message_iter_get_basic(&element, &byte);
        buffer.append(byte);
        dbus_message_iter_next(&element);
    }
    return ByteString { buffer.bytes() };
}

ByteString variant_string(DBusMessageIter* variant)
{
    if (dbus_message_iter_get_arg_type(variant) != DBUS_TYPE_VARIANT)
        return {};
    DBusMessageIter value;
    dbus_message_iter_recurse(variant, &value);
    if (dbus_message_iter_get_arg_type(&value) != DBUS_TYPE_STRING && dbus_message_iter_get_arg_type(&value) != DBUS_TYPE_OBJECT_PATH)
        return {};
    char const* string = nullptr;
    dbus_message_iter_get_basic(&value, &string);
    return string ? ByteString { string } : ByteString {};
}

Vector<ByteString> variant_object_paths(DBusMessageIter* variant)
{
    Vector<ByteString> paths;
    if (dbus_message_iter_get_arg_type(variant) != DBUS_TYPE_VARIANT)
        return paths;
    DBusMessageIter value;
    dbus_message_iter_recurse(variant, &value);
    if (dbus_message_iter_get_arg_type(&value) != DBUS_TYPE_ARRAY)
        return paths;
    DBusMessageIter element;
    dbus_message_iter_recurse(&value, &element);
    while (dbus_message_iter_get_arg_type(&element) == DBUS_TYPE_OBJECT_PATH) {
        char const* path = nullptr;
        dbus_message_iter_get_basic(&element, &path);
        if (path)
            paths.append(ByteString { path });
        dbus_message_iter_next(&element);
    }
    return paths;
}

// Append a {sv} entry to an open a{sv} array with a string value.
void append_string_entry(DBusMessageIter* array, char const* key, char const* value)
{
    DBusMessageIter entry, variant;
    dbus_message_iter_open_container(array, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &variant);
    dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &value);
    dbus_message_iter_close_container(&entry, &variant);
    dbus_message_iter_close_container(array, &entry);
}

void append_bool_entry(DBusMessageIter* array, char const* key, bool value)
{
    DBusMessageIter entry, variant;
    dbus_bool_t dbus_value = value;
    dbus_message_iter_open_container(array, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "b", &variant);
    dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &dbus_value);
    dbus_message_iter_close_container(&entry, &variant);
    dbus_message_iter_close_container(array, &entry);
}

void append_bytes_entry(DBusMessageIter* array, char const* key, ByteString const& value)
{
    DBusMessageIter entry, variant, bytes;
    dbus_message_iter_open_container(array, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "ay", &variant);
    dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY, "y", &bytes);
    for (size_t i = 0; i < value.length(); ++i) {
        unsigned char byte = static_cast<unsigned char>(value.characters()[i]);
        dbus_message_iter_append_basic(&bytes, DBUS_TYPE_BYTE, &byte);
    }
    dbus_message_iter_close_container(&variant, &bytes);
    dbus_message_iter_close_container(&entry, &variant);
    dbus_message_iter_close_container(array, &entry);
}

// The reply to ListConnections() is an array of object paths.
Vector<ByteString> parse_object_path_array(DBusMessage* reply)
{
    Vector<ByteString> paths;
    if (!reply)
        return paths;
    DBusMessageIter iter;
    if (!dbus_message_iter_init(reply, &iter))
        return paths;
    if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY)
        return paths;
    DBusMessageIter element;
    dbus_message_iter_recurse(&iter, &element);
    while (dbus_message_iter_get_arg_type(&element) == DBUS_TYPE_OBJECT_PATH) {
        char const* path = nullptr;
        dbus_message_iter_get_basic(&element, &path);
        if (path)
            paths.append(ByteString { path });
        dbus_message_iter_next(&element);
    }
    return paths;
}

struct SavedConnection {
    ByteString ssid;
    bool wireless { false };
};

// Reads a settings a{sa{sv}} reply, returning the SSID and whether it is an
// 802-11-wireless connection.
Optional<SavedConnection> parse_connection_settings(DBusMessage* reply)
{
    if (!reply)
        return {};
    DBusMessageIter iter;
    if (!dbus_message_iter_init(reply, &iter) || dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY)
        return {};
    DBusMessageIter dict;
    dbus_message_iter_recurse(&iter, &dict);

    SavedConnection result;
    while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry;
        dbus_message_iter_recurse(&dict, &entry);
        char const* section = nullptr;
        dbus_message_iter_get_basic(&entry, &section);
        dbus_message_iter_next(&entry);
        if (section) {
            // `entry` points at the variant holding the section's a{sv}.
            DBusMessageIter value;
            dbus_message_iter_recurse(&entry, &value);
            if (strcmp(section, "802-11-wireless") == 0) {
                result.wireless = true;
                if (dbus_message_iter_get_arg_type(&value) == DBUS_TYPE_ARRAY)
                    dict_entry(&value, "ssid", [&](DBusMessageIter* variant) { result.ssid = variant_bytes(variant); });
            }
        }
        dbus_message_iter_next(&dict);
    }
    if (result.ssid.is_empty())
        return {};
    return result;
}

}

NetworkManagerClient::~NetworkManagerClient()
{
    if (m_pending_secret_reply)
        dbus_message_unref(m_pending_secret_reply);
    if (m_connection)
        dbus_connection_unref(m_connection);
}

DBusHandlerResult NetworkManagerClient::filter(DBusConnection*, DBusMessage* message, void* user_data)
{
    auto* client = static_cast<NetworkManagerClient*>(user_data);
    if (dbus_message_get_type(message) == DBUS_MESSAGE_TYPE_METHOD_CALL) {
        auto const* interface = dbus_message_get_interface(message);
        if (interface && strcmp(interface, secret_agent_iface) == 0) {
            client->handle_secret_agent_call(message);
            return DBUS_HANDLER_RESULT_HANDLED;
        }
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    if (dbus_message_get_type(message) == DBUS_MESSAGE_TYPE_SIGNAL) {
        auto const* interface = dbus_message_get_interface(message);
        if (interface) {
            if (strcmp(interface, "org.freedesktop.DBus") == 0) {
                // NameOwnerChanged: did NetworkManager itself arrive or go away?
                DBusMessageIter iter;
                if (dbus_message_iter_init(message, &iter)) {
                    char const* changed_name = nullptr;
                    if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_STRING) {
                        dbus_message_iter_get_basic(&iter, &changed_name);
                    }
                    if (changed_name && strcmp(changed_name, "org.freedesktop.NetworkManager") == 0) {
                        dbgln("WifiApplet: NetworkManager restarted; re-registering and refreshing");
                        client->register_agent();
                        client->schedule_refresh();
                    }
                }
                return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
            }
            bool interesting = strcmp(interface, "org.freedesktop.DBus.Properties") == 0
                || strcmp(interface, device_iface) == 0
                || strcmp(interface, wireless_iface) == 0
                || strcmp(interface, nm_iface) == 0;
            if (interesting)
                client->schedule_refresh();
        }
    }
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

NetworkManagerClient::NetworkManagerClient()
{
    if (!connect_to_bus()) {
        m_state.available = false;
        return;
    }
    m_state.available = true;
    refresh();
}

void NetworkManagerClient::register_agent()
{
    // Register as a NetworkManager secret agent, so activating a saved WPA
    // network connects (NetworkManager asks us for the passphrase). NetworkManager
    // drops its clients when it restarts, which is why this runs again then.
    if (!m_connection)
        return;
    auto* register_message = new_call(agent_manager_path, agent_manager_iface, "Register");
    char const* identifier = "SDE Wi-Fi applet";
    DBusMessageIter iter;
    dbus_message_iter_init_append(register_message, &iter);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &identifier);
    dbus_connection_send(m_connection, register_message, nullptr);
    dbus_message_unref(register_message);
    dbus_connection_flush(m_connection);
}

bool NetworkManagerClient::connect_to_bus()
{
    DBusError error;
    dbus_error_init(&error);
    m_connection = dbus_bus_get(DBUS_BUS_SYSTEM, &error);
    if (!m_connection) {
        if (dbus_error_is_set(&error)) {
            dbgln("WifiApplet: no system bus: {} ({})", error.name, error.message);
            dbus_error_free(&error);
        }
        return false;
    }

    dbus_connection_set_exit_on_disconnect(m_connection, false);
    dbus_connection_add_filter(m_connection, &NetworkManagerClient::filter, this, nullptr);

    dbus_bus_add_match(m_connection, "type='signal',interface='org.freedesktop.DBus.Properties'", &error);
    if (dbus_error_is_set(&error))
        dbus_error_free(&error);
    dbus_bus_add_match(m_connection, "type='signal',sender='org.freedesktop.NetworkManager'", &error);
    if (dbus_error_is_set(&error))
        dbus_error_free(&error);
    // NetworkManager forgets its clients when it restarts, and the applet's data
    // goes stale: watch its name owner so that can be noticed and recovered from.
    dbus_bus_add_match(m_connection, "type='signal',interface='org.freedesktop.DBus',member='NameOwnerChanged'", &error);
    if (dbus_error_is_set(&error))
        dbus_error_free(&error);

    register_agent();

    int fd = -1;
    if (dbus_connection_get_unix_fd(m_connection, &fd) && fd >= 0) {
        m_notifier = Core::Notifier::construct(fd, Core::Notifier::Type::Read, this);
        m_notifier->on_activation = [this] { on_socket_readable(); };
    }

    return true;
}

void NetworkManagerClient::reconnect()
{
    if (connect_to_bus()) {
        dbgln("WifiApplet: reconnected to the system bus");
        m_state.available = true;
        refresh();
        return;
    }
    schedule_reconnect();
}

void NetworkManagerClient::schedule_reconnect()
{
    // NetworkManager may be restarting: keep trying, a couple of seconds apart,
    // rather than sitting on a dead socket.
    if (!m_reconnect_timer)
        m_reconnect_timer = Core::Timer::create_single_shot(2000, [this] { reconnect(); });
    m_reconnect_timer->start();
}


void NetworkManagerClient::on_socket_readable()
{
    if (!m_connection)
        return;
    if (!dbus_connection_get_is_connected(m_connection)) {
        // NetworkManager (or the bus) went away. Drop the dead connection and
        // rebuild it, so the applet recovers instead of showing stale networks.
        dbgln("WifiApplet: the system bus connection dropped; reconnecting");
        m_notifier.clear();
        dbus_connection_unref(m_connection);
        m_connection = nullptr;
        m_state.available = false;
        schedule_reconnect();
        return;
    }
    dbus_connection_read_write(m_connection, 0);
    while (dbus_connection_get_dispatch_status(m_connection) == DBUS_DISPATCH_DATA_REMAINS)
        dbus_connection_dispatch(m_connection);

    if (m_needs_refresh)
        schedule_refresh();
}

void NetworkManagerClient::schedule_refresh()
{
    m_needs_refresh = false;
    if (m_refresh_scheduled)
        return;
    m_refresh_scheduled = true;
    // Refresh on a fresh turn of the loop, never inside the dispatcher.
    Core::deferred_invoke([this] {
        m_refresh_scheduled = false;
        refresh();
    });
}

void NetworkManagerClient::set_wireless_enabled(bool enabled)
{
    if (!m_connection)
        return;
    auto* message = dbus_message_new_method_call(nm_name, nm_path, props_iface, "Set");
    DBusMessageIter iter, variant;
    char const* interface = nm_iface;
    char const* property = "WirelessEnabled";
    dbus_bool_t value = enabled;
    dbus_message_iter_init_append(message, &iter);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &interface);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &property);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_VARIANT, "b", &variant);
    dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &value);
    dbus_message_iter_close_container(&iter, &variant);
    if (auto* reply = send_and_block(m_connection, message))
        dbus_message_unref(reply);
    schedule_refresh();
}

void NetworkManagerClient::connect_to(WifiNetwork const& network, Optional<ByteString> psk)
{
    if (!m_connection)
        return;

    if (network.saved && !network.saved_connection_path.is_empty()) {
        // Activate the saved connection; our secret agent supplies the key if
        // NetworkManager needs one.
        auto* message = new_call(nm_path, nm_iface, "ActivateConnection");
        DBusMessageIter iter;
        char const* connection_path = network.saved_connection_path.characters();
        char const* device_path = network.device_path.characters();
        char const* specific = network.access_point_path.characters();
        dbus_message_iter_init_append(message, &iter);
        dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &connection_path);
        dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &device_path);
        dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &specific);
        if (auto* reply = send_and_block(m_connection, message))
            dbus_message_unref(reply);
        schedule_refresh();
        return;
    }

    auto* message = new_call(nm_path, nm_iface, "AddAndActivateConnection");
    DBusMessageIter iter, settings, connection_entry, inner, security_entry, security;
    char const* profile_id = network.ssid.characters();
    char const* type = "802-11-wireless";
    char const* mode = "infrastructure";
    char const* key_mgmt = "wpa-psk";

    dbus_message_iter_init_append(message, &iter);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sa{sv}}", &settings);

    auto open_section = [&](char const* name, DBusMessageIter& entry, DBusMessageIter& properties) {
        dbus_message_iter_open_container(&settings, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &name);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_ARRAY, "{sv}", &properties);
    };
    auto close_section = [&](DBusMessageIter& entry, DBusMessageIter& properties) {
        dbus_message_iter_close_container(&entry, &properties);
        dbus_message_iter_close_container(&settings, &entry);
    };

    open_section("connection", connection_entry, inner);
    append_string_entry(&inner, "id", profile_id);
    append_string_entry(&inner, "type", type);
    append_bool_entry(&inner, "autoconnect", true);
    close_section(connection_entry, inner);

    open_section("802-11-wireless", connection_entry, inner);
    append_bytes_entry(&inner, "ssid", network.ssid);
    append_string_entry(&inner, "mode", mode);
    close_section(connection_entry, inner);

    if (psk.has_value() && !psk->is_empty()) {
        open_section("802-11-wireless-security", security_entry, security);
        append_string_entry(&security, "key-mgmt", key_mgmt);
        append_string_entry(&security, "psk", psk->characters());
        close_section(security_entry, security);
    }

    dbus_message_iter_close_container(&iter, &settings);

    char const* device_path = network.device_path.characters();
    char const* specific = network.access_point_path.characters();
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &device_path);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &specific);

    if (auto* reply = send_and_block(m_connection, message))
        dbus_message_unref(reply);
    schedule_refresh();
}

void NetworkManagerClient::disconnect(WifiNetwork const& network)
{
    if (!m_connection)
        return;
    auto* message = new_call(network.device_path.characters(), device_iface, "Disconnect");
    if (auto* reply = send_and_block(m_connection, message))
        dbus_message_unref(reply);
    schedule_refresh();
}

void NetworkManagerClient::request_scan()
{
    if (!m_connection)
        return;
    // Scan every Wi-Fi device we know about (one scan refreshes all of a
    // device's access points).
    for (auto const& network : m_networks) {
        if (network.device_path.is_empty())
            continue;
        auto* message = new_call(network.device_path.characters(), wireless_iface, "RequestScan");
        DBusMessageIter iter, options;
        dbus_message_iter_init_append(message, &iter);
        dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &options);
        dbus_message_iter_close_container(&iter, &options);
        if (auto* reply = send_and_block(m_connection, message))
            dbus_message_unref(reply);
        break;
    }
}

void NetworkManagerClient::refresh()
{
    if (!m_connection) {
        m_state.available = false;
        if (on_changed)
            on_changed();
        return;
    }

    NetworkState state;
    state.available = true;

    // Manager properties.
    if (auto* reply = get_all(m_connection, nm_path, nm_iface)) {
        DBusMessageIter iter;
        if (dbus_message_iter_init(reply, &iter)) {
            dict_entry(&iter, "WirelessEnabled", [&](DBusMessageIter* v) { state.wireless_enabled = variant_bool(v, true); });
            dict_entry(&iter, "WirelessHardwareEnabled", [&](DBusMessageIter* v) { state.wireless_hardware_enabled = variant_bool(v, true); });
            dict_entry(&iter, "NetworkingEnabled", [&](DBusMessageIter* v) { state.networking_enabled = variant_bool(v, true); });
            dict_entry(&iter, "Connectivity", [&](DBusMessageIter* v) { state.connectivity = static_cast<int>(variant_u32(v)); });
        }
        dbus_message_unref(reply);
    }

    // Saved connections (to know an SSID is known and to reuse its profile).
    HashMap<ByteString, ByteString> saved_connections;
    if (auto* reply = send_and_block(m_connection, new_call(settings_path, settings_iface, "ListConnections"))) {
        for (auto const& path : parse_object_path_array(reply)) {
            auto* settings_reply = send_and_block(m_connection, new_call(path.characters(), connection_iface, "GetSettings"));
            if (auto connection = parse_connection_settings(settings_reply); connection.has_value() && connection->wireless && !connection->ssid.is_empty())
                saved_connections.set(connection->ssid, path);
            if (settings_reply)
                dbus_message_unref(settings_reply);
        }
        dbus_message_unref(reply);
    }

    Vector<WifiNetwork> networks;
    bool connecting = false;
    bool active_found = false;

    // Devices: find the Wi-Fi ones and their access points.
    Vector<ByteString> device_paths;
    if (auto* reply = get_all(m_connection, nm_path, nm_iface)) {
        DBusMessageIter iter;
        if (dbus_message_iter_init(reply, &iter))
            dict_entry(&iter, "Devices", [&](DBusMessageIter* v) { device_paths = variant_object_paths(v); });
        dbus_message_unref(reply);
    }

    for (auto const& device_path : device_paths) {
        unsigned device_type = 0;
        ByteString active_access_point;
        Vector<ByteString> access_points;

        if (auto* reply = get_all(m_connection, device_path.characters(), device_iface)) {
            DBusMessageIter iter;
            if (dbus_message_iter_init(reply, &iter))
                dict_entry(&iter, "DeviceType", [&](DBusMessageIter* v) { device_type = variant_u32(v); });
            dbus_message_unref(reply);
        }
        if (device_type != device_type_wifi)
            continue;
        if (state.wifi_device_path.is_empty())
            state.wifi_device_path = device_path;

        if (auto* reply = get_all(m_connection, device_path.characters(), wireless_iface)) {
            DBusMessageIter iter;
            if (dbus_message_iter_init(reply, &iter)) {
                dict_entry(&iter, "AccessPoints", [&](DBusMessageIter* v) { access_points = variant_object_paths(v); });
                dict_entry(&iter, "ActiveAccessPoint", [&](DBusMessageIter* v) { active_access_point = variant_string(v); });
            }
            dbus_message_unref(reply);
        }

        // Is a connection activating on this device?
        if (auto* reply = get_all(m_connection, device_path.characters(), device_iface)) {
            DBusMessageIter iter;
            if (dbus_message_iter_init(reply, &iter))
                dict_entry(&iter, "State", [&](DBusMessageIter* v) {
                    unsigned device_state = variant_u32(v);
                    // NM_DEVICE_STATE_PREPARE..IP_CHECK all mean "coming up".
                    if (device_state >= 40 && device_state <= 90)
                        connecting = true;
                });
            dbus_message_unref(reply);
        }

        bool active_ap_has_ssid = false;
        for (auto const& ap_path : access_points) {
            WifiNetwork network;
            network.access_point_path = ap_path;
            network.device_path = device_path;
            bool privacy = false;
            unsigned wpa = 0;
            unsigned rsn = 0;

            if (auto* reply = get_all(m_connection, ap_path.characters(), ap_iface)) {
                DBusMessageIter iter;
                if (dbus_message_iter_init(reply, &iter)) {
                    dict_entry(&iter, "Ssid", [&](DBusMessageIter* v) { network.ssid = variant_bytes(v); });
                    dict_entry(&iter, "Strength", [&](DBusMessageIter* v) { network.strength = static_cast<int>(variant_u32(v)); });
                    dict_entry(&iter, "Frequency", [&](DBusMessageIter* v) { network.frequency = static_cast<int>(variant_u32(v)); });
                    dict_entry(&iter, "Flags", [&](DBusMessageIter* v) { privacy = (variant_u32(v) & ap_flag_privacy) != 0; });
                    dict_entry(&iter, "WpaFlags", [&](DBusMessageIter* v) { wpa = variant_u32(v); });
                    dict_entry(&iter, "RsnFlags", [&](DBusMessageIter* v) { rsn = variant_u32(v); });
                }
                dbus_message_unref(reply);
            }

            if (network.ssid.is_empty())
                continue;
            network.secured = privacy || wpa != 0 || rsn != 0;
            network.connected = !active_access_point.is_empty() && ap_path == active_access_point;
            if (network.connected) {
                active_found = true;
                active_ap_has_ssid = true;
                state.active_ssid = network.ssid;
                state.active_strength = network.strength;
            }
            network.saved = saved_connections.contains(network.ssid);
            if (network.saved)
                network.saved_connection_path = saved_connections.get(network.ssid).value();

            // Collapse duplicate SSIDs, keeping the strongest access point.
            bool merged = false;
            for (auto& existing : networks) {
                if (existing.ssid == network.ssid) {
                    if (network.strength > existing.strength)
                        existing = network;
                    merged = true;
                    break;
                }
            }
            if (!merged)
                networks.append(move(network));
        }
        (void)active_ap_has_ssid;
    }
    (void)active_found;
    state.connecting = connecting;

    // Connected network first, then by signal strength.
    quick_sort(networks.begin(), networks.end(), [](WifiNetwork const& a, WifiNetwork const& b) {
        if (a.connected != b.connected)
            return a.connected;
        return a.strength > b.strength;
    });

    bool changed = m_networks.size() != networks.size()
        || m_state.active_ssid != state.active_ssid
        || m_state.wireless_enabled != state.wireless_enabled
        || m_state.wireless_hardware_enabled != state.wireless_hardware_enabled
        || m_state.networking_enabled != state.networking_enabled
        || m_state.connecting != state.connecting
        || m_state.active_strength != state.active_strength;
    if (!changed) {
        for (size_t i = 0; i < networks.size(); ++i) {
            if (m_networks[i].ssid != networks[i].ssid || m_networks[i].strength != networks[i].strength
                || m_networks[i].connected != networks[i].connected || m_networks[i].saved != networks[i].saved) {
                changed = true;
                break;
            }
        }
    }

    m_state = state;
    m_networks = move(networks);

    if (changed) {
        if (m_state.active_ssid.is_empty())
            dbgln("WifiApplet: {} networks, not connected (wireless {})", m_networks.size(), m_state.wireless_enabled ? "on" : "off");
        else
            dbgln("WifiApplet: {} networks, connected to {} ({}%)", m_networks.size(), ssid_to_string(m_state.active_ssid), m_state.active_strength);
    }

    if (changed && on_changed)
        on_changed();
}

void NetworkManagerClient::handle_secret_agent_call(DBusMessage* message)
{
    auto const* member = dbus_message_get_member(message);
    if (!member)
        return;

    if (strcmp(member, "GetSecrets") == 0) {
        // Parse the SSID out of the connection settings so the prompt can name
        // the network.
        ByteString ssid;
        DBusMessageIter iter;
        if (dbus_message_iter_init(message, &iter) && dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_ARRAY) {
            DBusMessageIter dict;
            dbus_message_iter_recurse(&iter, &dict);
            while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
                DBusMessageIter entry;
                dbus_message_iter_recurse(&dict, &entry);
                char const* section = nullptr;
                dbus_message_iter_get_basic(&entry, &section);
                dbus_message_iter_next(&entry);
                if (section && strcmp(section, "802-11-wireless") == 0) {
                    DBusMessageIter value;
                    dbus_message_iter_recurse(&entry, &value);
                    if (dbus_message_iter_get_arg_type(&value) == DBUS_TYPE_ARRAY)
                        dict_entry(&value, "ssid", [&](DBusMessageIter* v) { ssid = variant_bytes(v); });
                }
                dbus_message_iter_next(&dict);
            }
        }

        if (m_pending_secret_reply)
            dbus_message_unref(m_pending_secret_reply);
        m_pending_secret_reply = dbus_message_new_method_return(message);
        m_pending_secret_ssid = ssid;

        auto* client = this;
        m_pending_secret_callback = [client](Optional<ByteString> passphrase) {
            client->complete_secret_request(move(passphrase));
        };
        if (on_password_required) {
            auto callback = move(m_pending_secret_callback);
            on_password_required(ssid, move(callback));
        } else {
            complete_secret_request({});
        }
        return;
    }

    if (strcmp(member, "CancelGetSecrets") == 0 || strcmp(member, "DeleteSecrets") == 0 || strcmp(member, "SaveSecrets") == 0) {
        if (strcmp(member, "CancelGetSecrets") == 0 && m_pending_secret_reply) {
            dbus_message_unref(m_pending_secret_reply);
            m_pending_secret_reply = nullptr;
            m_pending_secret_callback = nullptr;
        }
        if (auto* reply = dbus_message_new_method_return(message)) {
            dbus_connection_send(m_connection, reply, nullptr);
            dbus_message_unref(reply);
        }
    }
}

void NetworkManagerClient::complete_secret_request(Optional<ByteString> passphrase)
{
    m_pending_secret_callback = nullptr;
    if (!m_pending_secret_reply || !m_connection)
        return;

    auto* reply = m_pending_secret_reply;
    m_pending_secret_reply = nullptr;

    if (passphrase.has_value() && !passphrase->is_empty()) {
        DBusMessageIter iter, settings, entry, properties;
        dbus_message_iter_init_append(reply, &iter);
        dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sa{sv}}", &settings);
        dbus_message_iter_open_container(&settings, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
        char const* security = "802-11-wireless-security";
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &security);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_ARRAY, "{sv}", &properties);
        append_string_entry(&properties, "psk", passphrase->characters());
        dbus_message_iter_close_container(&entry, &properties);
        dbus_message_iter_close_container(&settings, &entry);
        dbus_message_iter_close_container(&iter, &settings);
    }

    dbus_connection_send(m_connection, reply, nullptr);
    dbus_connection_flush(m_connection);
    dbus_message_unref(reply);
    schedule_refresh();
}
