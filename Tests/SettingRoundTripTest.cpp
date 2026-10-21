/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Setting round-trip: change a setting the *desktop* owns through the app-facing
// WindowServer protocol and read it back. The LibWindowServer shim must forward
// the change to the desktop over serenity-window (set_host_setting) and answer
// from what it is told, rather than writing the desktop's configuration itself.
//
// Driven by scripts/run-setting-test.sh, which asserts the compositor applied and
// persisted the value.

#include <LibGUI/Application.h>
#include <LibGUI/ConnectionToWindowServer.h>
#include <LibMain/Main.h>

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    auto app = TRY(GUI::Application::create(arguments));
    auto& connection = GUI::ConnectionToWindowServer::the();

    float const before = connection.get_mouse_acceleration();
    connection.async_set_mouse_acceleration(2.5f);
    // The sync get is ordered after the async set on the same connection.
    float const after = connection.get_mouse_acceleration();
    outln("SETTING-TEST: accel {} -> {}", before, after);

    int const double_click_before = connection.get_double_click_speed();
    connection.async_set_double_click_speed(321);
    int const double_click_after = connection.get_double_click_speed();
    outln("SETTING-TEST: double-click {} -> {}", double_click_before, double_click_after);

    // Bounds still come from the desktop (host_config / host_settings).
    outln("SETTING-TEST: accel bounds {}..{}", connection.get_mouse_acceleration_min(), connection.get_mouse_acceleration_max());
    return 0;
}
