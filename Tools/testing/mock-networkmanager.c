/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// mock-networkmanager — a minimal stand-in for NetworkManager's D-Bus API, so
// the Wi-Fi applet can be tested deterministically (a private bus, fixed
// networks) without a real NetworkManager or radio. It serves what the applet
// reads (Properties.GetAll on the manager, the Wi-Fi device and its access
// points, Settings.ListConnections and GetSettings) and keeps just enough state
// for the applet's actions to have visible effects:
//
//   * Properties.Set WirelessEnabled turns the radio off (no access points) or on;
//   * Device.Disconnect drops the active access point;
//   * Device.Wireless.RequestScan finds a third network, "ScanFound";
//   * ActivateConnection on the saved MockNet profile asks the registered secret
//     agent for the passphrase (GetSecrets), as NetworkManager does, and logs the
//     answer;
//
// and emits PropertiesChanged after each change. It owns the
// org.freedesktop.NetworkManager name on whatever bus DBUS_SYSTEM_BUS_ADDRESS
// points at, and logs every non-property call to stderr for the tests.
//
// Fault injection, for the applet's robustness tests:
//   --delay-ms N    answer every call N ms late (a slow NetworkManager)
//   --hang-ap       never answer GetAll on the second access point (a stuck one)
//   --chatter-ms N  emit a PropertiesChanged signal every N ms (a busy one)
//
// --extra-aps adds four more networks, one of each remaining security type
// (Enterprise, Open, Enhanced Open, WEP), for looking at the popup.
//
// Security: MockNet and OpenGuest are WPA2, ScanFound is WPA3-only (SAE). The
// saved MockNet profile can be updated (its autoconnect) and deleted; both are
// logged, the update with what the profile still contained.
//
// Usage: mock-networkmanager [--delay-ms N] [--hang-ap] [--chatter-ms N] [--extra-aps]

#include <dbus/dbus.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define NM_NAME "org.freedesktop.NetworkManager"
#define MGR_PATH "/org/freedesktop/NetworkManager"
#define DEV_PATH "/org/freedesktop/NetworkManager/Devices/1"
#define AP1_PATH "/org/freedesktop/NetworkManager/AccessPoints/1"
#define AP2_PATH "/org/freedesktop/NetworkManager/AccessPoints/2"
#define AP3_PATH "/org/freedesktop/NetworkManager/AccessPoints/3"
#define AP4_PATH "/org/freedesktop/NetworkManager/AccessPoints/4"
#define AP5_PATH "/org/freedesktop/NetworkManager/AccessPoints/5"
#define AP6_PATH "/org/freedesktop/NetworkManager/AccessPoints/6"
#define AP7_PATH "/org/freedesktop/NetworkManager/AccessPoints/7"
#define SETTINGS_PATH "/org/freedesktop/NetworkManager/Settings"
#define SAVED_PATH "/org/freedesktop/NetworkManager/Settings/1"
#define AGENT_MGR_PATH "/org/freedesktop/NetworkManager/AgentManager"
#define AGENT_PATH "/org/freedesktop/NetworkManager/SecretAgent"

#define IFACE_NM "org.freedesktop.NetworkManager"
#define IFACE_DEV "org.freedesktop.NetworkManager.Device"
#define IFACE_WIFI "org.freedesktop.NetworkManager.Device.Wireless"
#define IFACE_AP "org.freedesktop.NetworkManager.AccessPoint"
#define IFACE_SETTINGS "org.freedesktop.NetworkManager.Settings"
#define IFACE_CONNECTION "org.freedesktop.NetworkManager.Settings.Connection"
#define IFACE_AGENT_MGR "org.freedesktop.NetworkManager.AgentManager"
#define IFACE_AGENT "org.freedesktop.NetworkManager.SecretAgent"
#define IFACE_PROPS "org.freedesktop.DBus.Properties"

static DBusConnection* s_connection;

static int s_delay_ms;
static int s_hang_ap;
static int s_chatter_ms;
static int s_extra_aps;
static int s_saved = 1;            // the MockNet profile exists
static int s_saved_autoconnect = 1;

