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
#include <LibCore/Notifier.h>

#include <dbus/dbus.h>
#include <stdlib.h>
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

// How long one NetworkManager call may take before it counts as failed.
// NetworkManager answers in milliseconds when it is healthy; this is the ceiling
// for a stuck one. Nothing waits for it: the call simply completes with no reply.
constexpr int networkmanager_call_timeout_ms = 2000;

// How long to wait between attempts to reach a system bus that is not there.
constexpr int reconnect_interval_ms = 2000;

// Messages dispatched per turn of the event loop, so a flood of signals cannot
// keep the applet from painting.
constexpr int dispatch_batch = 64;

DBusMessage* new_call(char const* path, char const* interface, char const* method)
{
    return dbus_message_new_method_call(nm_name, path, interface, method);
}

DBusMessage* new_get_all(char const* path, char const* interface)
{
    auto* message = dbus_message_new_method_call(nm_name, path, props_iface, "GetAll");
    DBusMessageIter iter;
    dbus_message_iter_init_append(message, &iter);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &interface);
    return message;
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

// The 802-11-wireless SSID in a connection's a{sa{sv}} settings, with `iter`
// pointing at that array.
ByteString settings_ssid(DBusMessageIter* iter)
{
    ByteString ssid;
    if (dbus_message_iter_get_arg_type(iter) != DBUS_TYPE_ARRAY)
        return ssid;
    DBusMessageIter dict;
    dbus_message_iter_recurse(iter, &dict);
    while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry;
        dbus_message_iter_recurse(&dict, &entry);
        char const* section = nullptr;
        dbus_message_iter_get_basic(&entry, &section);
        dbus_message_iter_next(&entry);
        if (section && strcmp(section, "802-11-wireless") == 0 && dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_ARRAY)
            dict_entry(&entry, "ssid", [&](DBusMessageIter* variant) { ssid = variant_bytes(variant); });
        dbus_message_iter_next(&dict);
    }
    return ssid;
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
        // `entry` now points at the section's a{sv} (a{sa{sv}} has no variant
        // between a section's name and its properties).
        if (section && strcmp(section, "802-11-wireless") == 0) {
            result.wireless = true;
            if (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_ARRAY)
                dict_entry(&entry, "ssid", [&](DBusMessageIter* variant) { result.ssid = variant_bytes(variant); });
        }
        dbus_message_iter_next(&dict);
    }
    if (result.ssid.is_empty())
        return {};
    return result;
}

}


// One outstanding call: what to run when its reply (or its failure) arrives.
struct NetworkManagerClient::PendingCall {
    NetworkManagerClient* client { nullptr };
    DBusPendingCall* pending { nullptr };
    ReplyHandler handler;
};

// One refresh in flight. Its calls run in parallel, each filling in its own slot,
// and the last one to answer assembles the result. Slots are sized before the
// calls that fill them are made and never resized after, so a handler's index
// stays valid.
struct NetworkManagerClient::Refresh : RefCounted<Refresh> {
    struct Device {
        ByteString path;
        unsigned type { 0 };
        unsigned state { 0 };
        ByteString active_access_point;
        Vector<ByteString> access_points;
        Vector<Optional<WifiNetwork>> networks; // one per access point
    };

    u64 generation { 0 };
    size_t outstanding { 0 };
    bool manager_answered { false };
    NetworkState state;
    Vector<Device> devices;
    Vector<ByteString> connection_paths;
    Vector<ByteString> connection_ssids; // one per saved connection; empty if not Wi-Fi
};

namespace {

// libdbus's watches and timeouts, on LibCore's event loop. libdbus may remove a
// watch or timeout from inside our own callback (handling a read can find the
// socket closed), so removal only detaches it; the notifier or timer is freed on
// a later turn of the loop.
struct WatchBinding {
    DBusWatch* watch { nullptr };
    RefPtr<Core::Notifier> notifier;
    Function<void()> after_handle;
};

struct TimeoutBinding {
    DBusTimeout* timeout { nullptr };
    RefPtr<Core::Timer> timer;
    Function<void()> after_handle;
};

template<typename Binding>
void release_later(Binding* binding)
{
    Core::deferred_invoke([binding] { delete binding; });
}

bool is_expected_absence(char const* error_name)
{
    // NetworkManager is not running: the state shows it, logging it every poll
    // would only fill the journal.
    return error_name && (strcmp(error_name, DBUS_ERROR_SERVICE_UNKNOWN) == 0 || strcmp(error_name, DBUS_ERROR_NAME_HAS_NO_OWNER) == 0);
}

}

