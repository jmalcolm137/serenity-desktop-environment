/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Function.h>
#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibCore/EventReceiver.h>
#include <LibCore/Notifier.h>
#include <LibCore/Timer.h>

#include <dbus/dbus.h>

// One visible Wi-Fi network (one SSID, the strongest access point for it),
// mirroring what plasma-nm shows in its list.
struct WifiNetwork {
    ByteString ssid;
    ByteString access_point_path;
    ByteString device_path;
    int strength { 0 };
    int frequency { 0 };
    bool secured { false };
    bool connected { false };
    bool saved { false };
    ByteString saved_connection_path;
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

// A NetworkManager client over the system D-Bus, integrated with LibCore's
// event loop through a Core::Notifier, exactly like plasma-nm talks to
// NetworkManager. It also implements org.freedesktop.NetworkManager.SecretAgent
// so a saved WPA network can be activated (NetworkManager asks us for the
// passphrase). All state lives in refresh(); signals only mark it dirty, so we
// never reenter the D-Bus dispatcher with a blocking call.
class NetworkManagerClient : public Core::EventReceiver {
    C_OBJECT(NetworkManagerClient);

public:
    virtual ~NetworkManagerClient() override;

    NetworkState const& state() const { return m_state; }
    Vector<WifiNetwork> const& networks() const { return m_networks; }

    // Fired after every refresh that changed the visible state.
    Function<void()> on_changed;
    // Asks the UI for a passphrase (for a new network, or when NetworkManager's
    // secret agent needs one for a saved connection). The UI must call the
    // callback exactly once; an empty Optional means "cancelled".
    Function<void(ByteString const& ssid, Function<void(Optional<ByteString>)>)> on_password_required;

    void set_wireless_enabled(bool);
    void connect_to(WifiNetwork const& network, Optional<ByteString> psk);
    void disconnect(WifiNetwork const& network);
    void request_scan();

    // Re-read everything from NetworkManager and (if it changed) notify.
    void refresh();

    // Rebuild the bus connection after NetworkManager (or the bus) went away, so
    // a NetworkManager restart does not leave the applet showing stale networks
    // for the rest of the session.
    void reconnect();
    void register_agent();
    void schedule_reconnect();
    bool connect_to_bus();

private:
    NetworkManagerClient();

    static DBusHandlerResult filter(DBusConnection*, DBusMessage*, void*);
    void on_socket_readable();
    void schedule_refresh();

    void handle_secret_agent_call(DBusMessage*);
    void complete_secret_request(Optional<ByteString> passphrase);

    DBusConnection* m_connection { nullptr };
    DBusMessage* m_pending_secret_reply { nullptr };
    ByteString m_pending_secret_ssid;
    Function<void(Optional<ByteString>)> m_pending_secret_callback;
    RefPtr<Core::Notifier> m_notifier;
    RefPtr<Core::Timer> m_reconnect_timer;
    bool m_needs_refresh { false };
    bool m_refresh_scheduled { false };

    NetworkState m_state;
    Vector<WifiNetwork> m_networks;
};