static int s_wireless_enabled = 1;
static int s_scanned;              // RequestScan found AP3
static char const* s_active_ap = AP1_PATH; // NULL: not connected
static char s_agent[256];          // the secret agent's unique name

static void log_line(char const* format, char const* a, char const* b)
{
    fprintf(stderr, format, a, b);
    fputc('\n', stderr);
    fflush(stderr);
}

static unsigned device_state(void)
{
    if (!s_wireless_enabled)
        return 20; // UNAVAILABLE
    return s_active_ap ? 100 : 30; // ACTIVATED : DISCONNECTED
}

static void open_entry(DBusMessageIter* array, char const* key, char const* signature, DBusMessageIter* entry, DBusMessageIter* variant)
{
    dbus_message_iter_open_container(array, DBUS_TYPE_DICT_ENTRY, NULL, entry);
    dbus_message_iter_append_basic(entry, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(entry, DBUS_TYPE_VARIANT, signature, variant);
}

static void close_entry(DBusMessageIter* array, DBusMessageIter* entry, DBusMessageIter* variant)
{
    dbus_message_iter_close_container(entry, variant);
    dbus_message_iter_close_container(array, entry);
}

static void append_bool(DBusMessageIter* array, char const* key, int value)
{
    DBusMessageIter entry, variant;
    open_entry(array, key, "b", &entry, &variant);
    dbus_bool_t v = value ? TRUE : FALSE;
    dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &v);
    close_entry(array, &entry, &variant);
}

static void append_u32(DBusMessageIter* array, char const* key, unsigned value)
{
    DBusMessageIter entry, variant;
    open_entry(array, key, "u", &entry, &variant);
    dbus_uint32_t v = value;
    dbus_message_iter_append_basic(&variant, DBUS_TYPE_UINT32, &v);
    close_entry(array, &entry, &variant);
}

static void append_byte(DBusMessageIter* array, char const* key, unsigned value)
{
    DBusMessageIter entry, variant;
    open_entry(array, key, "y", &entry, &variant);
    unsigned char v = (unsigned char)value;
    dbus_message_iter_append_basic(&variant, DBUS_TYPE_BYTE, &v);
    close_entry(array, &entry, &variant);
}

static void append_string(DBusMessageIter* array, char const* key, char const* value)
{
    DBusMessageIter entry, variant;
    open_entry(array, key, "s", &entry, &variant);
    dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &value);
    close_entry(array, &entry, &variant);
}

static void append_object_path(DBusMessageIter* array, char const* key, char const* value)
{
    DBusMessageIter entry, variant;
    open_entry(array, key, "o", &entry, &variant);
    dbus_message_iter_append_basic(&variant, DBUS_TYPE_OBJECT_PATH, &value);
    close_entry(array, &entry, &variant);
}

static void append_object_paths(DBusMessageIter* array, char const* key, char const* const* values, int count)
{
    DBusMessageIter entry, variant, list;
    open_entry(array, key, "ao", &entry, &variant);
    dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY, "o", &list);
    for (int i = 0; i < count; ++i)
        dbus_message_iter_append_basic(&list, DBUS_TYPE_OBJECT_PATH, &values[i]);
    dbus_message_iter_close_container(&variant, &list);
    close_entry(array, &entry, &variant);
}

static void append_byte_array(DBusMessageIter* array, char const* key, char const* value)
{
    DBusMessageIter entry, variant, bytes;
    open_entry(array, key, "ay", &entry, &variant);
    dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY, "y", &bytes);
    for (char const* p = value; *p; ++p) {
        unsigned char byte = (unsigned char)*p;
        dbus_message_iter_append_basic(&bytes, DBUS_TYPE_BYTE, &byte);
    }
    dbus_message_iter_close_container(&variant, &bytes);
    close_entry(array, &entry, &variant);
}

// NM80211ApSecurityFlags.
#define SEC_PAIR_CCMP 0x8
#define SEC_KEY_MGMT_PSK 0x100
#define SEC_KEY_MGMT_8021X 0x200
#define SEC_KEY_MGMT_SAE 0x400
#define SEC_KEY_MGMT_OWE 0x800