NetworkManagerClient::NetworkManagerClient()
{
    connect_to_bus();
}

NetworkManagerClient::~NetworkManagerClient()
{
    disconnect_from_bus();
}

void NetworkManagerClient::connect_to_bus()
{
    // A private connection, so that losing it, closing it and opening a new one
    // is entirely ours (a shared one outlives us inside libdbus).
    auto const* environment_address = getenv("DBUS_SYSTEM_BUS_ADDRESS");
    ByteString address = environment_address && *environment_address ? environment_address : "unix:path=/var/run/dbus/system_bus_socket";

    DBusError error;
    dbus_error_init(&error);
    m_connection = dbus_connection_open_private(address.characters(), &error);
    if (!m_connection) {
        if (!m_reconnect_timer)
            dbgln("WifiApplet: no system bus at {}: {}; retrying every {}s", address, error.message ? error.message : "unknown error", reconnect_interval_ms / 1000);
        dbus_error_free(&error);
        schedule_reconnect();
        return;
    }

    dbus_connection_set_exit_on_disconnect(m_connection, false);
    dbus_connection_add_filter(m_connection, &NetworkManagerClient::filter, this, nullptr);

    dbus_connection_set_watch_functions(
        m_connection,
        [](DBusWatch* watch, void* data) -> dbus_bool_t {
            auto* client = static_cast<NetworkManagerClient*>(data);
            unsigned flags = dbus_watch_get_flags(watch);
            auto type = Core::Notifier::Type::None;
            if (flags & DBUS_WATCH_READABLE)
                type |= Core::Notifier::Type::Read;
            if (flags & DBUS_WATCH_WRITABLE)
                type |= Core::Notifier::Type::Write;
            auto* binding = new WatchBinding;
            binding->watch = watch;
            binding->after_handle = [client] { client->schedule_dispatch(); };
            binding->notifier = Core::Notifier::construct(dbus_watch_get_unix_fd(watch), type);
            binding->notifier->on_activation = [binding, flags] {
                if (!binding->watch)
                    return;
                dbus_watch_handle(binding->watch, flags);
                binding->after_handle();
            };
            binding->notifier->set_enabled(dbus_watch_get_enabled(watch));
            dbus_watch_set_data(watch, binding, nullptr);
            return true;
        },
        [](DBusWatch* watch, void*) {
            auto* binding = static_cast<WatchBinding*>(dbus_watch_get_data(watch));
            if (!binding)
                return;
            dbus_watch_set_data(watch, nullptr, nullptr);
            binding->watch = nullptr;
            binding->notifier->set_enabled(false);
            release_later(binding);
        },
        [](DBusWatch* watch, void*) {
            if (auto* binding = static_cast<WatchBinding*>(dbus_watch_get_data(watch)))
                binding->notifier->set_enabled(dbus_watch_get_enabled(watch));
        },
        this, nullptr);

    dbus_connection_set_timeout_functions(
        m_connection,
        [](DBusTimeout* timeout, void* data) -> dbus_bool_t {
            auto* client = static_cast<NetworkManagerClient*>(data);
            auto* binding = new TimeoutBinding;
            binding->timeout = timeout;
            binding->after_handle = [client] { client->schedule_dispatch(); };
            binding->timer = Core::Timer::create_repeating(dbus_timeout_get_interval(timeout), [binding] {
                if (!binding->timeout)
                    return;
                dbus_timeout_handle(binding->timeout);
                binding->after_handle();
            });
            if (dbus_timeout_get_enabled(timeout))
                binding->timer->start();
            dbus_timeout_set_data(timeout, binding, nullptr);
            return true;
        },
        [](DBusTimeout* timeout, void*) {
            auto* binding = static_cast<TimeoutBinding*>(dbus_timeout_get_data(timeout));
            if (!binding)
                return;
            dbus_timeout_set_data(timeout, nullptr, nullptr);
            binding->timeout = nullptr;
            binding->timer->stop();
            release_later(binding);
        },
        [](DBusTimeout* timeout, void*) {
            auto* binding = static_cast<TimeoutBinding*>(dbus_timeout_get_data(timeout));
            if (!binding)
                return;
            if (dbus_timeout_get_enabled(timeout))
                binding->timer->restart(dbus_timeout_get_interval(timeout));
            else
                binding->timer->stop();
        },
        this, nullptr);

    dbus_connection_set_dispatch_status_function(
        m_connection,
        [](DBusConnection*, DBusDispatchStatus status, void* data) {
            if (status == DBUS_DISPATCH_DATA_REMAINS)
                static_cast<NetworkManagerClient*>(data)->schedule_dispatch();
        },
        this, nullptr);

    // Hello registers us with the bus. Asynchronously, like everything else: the
    // messages queued behind it are delivered in order once it is answered.
    call(dbus_message_new_method_call(DBUS_SERVICE_DBUS, DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS, "Hello"), [this](DBusMessage* reply) {
        if (!reply) {
            dbgln("WifiApplet: the system bus did not answer Hello");
            connection_lost();
            return;
        }
        m_registered_with_bus = true;
        on_connected();
    });
}

