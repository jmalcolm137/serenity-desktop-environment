/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Function.h>
#include <AK/HashTable.h>
#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibCore/EventReceiver.h>
#include <LibCore/Timer.h>

#include <dbus/dbus.h>

// How a network is secured, from its access point's flags. This decides what
// the user is asked for and how the connection is set up.
enum class WifiSecurity {
    Open,        // no encryption
    EnhancedOpen, // OWE: encrypted, but no password
    Wep,         // WEP (obsolete; a key)
    Personal,    // WPA/WPA2 personal (a passphrase), possibly also WPA3
    Wpa3Personal, // WPA3 (SAE) only
    Enterprise,  // 802.1X: certificates and an identity, set up elsewhere
};

struct WifiSecurityInfo {
    WifiSecurity type { WifiSecurity::Open };
    // As the user knows it: "WPA2", "WPA2/WPA3", "WPA3 Enterprise", "Open", ...
    StringView label;
};

// From an access point's Flags, WpaFlags and RsnFlags (NM80211ApFlags and
// NM80211ApSecurityFlags).
WifiSecurityInfo classify_wifi_security(unsigned flags, unsigned wpa_flags, unsigned rsn_flags);

// One visible Wi-Fi network (one SSID, the strongest access point for it),
// mirroring what plasma-nm shows in its list.
struct WifiNetwork {
    ByteString ssid;
    ByteString access_point_path;
    ByteString device_path;
    int strength { 0 };
    int frequency { 0 };
    bool secured { false };
    WifiSecurity security { WifiSecurity::Open };
    StringView security_label { "Open"sv };
    bool connected { false };
    bool saved { false };
    ByteString saved_connection_path;
    // The saved profile's connection.autoconnect: whether NetworkManager joins
    // this network by itself when it is in range.
    bool autoconnect { true };

    // Joining it needs something typed in (a passphrase or a WEP key).
    bool needs_password() const { return security == WifiSecurity::Personal || security == WifiSecurity::Wpa3Personal || security == WifiSecurity::Wep; }
};

struct NetworkState {
    bool available { false };
    bool networking_enabled { true };
    bool wireless_hardware_enabled { true };
    bool wireless_enabled { true };
    bool connecting { false };
    int connectivity { 0 };
    ByteString active_ssid;
    int active_strength { 0 };
    ByteString wifi_device_path;
};

// A NetworkManager client over the system D-Bus, like plasma-nm's. It also
// implements org.freedesktop.NetworkManager.SecretAgent, so a saved WPA network
// can be activated (NetworkManager asks us for the passphrase).
//
// Nothing here ever waits on the bus. The connection is integrated with LibCore's
// event loop (its watches, timeouts and dispatch), every call is asynchronous and
// bounded by a timeout, and a refresh is a fan-out of parallel calls whose results
// are assembled when the last one answers. Signals only ask for a refresh; one
// that arrives while a refresh is in flight queues exactly one more, so a busy
// NetworkManager can neither starve the refresh nor leave stale data on screen.
// NetworkManager restarting and the bus going away are both recovered from.
class NetworkManagerClient : public Core::EventReceiver {
    C_OBJECT(NetworkManagerClient);

public:
    virtual ~NetworkManagerClient() override;

    NetworkState const& state() const { return m_state; }
    Vector<WifiNetwork> const& networks() const { return m_networks; }

    // Fired after every refresh that changed the visible state.
    Function<void()> on_changed;
    // Fired after every refresh, changed or not.
    Function<void()> on_refresh_finished;
    // Asks the UI for a passphrase (for a new network, or when NetworkManager's
    // secret agent needs one for a saved connection). The UI must call the
    // callback exactly once; an empty Optional means "cancelled". A callback that
    // is called after NetworkManager withdrew its request is ignored.
    Function<void(ByteString const& ssid, Function<void(Optional<ByteString>)>)> on_password_required;
    // NetworkManager withdrew the request on_password_required is prompting for.
    Function<void()> on_password_request_cancelled;

    void set_wireless_enabled(bool);
    void connect_to(WifiNetwork const& network, Optional<ByteString> psk);
    void disconnect(WifiNetwork const& network);
    // Whether NetworkManager may join a saved network by itself. The profile and
    // its password stay, so the user can still connect with a click.
    void set_autoconnect(WifiNetwork const& network, bool);
    // Delete the saved profile, password and all.
    void forget(WifiNetwork const& network);
    void request_scan();

    // Re-read everything from NetworkManager and (if it changed) notify. Returns
    // at once; the result arrives through on_changed.
    void refresh();

private:
    struct Refresh;
    struct PendingCall;
    using ReplyHandler = Function<void(DBusMessage* reply)>;

    NetworkManagerClient();

    // The bus connection.
    void connect_to_bus();
    void on_connected();
    void disconnect_from_bus();
    void connection_lost();
    void schedule_reconnect();
    void schedule_dispatch();
    void dispatch();
    void register_agent();

    static DBusHandlerResult filter(DBusConnection*, DBusMessage*, void*);
    void handle_signal(DBusMessage*);
    void networkmanager_appeared(ByteString owner);
    void networkmanager_vanished();

    // Calls. The handler always runs exactly once, on the event loop, with the
    // reply or with nullptr (an error, a timeout, a lost connection) -- unless
    // the connection is torn down first, in which case it never runs.
    void call(DBusMessage*, ReplyHandler);
    void call_get_all(char const* path, char const* interface, ReplyHandler);
    void call_for_refresh(NonnullRefPtr<Refresh> const&, DBusMessage*, ReplyHandler);
    void cancel_pending_calls();
    void action(DBusMessage*);

    // A refresh.
    void schedule_refresh();
    void finish_refresh(Refresh&);
    void release(NonnullRefPtr<Refresh> const&);
    void publish(NetworkState, Vector<WifiNetwork>);

    // The secret agent.
    void handle_secret_agent_call(DBusMessage*);
    void complete_secret_request(u64 request_id, Optional<ByteString> passphrase);
    void abandon_secret_request(char const* error_name, char const* error_message);
    void send_reply(DBusMessage*);

    DBusConnection* m_connection { nullptr };
    bool m_registered_with_bus { false };
    ByteString m_networkmanager_owner;
    HashTable<PendingCall*> m_pending_calls;
    RefPtr<Core::Timer> m_reconnect_timer;
    bool m_dispatch_scheduled { false };
    bool m_teardown_scheduled { false };

    u64 m_generation { 0 };
    bool m_refresh_in_flight { false };
    bool m_refresh_again { false };
    bool m_refresh_scheduled { false };

    DBusMessage* m_pending_secret_request { nullptr };
    u64 m_secret_request_id { 0 };

    NetworkState m_state;
    Vector<WifiNetwork> m_networks;
};