static void append_access_point(DBusMessageIter* array, char const* ssid, unsigned strength, unsigned frequency, unsigned privacy, unsigned rsn)
{
    append_byte_array(array, "Ssid", ssid);
    append_byte(array, "Strength", strength);
    append_u32(array, "Frequency", frequency);
    append_u32(array, "Flags", privacy);
    append_u32(array, "WpaFlags", 0);
    append_u32(array, "RsnFlags", rsn);
    append_u32(array, "Mode", 2);
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
        append_bool(&array, "WirelessEnabled", s_wireless_enabled);
        append_bool(&array, "WirelessHardwareEnabled", 1);
        append_u32(&array, "Connectivity", s_active_ap ? 4 : 1);
        append_u32(&array, "State", s_active_ap ? 70 : 20);
        append_object_paths(&array, "Devices", devices, 1);
        append_object_paths(&array, "ActiveConnections", active, s_active_ap ? 1 : 0);
    } else if (!strcmp(path, DEV_PATH) && !strcmp(interface, IFACE_DEV)) {
        append_u32(&array, "DeviceType", 2); // NM_DEVICE_TYPE_WIFI
        append_u32(&array, "State", device_state());
        append_string(&array, "Interface", "wlan0");
        append_object_path(&array, "ActiveConnection", s_active_ap ? "/org/freedesktop/NetworkManager/ActiveConnection/1" : "/");
    } else if (!strcmp(path, DEV_PATH) && !strcmp(interface, IFACE_WIFI)) {
        char const* access_points[] = { AP1_PATH, AP2_PATH, AP3_PATH, AP4_PATH, AP5_PATH, AP6_PATH, AP7_PATH };
        int count = !s_wireless_enabled ? 0 : (s_extra_aps ? 7 : (s_scanned ? 3 : 2));
        append_object_paths(&array, "AccessPoints", access_points, count);
        append_object_path(&array, "ActiveAccessPoint", s_active_ap ? s_active_ap : "/");
        append_string(&array, "HwAddress", "00:11:22:33:44:55");
        append_u32(&array, "Mode", 2);
    } else if (!strcmp(path, AP1_PATH) && !strcmp(interface, IFACE_AP)) {
        append_access_point(&array, "MockNet", 82, 2412, 1, SEC_KEY_MGMT_PSK | SEC_PAIR_CCMP);
    } else if (!strcmp(path, AP2_PATH) && !strcmp(interface, IFACE_AP)) {
        append_access_point(&array, "OpenGuest", 45, 2437, 1, SEC_KEY_MGMT_PSK | SEC_PAIR_CCMP);
    } else if (!strcmp(path, AP3_PATH) && !strcmp(interface, IFACE_AP)) {
        append_access_point(&array, "ScanFound", 60, 5180, 1, SEC_KEY_MGMT_SAE | SEC_PAIR_CCMP);
    } else if (!strcmp(path, AP4_PATH) && !strcmp(interface, IFACE_AP)) {
        append_access_point(&array, "CorpNet", 70, 5240, 1, SEC_KEY_MGMT_8021X | SEC_PAIR_CCMP);
    } else if (!strcmp(path, AP5_PATH) && !strcmp(interface, IFACE_AP)) {
        append_access_point(&array, "CoffeeShop", 38, 2462, 0, 0);
    } else if (!strcmp(path, AP6_PATH) && !strcmp(interface, IFACE_AP)) {
        append_access_point(&array, "Library", 55, 5745, 1, SEC_KEY_MGMT_OWE | SEC_PAIR_CCMP);
    } else if (!strcmp(path, AP7_PATH) && !strcmp(interface, IFACE_AP)) {
        append_access_point(&array, "OldRouter", 20, 2422, 1, 0);
    }

    dbus_message_iter_close_container(&iter, &array);
    return reply;
}