void NetworkManagerClient::on_connected()
{
    if (m_reconnect_timer)
        dbgln("WifiApplet: connected to the system bus");
    m_reconnect_timer = nullptr;

    // With no error argument, AddMatch does not wait for the bus's answer.
    dbus_bus_add_match(m_connection, "type='signal',sender='org.freedesktop.NetworkManager'", nullptr);
    // NetworkManager forgets its agents when it restarts, and the applet's data
    // goes stale when it stops: watch its name owner.
    dbus_bus_add_match(m_connection, "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='org.freedesktop.NetworkManager'", nullptr);

    auto* message = dbus_message_new_method_call(DBUS_SERVICE_DBUS, DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS, "GetNameOwner");
    char const* name = nm_name;
    dbus_message_append_args(message, DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID);
    call(message, [this](DBusMessage* reply) {
        char const* owner = nullptr;
        if (reply && dbus_message_get_args(reply, nullptr, DBUS_TYPE_STRING, &owner, DBUS_TYPE_INVALID) && owner)
            networkmanager_appeared(owner);
        else
            refresh();
    });
}

void NetworkManagerClient::networkmanager_appeared(ByteString owner)
{
    m_networkmanager_owner = move(owner);
    register_agent();
    schedule_refresh();
}

void NetworkManagerClient::networkmanager_vanished()
{
    dbgln("WifiApplet: NetworkManager went away");
    m_networkmanager_owner = {};
    // Its request for a passphrase went with it.
    if (m_pending_secret_request) {
        dbus_message_unref(m_pending_secret_request);
        m_pending_secret_request = nullptr;
        ++m_secret_request_id;
        if (on_password_request_cancelled)
            on_password_request_cancelled();
    }
    schedule_refresh();
}

void NetworkManagerClient::register_agent()
{
    // Register as a NetworkManager secret agent, so activating a saved WPA
    // network connects (NetworkManager asks us for the passphrase).
    auto* message = new_call(agent_manager_path, agent_manager_iface, "Register");
    char const* identifier = "org.serenityos.sde.WifiApplet";
    dbus_message_append_args(message, DBUS_TYPE_STRING, &identifier, DBUS_TYPE_INVALID);
    call(message, [](DBusMessage* reply) {
        if (!reply)
            dbgln("WifiApplet: could not register as a secret agent; saved networks that need a passphrase will not connect");
    });
}

void NetworkManagerClient::connection_lost()
{
    // Never tear the connection down from inside its own dispatch: do it on a
    // fresh turn of the loop.
    if (m_teardown_scheduled)
        return;
    m_teardown_scheduled = true;
    deferred_invoke([this] {
        m_teardown_scheduled = false;
        if (!m_connection)
            return;
        dbgln("WifiApplet: lost the system bus connection; reconnecting");
        disconnect_from_bus();
        publish({}, {});
        if (on_refresh_finished)
            on_refresh_finished();
        schedule_reconnect();
    });
}

void NetworkManagerClient::disconnect_from_bus()
{
    if (!m_connection)
        return;

    // Calls in flight die with the connection, and so does a refresh waiting on
    // them: the next connection starts a new one.
    cancel_pending_calls();
    ++m_generation;
    m_refresh_in_flight = false;
    m_refresh_again = false;

    if (m_pending_secret_request) {
        dbus_message_unref(m_pending_secret_request);
        m_pending_secret_request = nullptr;
        ++m_secret_request_id;
        if (on_password_request_cancelled)
            on_password_request_cancelled();
    }

    dbus_connection_remove_filter(m_connection, &NetworkManagerClient::filter, this);
    dbus_connection_set_dispatch_status_function(m_connection, nullptr, nullptr, nullptr);
    dbus_connection_set_watch_functions(m_connection, nullptr, nullptr, nullptr, nullptr, nullptr);
    dbus_connection_set_timeout_functions(m_connection, nullptr, nullptr, nullptr, nullptr, nullptr);
    dbus_connection_close(m_connection);
    dbus_connection_unref(m_connection);
    m_connection = nullptr;
    m_registered_with_bus = false;
    m_networkmanager_owner = {};
}

