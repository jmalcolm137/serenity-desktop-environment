/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "LibWM.h"
#include <LibCore/ArgsParser.h>
#include <LibCore/Timer.h>
#include <LibGUI/Application.h>
#include <LibGUI/BoxLayout.h>
#include <LibGUI/TextBox.h>
#include <LibGUI/Widget.h>
#include <LibGUI/Window.h>
#include <LibMain/Main.h>

// A LibGUI window with a focused text box that logs what it holds, for
// scripts/run-keyboard-replug-test.sh: a key typed before the keyboard goes away
// and one typed after a keyboard comes back must both arrive. The second one
// only does if LibWM notices that its wl_keyboard went with the capability and
// asks for a new one when the capability returns.
ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    int duration_ms = 20000;
    Core::ArgsParser args_parser;
    args_parser.add_option(duration_ms, "Quit after this many milliseconds", "duration", 'd', "ms");
    args_parser.parse(arguments);

    LibWM::initialize();
    auto app = TRY(GUI::Application::create(arguments));

    auto window = GUI::Window::construct();
    window->set_title("Keyboard Replug Test");
    window->set_rect(200, 150, 300, 80);
    auto main_widget = window->set_main_widget<GUI::Widget>();
    main_widget->set_fill_with_background_color(true);
    main_widget->set_layout<GUI::VerticalBoxLayout>(GUI::Margins { 8 });
    auto& text_box = main_widget->add<GUI::TextBox>();
    text_box.on_change = [&] {
        outln("REPLUG-TEST: text='{}'", text_box.text());
        fflush(stdout);
    };
    window->show();
    text_box.set_focus(true);
    outln("REPLUG-TEST: shown");
    fflush(stdout);

    auto quit = Core::Timer::create_single_shot(duration_ms, [&] { app->quit(0); });
    quit->start();
    return app->exec();
}