// The saved MockNet profile, as GetSettings returns it (secrets left out).
static DBusMessage* build_settings_reply(DBusMessage* call)
{
    DBusMessage* reply = dbus_message_new_method_return(call);
    DBusMessageIter iter, settings, section, properties;
    dbus_message_iter_init_append(reply, &iter);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sa{sv}}", &settings);

    char const* connection = "connection";
    dbus_message_iter_open_container(&settings, DBUS_TYPE_DICT_ENTRY, NULL, &section);
    dbus_message_iter_append_basic(&section, DBUS_TYPE_STRING, &connection);
    dbus_message_iter_open_container(&section, DBUS_TYPE_ARRAY, "{sv}", &properties);
    append_string(&properties, "id", "MockNet");
    append_string(&properties, "type", "802-11-wireless");
    append_bool(&properties, "autoconnect", s_saved_autoconnect);
    dbus_message_iter_close_container(&section, &properties);
    dbus_message_iter_close_container(&settings, &section);

    char const* wireless = "802-11-wireless";
    dbus_message_iter_open_container(&settings, DBUS_TYPE_DICT_ENTRY, NULL, &section);
    dbus_message_iter_append_basic(&section, DBUS_TYPE_STRING, &wireless);
    dbus_message_iter_open_container(&section, DBUS_TYPE_ARRAY, "{sv}", &properties);
    append_byte_array(&properties, "ssid", "MockNet");
    append_string(&properties, "mode", "infrastructure");
    dbus_message_iter_close_container(&section, &properties);
    dbus_message_iter_close_container(&settings, &section);

    dbus_message_iter_close_container(&iter, &settings);
    return reply;
}

// PropertiesChanged on `path`, which is all the applet needs to know to refresh.
static void emit_changed(char const* path, char const* interface)
{
    DBusMessage* signal = dbus_message_new_signal(path, IFACE_PROPS, "PropertiesChanged");
    DBusMessageIter iter, changed, invalidated;
    dbus_message_iter_init_append(signal, &iter);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &interface);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &changed);
    append_u32(&changed, "State", device_state());
    dbus_message_iter_close_container(&iter, &changed);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "s", &invalidated);
    dbus_message_iter_close_container(&iter, &invalidated);
    dbus_connection_send(s_connection, signal, NULL);
    dbus_message_unref(signal);
}

static void state_changed(void)
{
    emit_changed(MGR_PATH, IFACE_NM);
    emit_changed(DEV_PATH, IFACE_DEV);
}

// The 802-11-wireless-security.psk in an a{sa{sv}} settings dictionary.
static char const* find_psk(DBusMessageIter* iter)
{
    char const* psk = NULL;
    DBusMessageIter settings;
    if (dbus_message_iter_get_arg_type(iter) != DBUS_TYPE_ARRAY)
        return NULL;
    dbus_message_iter_recurse(iter, &settings);
    while (dbus_message_iter_get_arg_type(&settings) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter section, properties;
        char const* name = NULL;
        dbus_message_iter_recurse(&settings, &section);
        dbus_message_iter_get_basic(&section, &name);
        dbus_message_iter_next(&section);
        dbus_message_iter_recurse(&section, &properties);
        while (name && !strcmp(name, "802-11-wireless-security") && dbus_message_iter_get_arg_type(&properties) == DBUS_TYPE_DICT_ENTRY) {
            DBusMessageIter property, variant;
            char const* key = NULL;
            dbus_message_iter_recurse(&properties, &property);
            dbus_message_iter_get_basic(&property, &key);
            dbus_message_iter_next(&property);
            dbus_message_iter_recurse(&property, &variant);
            if (key && !strcmp(key, "psk") && dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_STRING)
                dbus_message_iter_get_basic(&variant, &psk);
            dbus_message_iter_next(&properties);
        }
        dbus_message_iter_next(&settings);
    }
    return psk;
}