void NetworkManagerClient::schedule_reconnect()
{
    if (!m_reconnect_timer)
        m_reconnect_timer = Core::Timer::create_single_shot(reconnect_interval_ms, [this] { connect_to_bus(); });
    m_reconnect_timer->restart();
}

void NetworkManagerClient::schedule_dispatch()
{
    if (m_dispatch_scheduled)
        return;
    m_dispatch_scheduled = true;
    deferred_invoke([this] { dispatch(); });
}

void NetworkManagerClient::dispatch()
{
    m_dispatch_scheduled = false;
    if (!m_connection)
        return;
    for (int i = 0; i < dispatch_batch; ++i) {
        if (dbus_connection_dispatch(m_connection) != DBUS_DISPATCH_DATA_REMAINS)
            return;
    }
    // More is queued: let the UI have a turn first.
    schedule_dispatch();
}

DBusHandlerResult NetworkManagerClient::filter(DBusConnection*, DBusMessage* message, void* user_data)
{
    auto* client = static_cast<NetworkManagerClient*>(user_data);
    switch (dbus_message_get_type(message)) {
    case DBUS_MESSAGE_TYPE_METHOD_CALL:
        if (dbus_message_has_interface(message, secret_agent_iface)) {
            client->handle_secret_agent_call(message);
            return DBUS_HANDLER_RESULT_HANDLED;
        }
        break;
    case DBUS_MESSAGE_TYPE_SIGNAL:
        client->handle_signal(message);
        break;
    default:
        break;
    }
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

void NetworkManagerClient::handle_signal(DBusMessage* message)
{
    if (dbus_message_is_signal(message, DBUS_INTERFACE_LOCAL, "Disconnected")) {
        connection_lost();
        return;
    }

    if (dbus_message_is_signal(message, DBUS_INTERFACE_DBUS, "NameOwnerChanged")) {
        char const* name = nullptr;
        char const* old_owner = nullptr;
        char const* new_owner = nullptr;
        if (!dbus_message_get_args(message, nullptr, DBUS_TYPE_STRING, &name, DBUS_TYPE_STRING, &old_owner, DBUS_TYPE_STRING, &new_owner, DBUS_TYPE_INVALID))
            return;
        if (!name || strcmp(name, nm_name) != 0)
            return;
        if (new_owner && *new_owner) {
            dbgln("WifiApplet: NetworkManager is running; registering and refreshing");
            networkmanager_appeared(new_owner);
        } else {
            networkmanager_vanished();
        }
        return;
    }

    // Anything else NetworkManager says may change what is shown.
    if (auto const* interface = dbus_message_get_interface(message)) {
        if (strcmp(interface, props_iface) == 0 || strncmp(interface, nm_iface, strlen(nm_iface)) == 0)
            schedule_refresh();
    }
}

void NetworkManagerClient::call(DBusMessage* message, ReplyHandler handler)
{
    auto fail_later = [this](ReplyHandler handler) {
        deferred_invoke([handler = move(handler)] { handler(nullptr); });
    };
    if (!message) {
        fail_later(move(handler));
        return;
    }
    if (!m_connection || !dbus_connection_get_is_connected(m_connection)) {
        dbus_message_unref(message);
        fail_later(move(handler));
        return;
    }

    DBusPendingCall* pending = nullptr;
    bool sent = dbus_connection_send_with_reply(m_connection, message, &pending, networkmanager_call_timeout_ms);
    dbus_message_unref(message);
    if (!sent || !pending) {
        fail_later(move(handler));
        return;
    }

    auto* pending_call = new PendingCall { this, pending, move(handler) };
    m_pending_calls.set(pending_call);
    dbus_pending_call_set_notify(
        pending,
        [](DBusPendingCall* pending, void* data) {
            auto* pending_call = static_cast<PendingCall*>(data);
            auto* client = pending_call->client;
            client->m_pending_calls.remove(pending_call);
            auto handler = move(pending_call->handler);
            auto* reply = dbus_pending_call_steal_reply(pending);
            // Frees pending_call (its free function), so nothing below touches it.
            dbus_pending_call_unref(pending);

            if (reply && dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR) {
                auto const* name = dbus_message_get_error_name(reply);
                if (!is_expected_absence(name)) {
                    char const* text = nullptr;
                    dbus_message_get_args(reply, nullptr, DBUS_TYPE_STRING, &text, DBUS_TYPE_INVALID);
                    dbgln("WifiApplet: NetworkManager call failed: {} ({})", name, text ? text : "");
                }
                dbus_message_unref(reply);
                reply = nullptr;
            }
            handler(reply);
            if (reply)
                dbus_message_unref(reply);
        },
        pending_call,
        [](void* data) { delete static_cast<PendingCall*>(data); });
}

void NetworkManagerClient::call_get_all(char const* path, char const* interface, ReplyHandler handler)
{
    call(new_get_all(path, interface), move(handler));
}

void NetworkManagerClient::cancel_pending_calls()
{
    auto calls = move(m_pending_calls);
    for (auto* pending_call : calls) {
        auto* pending = pending_call->pending;
        dbus_pending_call_cancel(pending);
        // Our reference was the last one: this frees pending_call and its handler,
        // which is never run.
        dbus_pending_call_unref(pending);
    }
}

void NetworkManagerClient::action(DBusMessage* message)
{
    // The outcome shows up in NetworkManager's state, so the reply only prompts
    // a refresh; a failure has been logged by call().
    call(message, [this](DBusMessage*) { schedule_refresh(); });
}

void NetworkManagerClient::schedule_refresh()
{
    if (m_refresh_scheduled)
        return;
    m_refresh_scheduled = true;
    deferred_invoke([this] {
        m_refresh_scheduled = false;
        refresh();
    });
}

void NetworkManagerClient::set_wireless_enabled(bool enabled)
{
    if (!m_registered_with_bus)
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
    action(message);
}

void NetworkManagerClient::connect_to(WifiNetwork const& network, Optional<ByteString> psk)
{
    if (!m_registered_with_bus)
        return;

    // NetworkManager wants "/" for "any device" and "no particular access point".
    auto object_path_or_root = [](ByteString const& path) { return path.is_empty() ? "/" : path.characters(); };

    if (network.saved && !network.saved_connection_path.is_empty()) {
        // Activate the saved connection; our secret agent supplies the key if
        // NetworkManager needs one.
        auto* message = new_call(nm_path, nm_iface, "ActivateConnection");
        char const* connection_path = network.saved_connection_path.characters();
        char const* device_path = object_path_or_root(network.device_path);
        char const* specific = object_path_or_root(network.access_point_path);
        dbus_message_append_args(message, DBUS_TYPE_OBJECT_PATH, &connection_path, DBUS_TYPE_OBJECT_PATH, &device_path, DBUS_TYPE_OBJECT_PATH, &specific, DBUS_TYPE_INVALID);
        action(message);
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

    char const* device_path = object_path_or_root(network.device_path.is_empty() ? m_state.wifi_device_path : network.device_path);
    char const* specific = object_path_or_root(network.access_point_path);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &device_path);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &specific);
    action(message);
}

