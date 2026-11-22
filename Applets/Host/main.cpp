/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// sde-applet-host — hosts SDE's own Taskbar applets (ResourceGraph, Network,
// Battery, WiFi) inside a single LibGUI process so they share one
// Core::EventLoop, one Wayland connection, one font/theme cache, and one set of
// dynamic-linker relocations (saving ~3-4 MB of unique private PSS compared to
// four separate processes; see docs/25-performance.md §4).

#include <AK/Vector.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/System.h>
#include <LibGUI/Application.h>
#include <LibGUI/Window.h>
#include <LibMain/Main.h>

#include "BatteryWidget.h"
#include "NetworkManagerClient.h"
#include "WifiWidget.h"

// Defined in Applets/Network/main.cpp and Applets/ResourceGraph/main.cpp when
// compiled with -DSDE_APPLET_HOST_EMBEDDED.
ErrorOr<NonnullRefPtr<GUI::Window>> create_network_applet_window(StringView name, bool display_notifications);
ErrorOr<Vector<NonnullRefPtr<GUI::Window>>> create_resource_graph_applet_windows(StringView cpu, StringView memory, StringView network);

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    TRY(Core::System::pledge("stdio recvfd sendfd rpath unix proc exec"));

    auto app = TRY(GUI::Application::create(arguments));
    app->set_quit_when_last_window_deleted(false);

    bool enable_battery = false;
    bool enable_wifi = false;
    bool enable_network = false;
    bool enable_resource_graph = false;

    ByteString battery_name = "Battery";
    ByteString power_supply_root = "/sys/class/power_supply";
    bool battery_icon_only = false;

    ByteString wifi_name = "WiFi";

    StringView network_name = "Network"sv;
    bool network_notifications = false;

    StringView cpu_spec = "CPUGraph,#00bb00"sv;
    StringView memory_spec = "MemoryGraph,#00bbbb"sv;
    StringView network_graph_spec = "NetworkGraph,#bbbb00"sv;

    Core::ArgsParser args_parser;
    args_parser.add_option(enable_battery, "Host the Battery applet", "battery");
    args_parser.add_option(enable_wifi, "Host the Wi-Fi applet", "wifi");
    args_parser.add_option(enable_network, "Host the Network applet", "network");
    args_parser.add_option(enable_resource_graph, "Host the ResourceGraph applets", "resource-graph");
    args_parser.add_option(battery_name, "Battery applet name", "battery-name", 0, "name");
    args_parser.add_option(battery_icon_only, "Show only the battery icon", "battery-icon-only");
    args_parser.add_option(power_supply_root, "Override sysfs power-supply directory", "power-supply-root", 0, "path");
    args_parser.add_option(wifi_name, "Wi-Fi applet name", "wifi-name", 0, "name");
    args_parser.add_option(network_name, "Network applet name", "network-name", 0, "name");
    args_parser.add_option(network_notifications, "Display Network applet notifications", "network-notifications");
    args_parser.add_option(cpu_spec, "CPU graph spec (name,#rrggbb)", "cpu", 0, "spec");
    args_parser.add_option(memory_spec, "Memory graph spec (name,#rrggbb)", "memory", 0, "spec");
    args_parser.add_option(network_graph_spec, "Network graph spec (name,#rrggbb)", "network-graph", 0, "spec");
    args_parser.parse(arguments);

    // If no individual applet flags were passed, host all four by default.
    if (!enable_battery && !enable_wifi && !enable_network && !enable_resource_graph) {
        enable_battery = true;
        enable_wifi = true;
        enable_network = true;
        enable_resource_graph = true;
    }

    Vector<NonnullRefPtr<GUI::Window>> windows;
    RefPtr<NetworkManagerClient> nm_client;

    if (enable_resource_graph) {
        auto graphs = create_resource_graph_applet_windows(cpu_spec, memory_spec, network_graph_spec);
        if (graphs.is_error()) {
            warnln("sde-applet-host: ResourceGraph failed: {}", graphs.error());
        } else {
            for (auto& window : graphs.value())
                windows.append(move(window));
        }
    }

    if (enable_network) {
        auto window = create_network_applet_window(network_name, network_notifications);
        if (window.is_error())
            warnln("sde-applet-host: Network failed: {}", window.error());
        else
            windows.append(window.release_value());
    }

    if (enable_battery) {
        auto window = GUI::Window::construct();
        window->set_title(battery_name);
        window->set_window_type(GUI::WindowType::Applet);
        window->set_has_alpha_channel(true);
        window->resize(16, 16);
        auto widget = window->set_main_widget<BatteryWidget>(power_supply_root);
        widget->set_show_percentage(!battery_icon_only);
        window->show();
        windows.append(move(window));
    }

    if (enable_wifi) {
        auto client_or_error = NetworkManagerClient::try_create();
        if (client_or_error.is_error()) {
            warnln("sde-applet-host: WiFi unavailable: {}", client_or_error.error());
        } else {
            nm_client = client_or_error.release_value();
            auto window = GUI::Window::construct();
            window->set_title(wifi_name);
            window->set_window_type(GUI::WindowType::Applet);
            window->set_has_alpha_channel(true);
            window->resize(16, 16);
            [[maybe_unused]] auto widget = window->set_main_widget<WifiWidget>(*nm_client);
            window->show();
            windows.append(move(window));
        }
    }

    if (windows.is_empty()) {
        warnln("sde-applet-host: no applets started");
        return 1;
    }

    return app->exec();
}