// The value of `section`.`key` in an a{sa{sv}} settings dictionary, as a
// variant iterator in *out. Returns 0 if it is not there.
static int settings_lookup(DBusMessageIter* iter, char const* wanted_section, char const* wanted_key, DBusMessageIter* out)
{
    DBusMessageIter settings;
    if (dbus_message_iter_get_arg_type(iter) != DBUS_TYPE_ARRAY)
        return 0;
    dbus_message_iter_recurse(iter, &settings);
    for (; dbus_message_iter_get_arg_type(&settings) == DBUS_TYPE_DICT_ENTRY; dbus_message_iter_next(&settings)) {
        DBusMessageIter section, properties;
        char const* name = NULL;
        dbus_message_iter_recurse(&settings, &section);
        dbus_message_iter_get_basic(&section, &name);
        if (!name || strcmp(name, wanted_section) != 0)
            continue;
        dbus_message_iter_next(&section);
        dbus_message_iter_recurse(&section, &properties);
        for (; dbus_message_iter_get_arg_type(&properties) == DBUS_TYPE_DICT_ENTRY; dbus_message_iter_next(&properties)) {
            DBusMessageIter property;
            char const* key = NULL;
            dbus_message_iter_recurse(&properties, &property);
            dbus_message_iter_get_basic(&property, &key);
            if (key && !strcmp(key, wanted_key)) {
                dbus_message_iter_next(&property);
                dbus_message_iter_recurse(&property, out);
                return 1;
            }
        }
    }
    return 0;
}

static char const* settings_string(DBusMessageIter* iter, char const* section, char const* key)
{
    DBusMessageIter value;
    char const* result = NULL;
    if (settings_lookup(iter, section, key, &value) && dbus_message_iter_get_arg_type(&value) == DBUS_TYPE_STRING)
        dbus_message_iter_get_basic(&value, &result);
    return result;
}

static int settings_bool(DBusMessageIter* iter, char const* section, char const* key, int fallback)
{
    DBusMessageIter value;
    dbus_bool_t result = fallback;
    if (settings_lookup(iter, section, key, &value) && dbus_message_iter_get_arg_type(&value) == DBUS_TYPE_BOOLEAN)
        dbus_message_iter_get_basic(&value, &result);
    return result ? 1 : 0;
}

// The SSID bytes as a string (the mock's SSIDs are ASCII).
static void settings_ssid(DBusMessageIter* iter, char* out, size_t size)
{
    DBusMessageIter value, bytes;
    size_t length = 0;
    out[0] = '\0';
    if (!settings_lookup(iter, "802-11-wireless", "ssid", &value) || dbus_message_iter_get_arg_type(&value) != DBUS_TYPE_ARRAY)
        return;
    dbus_message_iter_recurse(&value, &bytes);
    for (; dbus_message_iter_get_arg_type(&bytes) == DBUS_TYPE_BYTE && length + 1 < size; dbus_message_iter_next(&bytes)) {
        unsigned char byte = 0;
        dbus_message_iter_get_basic(&bytes, &byte);
        out[length++] = (char)byte;
    }
    out[length] = '\0';
}

static void on_secrets_reply(DBusPendingCall* pending, void* user_data)
{
    (void)user_data;
    DBusMessage* reply = dbus_pending_call_steal_reply(pending);
    dbus_pending_call_unref(pending);
    if (!reply)
        return;
    if (dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR) {
        log_line("mock-networkmanager: secrets error %s%s", dbus_message_get_error_name(reply), "");
        dbus_message_unref(reply);
        return;
    }

    char const* psk = NULL;
    DBusMessageIter iter;
    if (dbus_message_iter_init(reply, &iter))
        psk = find_psk(&iter);
    log_line("mock-networkmanager: secrets psk=%s%s", psk ? psk : "", "");
    if (psk && *psk) {
        s_active_ap = AP1_PATH;
        state_changed();
    }
    dbus_message_unref(reply);
}