void NetworkManagerClient::disconnect(WifiNetwork const& network)
{
    if (!m_registered_with_bus)
        return;
    auto const& device_path = network.device_path.is_empty() ? m_state.wifi_device_path : network.device_path;
    if (device_path.is_empty())
        return;
    action(new_call(device_path.characters(), device_iface, "Disconnect"));
}

void NetworkManagerClient::request_scan()
{
    // One scan refreshes all of a device's access points. Asking for one with the
    // radio off only earns an error.
    if (!m_registered_with_bus || m_state.wifi_device_path.is_empty() || !m_state.wireless_enabled)
        return;
    auto* message = new_call(m_state.wifi_device_path.characters(), wireless_iface, "RequestScan");
    DBusMessageIter iter, options;
    dbus_message_iter_init_append(message, &iter);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &options);
    dbus_message_iter_close_container(&iter, &options);
    action(message);
}

void NetworkManagerClient::call_for_refresh(NonnullRefPtr<Refresh> const& refresh, DBusMessage* message, ReplyHandler handler)
{
    ++refresh->outstanding;
    call(message, [this, refresh, handler = move(handler)](DBusMessage* reply) {
        // A refresh begun on a connection that has since been replaced.
        if (refresh->generation != m_generation)
            return;
        handler(reply);
        release(refresh);
    });
}

void NetworkManagerClient::release(NonnullRefPtr<Refresh> const& refresh)
{
    VERIFY(refresh->outstanding > 0);
    if (--refresh->outstanding == 0)
        finish_refresh(*refresh);
}

