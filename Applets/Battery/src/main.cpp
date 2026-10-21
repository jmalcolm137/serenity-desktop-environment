/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// BatteryApplet — the SDE battery status applet for the Taskbar's applet area.
// Modeled on KDE's Battery and Brightness applet: a compact tray icon that
// tracks the charge level (with a charging bolt), a tooltip with the state and
// time remaining, and a detail popup opened by clicking the icon. It reads the
// Linux power-supply class (sysfs) directly, so it needs no UPower/D-Bus
// dependency.

#include <LibCore/ArgsParser.h>
#include <LibCore/System.h>
#include <LibGUI/Application.h>
#include <LibGUI/Window.h>
#include <LibMain/Main.h>

#include "BatteryWidget.h"

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    TRY(Core::System::pledge("stdio recvfd sendfd rpath"));

    auto app = TRY(GUI::Application::create(arguments));
    app->set_quit_when_last_window_deleted(false);

    TRY(Core::System::unveil("/sys/class/power_supply", "r"));
    TRY(Core::System::unveil("/res", "r"));
    TRY(Core::System::unveil(nullptr, nullptr));

    ByteString applet_name = "Battery";
    ByteString power_supply_root = "/sys/class/power_supply";
    bool icon_only = false;

    Core::ArgsParser args_parser;
    args_parser.add_option(applet_name, "Applet name used by [Applet] Order in WindowServer.ini", "name", 'n', "name");
    args_parser.add_option(icon_only, "Show only the icon, without the percentage label", "icon-only", 'i');
    args_parser.add_option(power_supply_root, "Override the sysfs power-supply directory (for testing)", "power-supply-root", 0, "path");
    args_parser.parse(arguments);

    auto window = GUI::Window::construct();
    window->set_title(applet_name);
    window->set_window_type(GUI::WindowType::Applet);
    window->set_has_alpha_channel(true);
    window->resize(16, 16);

    auto battery_widget = window->set_main_widget<BatteryWidget>(power_supply_root);
    battery_widget->set_show_percentage(!icon_only);

    window->show();

    return app->exec();
}
