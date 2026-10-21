/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// WifiApplet — the SDE Wi-Fi applet for the Taskbar's applet area, modeled on
// KDE Plasma's Networks applet (plasma-nm): a Breeze signal-arc tray icon, a
// tooltip, and a popup with a Wi-Fi switch, the network list, connect/disconnect
// and a passphrase prompt. It talks to NetworkManager over the system D-Bus,
// exactly as plasma-nm does.

#include <LibCore/ArgsParser.h>
#include <LibCore/System.h>
#include <LibGUI/Application.h>
#include <LibGUI/Window.h>
#include <LibMain/Main.h>

#include "NetworkManagerClient.h"
#include "WifiWidget.h"

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    TRY(Core::System::pledge("stdio recvfd sendfd rpath unix"));

    auto app = TRY(GUI::Application::create(arguments));
    app->set_quit_when_last_window_deleted(false);

    ByteString applet_name = "WiFi";

    Core::ArgsParser args_parser;
    args_parser.add_option(applet_name, "Applet name used by [Applet] Order in WindowServer.ini", "name", 'n', "name");
    args_parser.parse(arguments);

    auto client = TRY(NetworkManagerClient::try_create());

    auto window = GUI::Window::construct();
    window->set_title(applet_name);
    window->set_window_type(GUI::WindowType::Applet);
    window->set_has_alpha_channel(true);
    window->resize(16, 16);

    [[maybe_unused]] auto widget = window->set_main_widget<WifiWidget>(client);
    window->show();

    return app->exec();
}