void NetworkManagerClient::refresh()
{
    if (!m_connection) {
        publish({}, {});
        if (on_refresh_finished)
            on_refresh_finished();
        return;
    }
    // Still saying Hello: the refresh follows once the bus has answered.
    if (!m_registered_with_bus)
        return;
    // One refresh at a time. A request that arrives meanwhile is remembered and
    // served by exactly one more refresh, so the last state wins without a stream
    // of signals restarting the refresh forever.
    if (m_refresh_in_flight) {
        m_refresh_again = true;
        return;
    }
    m_refresh_in_flight = true;
    m_refresh_again = false;

    auto refresh = adopt_ref(*new Refresh);
    refresh->generation = m_generation;
    // Held while the calls are issued, so an early answer cannot finish it.
    ++refresh->outstanding;

    // Manager properties and devices, then each device, then each Wi-Fi device's
    // access points.
    call_for_refresh(refresh, new_get_all(nm_path, nm_iface), [this, refresh](DBusMessage* reply) {
        if (!reply)
            return;
        refresh->manager_answered = true;
        auto& state = refresh->state;
        Vector<ByteString> device_paths;
        DBusMessageIter iter;
        if (dbus_message_iter_init(reply, &iter)) {
            dict_entry(&iter, "WirelessEnabled", [&](DBusMessageIter* v) { state.wireless_enabled = variant_bool(v, true); });
            dict_entry(&iter, "WirelessHardwareEnabled", [&](DBusMessageIter* v) { state.wireless_hardware_enabled = variant_bool(v, true); });
            dict_entry(&iter, "NetworkingEnabled", [&](DBusMessageIter* v) { state.networking_enabled = variant_bool(v, true); });
            dict_entry(&iter, "Connectivity", [&](DBusMessageIter* v) { state.connectivity = static_cast<int>(variant_u32(v)); });
            dict_entry(&iter, "Devices", [&](DBusMessageIter* v) { device_paths = variant_object_paths(v); });
        }

        refresh->devices.resize(device_paths.size());
        for (size_t i = 0; i < device_paths.size(); ++i) {
            refresh->devices[i].path = device_paths[i];
            call_for_refresh(refresh, new_get_all(device_paths[i].characters(), device_iface), [this, refresh, i](DBusMessage* reply) {
                if (!reply)
                    return;
                auto& device = refresh->devices[i];
                DBusMessageIter iter;
                if (dbus_message_iter_init(reply, &iter)) {
                    dict_entry(&iter, "DeviceType", [&](DBusMessageIter* v) { device.type = variant_u32(v); });
                    dict_entry(&iter, "State", [&](DBusMessageIter* v) { device.state = variant_u32(v); });
                }
                if (device.type != device_type_wifi)
                    return;

                call_for_refresh(refresh, new_get_all(device.path.characters(), wireless_iface), [this, refresh, i](DBusMessage* reply) {
                    if (!reply)
                        return;
                    auto& device = refresh->devices[i];
                    DBusMessageIter iter;
                    if (dbus_message_iter_init(reply, &iter)) {
                        dict_entry(&iter, "AccessPoints", [&](DBusMessageIter* v) { device.access_points = variant_object_paths(v); });
                        dict_entry(&iter, "ActiveAccessPoint", [&](DBusMessageIter* v) { device.active_access_point = variant_string(v); });
                    }
                    device.networks.resize(device.access_points.size());
                    for (size_t j = 0; j < device.access_points.size(); ++j) {
                        call_for_refresh(refresh, new_get_all(device.access_points[j].characters(), ap_iface), [refresh, i, j](DBusMessage* reply) {
                            if (!reply)
                                return;
                            WifiNetwork network;
                            bool privacy = false;
                            unsigned wpa = 0;
                            unsigned rsn = 0;
                            DBusMessageIter iter;
                            if (dbus_message_iter_init(reply, &iter)) {
                                dict_entry(&iter, "Ssid", [&](DBusMessageIter* v) { network.ssid = variant_bytes(v); });
                                dict_entry(&iter, "Strength", [&](DBusMessageIter* v) { network.strength = static_cast<int>(variant_u32(v)); });
                                dict_entry(&iter, "Frequency", [&](DBusMessageIter* v) { network.frequency = static_cast<int>(variant_u32(v)); });
                                dict_entry(&iter, "Flags", [&](DBusMessageIter* v) { privacy = (variant_u32(v) & ap_flag_privacy) != 0; });
                                dict_entry(&iter, "WpaFlags", [&](DBusMessageIter* v) { wpa = variant_u32(v); });
                                dict_entry(&iter, "RsnFlags", [&](DBusMessageIter* v) { rsn = variant_u32(v); });
                            }
                            network.secured = privacy || wpa != 0 || rsn != 0;
                            refresh->devices[i].networks[j] = move(network);
                        });
                    }
                });
            });
        }
    });

    // Saved connections, to know an SSID is known and to reuse its profile.
    call_for_refresh(refresh, new_call(settings_path, settings_iface, "ListConnections"), [this, refresh](DBusMessage* reply) {
        if (!reply)
            return;
        refresh->connection_paths = parse_object_path_array(reply);
        refresh->connection_ssids.resize(refresh->connection_paths.size());
        for (size_t k = 0; k < refresh->connection_paths.size(); ++k) {
            call_for_refresh(refresh, new_call(refresh->connection_paths[k].characters(), connection_iface, "GetSettings"), [refresh, k](DBusMessage* reply) {
                if (auto connection = parse_connection_settings(reply); connection.has_value() && connection->wireless)
                    refresh->connection_ssids[k] = connection->ssid;
            });
        }
    });

    release(refresh);
}

