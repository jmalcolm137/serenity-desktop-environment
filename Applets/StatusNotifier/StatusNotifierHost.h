/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/HashMap.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefCounted.h>
#include <LibGUI/Window.h>

#include "dbus.h"

namespace SDE {

// Hosts org.kde.StatusNotifierWatcher on the session D-Bus and renders each
// registered StatusNotifierItem (AppIndicator) as a native SerenityOS
// GUI::WindowType::Applet inside sde-applet-host (zero extra processes).
class StatusNotifierHost : public RefCounted<StatusNotifierHost> {
public:
    static ErrorOr<NonnullRefPtr<StatusNotifierHost>> try_create();
    ~StatusNotifierHost();

private:
    explicit StatusNotifierHost(NonnullRefPtr<Portal::Service>);

    struct TrayItem {
        ByteString key; // "<bus_name><object_path>"
        ByteString bus_name;
        ByteString object_path;
        ByteString title;
        ByteString icon_name;
        NonnullRefPtr<GUI::Window> window;
    };

    void register_item(ByteString const& service_arg, ByteString const& sender);
    void remove_items_for_owner(StringView owner);
    void refresh_item_metadata(TrayItem&);
    void send_item_action(ByteString const& bus_name, ByteString const& object_path, char const* method, int x, int y);

    static DBusHandlerResult dbus_signal_filter(DBusConnection*, DBusMessage*, void* user_data);

    NonnullRefPtr<Portal::Service> m_service;
    HashMap<ByteString, NonnullOwnPtr<TrayItem>> m_items;
    Vector<ByteString> m_hosts;
};

}