// Ask the registered agent for MockNet's passphrase, as NetworkManager does when a
// saved profile without stored secrets is activated.
static void request_secrets(void)
{
    if (!s_agent[0]) {
        log_line("mock-networkmanager: no secret agent registered%s%s", "", "");
        return;
    }
    DBusMessage* call = dbus_message_new_method_call(s_agent, AGENT_PATH, IFACE_AGENT, "GetSecrets");
    DBusMessageIter iter, settings, section, properties, hints;
    dbus_message_iter_init_append(call, &iter);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sa{sv}}", &settings);
    char const* wireless = "802-11-wireless";
    dbus_message_iter_open_container(&settings, DBUS_TYPE_DICT_ENTRY, NULL, &section);
    dbus_message_iter_append_basic(&section, DBUS_TYPE_STRING, &wireless);
    dbus_message_iter_open_container(&section, DBUS_TYPE_ARRAY, "{sv}", &properties);
    append_byte_array(&properties, "ssid", "MockNet");
    dbus_message_iter_close_container(&section, &properties);
    dbus_message_iter_close_container(&settings, &section);
    dbus_message_iter_close_container(&iter, &settings);
    char const* connection_path = SAVED_PATH;
    char const* setting_name = "802-11-wireless-security";
    dbus_uint32_t flags = 0x1; // ALLOW_INTERACTION
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &connection_path);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &setting_name);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "s", &hints);
    dbus_message_iter_close_container(&iter, &hints);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_UINT32, &flags);

    DBusPendingCall* pending = NULL;
    if (dbus_connection_send_with_reply(s_connection, call, &pending, 30000) && pending)
        dbus_pending_call_set_notify(pending, on_secrets_reply, NULL, NULL);
    dbus_message_unref(call);
}

static DBusMessage* handle_set(DBusMessage* message)
{
    DBusMessageIter iter, variant;
    char const* interface = NULL;
    char const* property = NULL;
    if (!dbus_message_iter_init(message, &iter))
        return dbus_message_new_error(message, DBUS_ERROR_INVALID_ARGS, "no arguments");
    dbus_message_iter_get_basic(&iter, &interface);
    dbus_message_iter_next(&iter);
    dbus_message_iter_get_basic(&iter, &property);
    dbus_message_iter_next(&iter);
    if (!property || strcmp(property, "WirelessEnabled") != 0 || dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_VARIANT)
        return dbus_message_new_error(message, DBUS_ERROR_INVALID_ARGS, "unsupported property");
    dbus_message_iter_recurse(&iter, &variant);
    dbus_bool_t value = 0;
    if (dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_BOOLEAN)
        dbus_message_iter_get_basic(&variant, &value);
    log_line("mock-networkmanager: Set WirelessEnabled=%s%s", value ? "true" : "false", "");
    s_wireless_enabled = value ? 1 : 0;
    s_active_ap = NULL;
    if (!s_wireless_enabled)
        s_scanned = 0;
    return dbus_message_new_method_return(message);
}