void NetworkManagerClient::finish_refresh(Refresh& refresh)
{
    m_refresh_in_flight = false;

    NetworkState state;
    Vector<WifiNetwork> networks;
    if (refresh.manager_answered) {
        state = refresh.state;
        state.available = true;

        HashMap<ByteString, ByteString> saved_connections;
        for (size_t k = 0; k < refresh.connection_paths.size(); ++k) {
            if (!refresh.connection_ssids[k].is_empty())
                saved_connections.set(refresh.connection_ssids[k], refresh.connection_paths[k]);
        }

        for (auto& device : refresh.devices) {
            if (device.type != device_type_wifi)
                continue;
            if (state.wifi_device_path.is_empty())
                state.wifi_device_path = device.path;
            // NM_DEVICE_STATE_PREPARE..IP_CHECK all mean "coming up".
            if (device.state >= 40 && device.state <= 90)
                state.connecting = true;

            for (size_t j = 0; j < device.access_points.size(); ++j) {
                if (!device.networks[j].has_value() || device.networks[j]->ssid.is_empty())
                    continue;
                auto network = device.networks[j].release_value();
                network.access_point_path = device.access_points[j];
                network.device_path = device.path;
                network.connected = !device.active_access_point.is_empty() && network.access_point_path == device.active_access_point;
                if (network.connected) {
                    state.active_ssid = network.ssid;
                    state.active_strength = network.strength;
                }
                if (auto saved = saved_connections.get(network.ssid); saved.has_value()) {
                    network.saved = true;
                    network.saved_connection_path = saved.value();
                }

                // One row per SSID: the access point we are connected to, else the
                // strongest.
                bool merged = false;
                for (auto& existing : networks) {
                    if (existing.ssid != network.ssid)
                        continue;
                    bool better = network.connected != existing.connected ? network.connected : network.strength > existing.strength;
                    if (better)
                        existing = network;
                    merged = true;
                    break;
                }
                if (!merged)
                    networks.append(move(network));
            }
        }

        // Connected network first, then by signal strength, then by name so equal
        // strengths do not trade places between refreshes.
        quick_sort(networks.begin(), networks.end(), [](WifiNetwork const& a, WifiNetwork const& b) {
            if (a.connected != b.connected)
                return a.connected;
            if (a.strength != b.strength)
                return a.strength > b.strength;
            return a.ssid < b.ssid;
        });
    }

    publish(move(state), move(networks));
    if (on_refresh_finished)
        on_refresh_finished();
    if (m_refresh_again) {
        m_refresh_again = false;
        schedule_refresh();
    }
}

void NetworkManagerClient::publish(NetworkState state, Vector<WifiNetwork> networks)
{
    bool changed = m_networks.size() != networks.size()
        || m_state.available != state.available
        || m_state.active_ssid != state.active_ssid
        || m_state.wireless_enabled != state.wireless_enabled
        || m_state.wireless_hardware_enabled != state.wireless_hardware_enabled
        || m_state.networking_enabled != state.networking_enabled
        || m_state.connecting != state.connecting
        || m_state.connectivity != state.connectivity
        || m_state.active_strength != state.active_strength
        || m_state.wifi_device_path != state.wifi_device_path;
    for (size_t i = 0; !changed && i < networks.size(); ++i) {
        auto const& a = m_networks[i];
        auto const& b = networks[i];
        changed = a.ssid != b.ssid || a.strength != b.strength || a.connected != b.connected || a.saved != b.saved || a.secured != b.secured;
    }

    m_state = move(state);
    m_networks = move(networks);
    if (!changed)
        return;

    if (!m_state.available)
        dbgln("WifiApplet: NetworkManager is not available");
    else if (m_state.active_ssid.is_empty())
        dbgln("WifiApplet: {} networks, not connected (wireless {})", m_networks.size(), m_state.wireless_enabled ? "on" : "off");
    else
        dbgln("WifiApplet: {} networks, connected to {} ({}%)", m_networks.size(), ssid_to_string(m_state.active_ssid), m_state.active_strength);

    if (on_changed)
        on_changed();
}