static DBusHandlerResult handle_method_call(DBusMessage* message)
{
    char const* path = dbus_message_get_path(message);
    char const* interface = dbus_message_get_interface(message);
    char const* member = dbus_message_get_member(message);
    if (!path || !interface || !member)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    // Log the interesting (non-property) calls so the tests can assert that an
    // action reached NetworkManager.
    if (strcmp(interface, IFACE_PROPS) != 0)
        log_line("mock-networkmanager: %s.%s", interface, member);

    DBusMessage* reply = NULL;
    int changed = 0;

    if (!strcmp(interface, IFACE_PROPS) && !strcmp(member, "GetAll")) {
        DBusMessageIter iter;
        char const* requested_interface = NULL;
        if (dbus_message_iter_init(message, &iter))
            dbus_message_iter_get_basic(&iter, &requested_interface);
        // A stuck access point: the caller has to give up on its own.
        if (s_hang_ap && !strcmp(path, AP2_PATH))
            return DBUS_HANDLER_RESULT_HANDLED;
        if (requested_interface)
            reply = build_get_all_reply(message, path, requested_interface);
    } else if (!strcmp(interface, IFACE_PROPS) && !strcmp(member, "Set")) {
        reply = handle_set(message);
        changed = dbus_message_get_type(reply) != DBUS_MESSAGE_TYPE_ERROR;
    } else if (!strcmp(interface, IFACE_SETTINGS) && !strcmp(member, "ListConnections")) {
        reply = dbus_message_new_method_return(message);
        DBusMessageIter iter, array;
        char const* saved = SAVED_PATH;
        dbus_message_iter_init_append(reply, &iter);
        dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "o", &array);
        if (s_saved)
            dbus_message_iter_append_basic(&array, DBUS_TYPE_OBJECT_PATH, &saved);
        dbus_message_iter_close_container(&iter, &array);
    } else if (!strcmp(interface, IFACE_CONNECTION) && strcmp(path, SAVED_PATH) == 0 && !s_saved) {
        reply = dbus_message_new_error(message, DBUS_ERROR_UNKNOWN_OBJECT, "no such connection");
    } else if (!strcmp(interface, IFACE_CONNECTION) && !strcmp(member, "GetSettings")) {
        if (!strcmp(path, SAVED_PATH))
            reply = build_settings_reply(message);
        else
            reply = dbus_message_new_error(message, DBUS_ERROR_UNKNOWN_OBJECT, "no such connection");
    } else if (!strcmp(interface, IFACE_CONNECTION) && !strcmp(member, "Update") && !strcmp(path, SAVED_PATH)) {
        // Log what the profile still contains: an update has to send the whole
        // profile back, so a client that dropped part of it would show here.
        DBusMessageIter arguments;
        char ssid[64] = "";
        char line[256];
        char const* id = NULL;
        char const* psk = NULL;
        int autoconnect = 1;
        if (dbus_message_iter_init(message, &arguments)) {
            id = settings_string(&arguments, "connection", "id");
            autoconnect = settings_bool(&arguments, "connection", "autoconnect", 1);
            settings_ssid(&arguments, ssid, sizeof(ssid));
            psk = find_psk(&arguments);
        }
        snprintf(line, sizeof(line), "mock-networkmanager: update id=%s ssid=%s autoconnect=%s secrets=%s",
            id ? id : "", ssid, autoconnect ? "true" : "false", psk ? "yes" : "no");
        log_line("%s%s", line, "");
        s_saved_autoconnect = autoconnect;
        reply = dbus_message_new_method_return(message);
        changed = 1;
    } else if (!strcmp(interface, IFACE_CONNECTION) && !strcmp(member, "Delete") && !strcmp(path, SAVED_PATH)) {
        log_line("mock-networkmanager: delete %s%s", path, "");
        s_saved = 0;
        reply = dbus_message_new_method_return(message);
        changed = 1;
    } else if (!strcmp(interface, IFACE_NM) && !strcmp(member, "GetDevices")) {
        reply = dbus_message_new_method_return(message);
        DBusMessageIter iter, array;
        dbus_message_iter_init_append(reply, &iter);
        dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "o", &array);
        char const* device = DEV_PATH;
        dbus_message_iter_append_basic(&array, DBUS_TYPE_OBJECT_PATH, &device);
        dbus_message_iter_close_container(&iter, &array);
    } else if (!strcmp(interface, IFACE_AGENT_MGR) && !strcmp(member, "Register")) {
        snprintf(s_agent, sizeof(s_agent), "%s", dbus_message_get_sender(message));
        reply = dbus_message_new_method_return(message);
    } else if (!strcmp(interface, IFACE_AGENT_MGR) && !strcmp(member, "Unregister")) {
        s_agent[0] = '\0';
        reply = dbus_message_new_method_return(message);
    } else if (!strcmp(member, "Disconnect")) {
        s_active_ap = NULL;
        changed = 1;
        reply = dbus_message_new_method_return(message);
    } else if (!strcmp(member, "RequestScan")) {
        if (s_wireless_enabled) {
            s_scanned = 1;
            changed = 1;
            reply = dbus_message_new_method_return(message);
        } else {
            reply = dbus_message_new_error(message, "org.freedesktop.NetworkManager.Device.NotAllowed", "Wi-Fi is off");
        }
    } else if (!strcmp(member, "ActivateConnection")) {
        DBusMessageIter iter;
        char const* connection = NULL;
        if (dbus_message_iter_init(message, &iter) && dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_OBJECT_PATH)
            dbus_message_iter_get_basic(&iter, &connection);
        reply = dbus_message_new_method_return(message);
        char const* active = "/org/freedesktop/NetworkManager/ActiveConnection/1";
        dbus_message_append_args(reply, DBUS_TYPE_OBJECT_PATH, &active, DBUS_TYPE_INVALID);
        if (connection && !strcmp(connection, SAVED_PATH))
            request_secrets();
    } else if (!strcmp(member, "DeactivateConnection") || !strcmp(member, "Enable")) {
        reply = dbus_message_new_method_return(message);
    } else if (!strcmp(member, "AddAndActivateConnection")) {
        DBusMessageIter arguments;
        char const* psk = NULL;
        if (dbus_message_iter_init(message, &arguments))
            psk = find_psk(&arguments);
        log_line("mock-networkmanager: add psk=%s%s", psk ? psk : "", "");
        {
            char ssid[64] = "";
            char line[256];
            char const* key_mgmt = NULL;
            if (dbus_message_iter_init(message, &arguments)) {
                key_mgmt = settings_string(&arguments, "802-11-wireless-security", "key-mgmt");
                settings_ssid(&arguments, ssid, sizeof(ssid));
                snprintf(line, sizeof(line), "mock-networkmanager: add ssid=%s key-mgmt=%s hidden=%s", ssid,
                    key_mgmt ? key_mgmt : "", settings_bool(&arguments, "802-11-wireless", "hidden", 0) ? "true" : "false");
                log_line("%s%s", line, "");
            }
        }
        reply = dbus_message_new_method_return(message);
        DBusMessageIter iter;
        char const* active = "/org/freedesktop/NetworkManager/ActiveConnection/2";
        char const* connection = "/org/freedesktop/NetworkManager/Settings/2";
        dbus_message_iter_init_append(reply, &iter);
        dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &active);
        dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &connection);
    } else {
        reply = dbus_message_new_error(message, DBUS_ERROR_UNKNOWN_METHOD, member);
    }

    if (s_delay_ms > 0)
        usleep((useconds_t)s_delay_ms * 1000);
    if (reply) {
        dbus_connection_send(s_connection, reply, NULL);
        dbus_message_unref(reply);
    }
    if (changed)
        state_changed();
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

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--delay-ms") && i + 1 < argc)
            s_delay_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--extra-aps"))
            s_extra_aps = 1;
        else if (!strcmp(argv[i], "--hang-ap"))
            s_hang_ap = 1;
        else if (!strcmp(argv[i], "--chatter-ms") && i + 1 < argc)
            s_chatter_ms = atoi(argv[++i]);
        else {
            fprintf(stderr, "usage: mock-networkmanager [--delay-ms N] [--hang-ap] [--chatter-ms N] [--extra-aps]\n");
            return 2;
        }
    }

    DBusError error;
    dbus_error_init(&error);
    s_connection = dbus_bus_get(DBUS_BUS_SYSTEM, &error);
    if (!s_connection) {
        fprintf(stderr, "mock-networkmanager: cannot connect to the bus: %s\n", error.message);
        return 1;
    }
    dbus_connection_set_exit_on_disconnect(s_connection, TRUE);

    dbus_bus_request_name(s_connection, NM_NAME, 0, &error);
    if (dbus_error_is_set(&error)) {
        fprintf(stderr, "mock-networkmanager: cannot own %s: %s\n", NM_NAME, error.message);
        return 1;
    }

    dbus_connection_add_filter(s_connection, filter, NULL, NULL);
    fprintf(stderr, "mock-networkmanager: serving %s\n", NM_NAME);
    fflush(stderr);

    long long next_chatter = now_ms() + s_chatter_ms;
    for (;;) {
        int wait = s_chatter_ms > 0 ? (int)(next_chatter - now_ms()) : -1;
        if (s_chatter_ms > 0 && wait < 0)
            wait = 0;
        if (!dbus_connection_read_write_dispatch(s_connection, wait))
            break;
        if (s_chatter_ms > 0 && now_ms() >= next_chatter) {
            emit_changed(DEV_PATH, IFACE_DEV);
            next_chatter = now_ms() + s_chatter_ms;
        }
    }
    return 0;
}