void NetworkManagerClient::send_reply(DBusMessage* reply)
{
    if (!reply)
        return;
    if (m_connection)
        dbus_connection_send(m_connection, reply, nullptr);
    dbus_message_unref(reply);
}

void NetworkManagerClient::handle_secret_agent_call(DBusMessage* message)
{
    // Secrets go to NetworkManager and nobody else.
    auto const* sender = dbus_message_get_sender(message);
    if (!sender || m_networkmanager_owner.is_empty() || m_networkmanager_owner != sender) {
        send_reply(dbus_message_new_error(message, "org.freedesktop.NetworkManager.SecretAgent.NotAuthorized", "Only NetworkManager may ask for secrets"));
        return;
    }

    if (dbus_message_is_method_call(message, secret_agent_iface, "GetSecrets")) {
        // GetSecrets(a{sa{sv}} connection, o connection_path, s setting_name, as hints, u flags)
        DBusMessageIter iter;
        ByteString ssid;
        char const* setting_name = nullptr;
        dbus_uint32_t flags = 0;
        if (dbus_message_iter_init(message, &iter)) {
            ssid = settings_ssid(&iter);
            if (dbus_message_iter_next(&iter) && dbus_message_iter_next(&iter) && dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_STRING)
                dbus_message_iter_get_basic(&iter, &setting_name);
            if (dbus_message_iter_next(&iter) && dbus_message_iter_next(&iter) && dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_UINT32)
                dbus_message_iter_get_basic(&iter, &flags);
        }

        constexpr dbus_uint32_t allow_interaction = 0x1;
        // We keep no secrets of our own, so all we can do is ask the user -- and
        // only when NetworkManager says the user may be asked (not, say, while it
        // autoconnects in the background).
        if (!setting_name || strcmp(setting_name, "802-11-wireless-security") != 0 || !(flags & allow_interaction) || !on_password_required) {
            send_reply(dbus_message_new_error(message, "org.freedesktop.NetworkManager.SecretAgent.NoSecrets", "No secrets available without asking the user"));
            return;
        }

        // A newer request replaces an older one.
        abandon_secret_request("org.freedesktop.NetworkManager.SecretAgent.AgentCanceled", "Superseded by a newer request");
        m_pending_secret_request = dbus_message_ref(message);
        auto request_id = ++m_secret_request_id;
        auto weak_client = make_weak_ptr<NetworkManagerClient>();
        on_password_required(ssid, [weak_client, request_id](Optional<ByteString> passphrase) {
            if (weak_client)
                weak_client->complete_secret_request(request_id, move(passphrase));
        });
        return;
    }

    if (dbus_message_is_method_call(message, secret_agent_iface, "CancelGetSecrets")) {
        if (m_pending_secret_request) {
            abandon_secret_request("org.freedesktop.NetworkManager.SecretAgent.AgentCanceled", "Canceled by NetworkManager");
            if (on_password_request_cancelled)
                on_password_request_cancelled();
        }
        send_reply(dbus_message_new_method_return(message));
        return;
    }

    if (dbus_message_is_method_call(message, secret_agent_iface, "SaveSecrets") || dbus_message_is_method_call(message, secret_agent_iface, "DeleteSecrets")) {
        // Nothing is stored here, so there is nothing to save or delete.
        send_reply(dbus_message_new_method_return(message));
        return;
    }

    send_reply(dbus_message_new_error(message, DBUS_ERROR_UNKNOWN_METHOD, "Unknown SecretAgent method"));
}

void NetworkManagerClient::abandon_secret_request(char const* error_name, char const* error_message)
{
    if (!m_pending_secret_request)
        return;
    send_reply(dbus_message_new_error(m_pending_secret_request, error_name, error_message));
    dbus_message_unref(m_pending_secret_request);
    m_pending_secret_request = nullptr;
    ++m_secret_request_id;
}

void NetworkManagerClient::complete_secret_request(u64 request_id, Optional<ByteString> passphrase)
{
    // The UI answered a request NetworkManager has since withdrawn or replaced.
    if (request_id != m_secret_request_id || !m_pending_secret_request)
        return;

    if (!passphrase.has_value() || passphrase->is_empty()) {
        abandon_secret_request("org.freedesktop.NetworkManager.SecretAgent.UserCanceled", "The user canceled");
        return;
    }

    auto* reply = dbus_message_new_method_return(m_pending_secret_request);
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

    send_reply(reply);
    dbus_message_unref(m_pending_secret_request);
    m_pending_secret_request = nullptr;
    ++m_secret_request_id;
    schedule_refresh();
}
