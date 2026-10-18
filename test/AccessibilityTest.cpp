/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "LibWM.h"
#include <LibGUI/Application.h>
#include <LibGUI/Button.h>
#include <LibGUI/Label.h>
#include <LibGUI/TextEditor.h>
#include <LibGUI/Widget.h>
#include <LibGUI/Window.h>
#include <LibMain/Main.h>

// Payload for the AT-SPI test. It has one of each interface the accessibility
// server exposes: a frame, a label/Text node, a Button/Action node, and an
// editable TextEditor (Text + EditableText).
ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    LibWM::initialize();
    auto app = TRY(GUI::Application::create(arguments));

    auto window = GUI::Window::construct();
    window->set_title("Accessibility Test");
    window->set_rect(200, 100, 400, 300);

    auto main_widget = window->set_main_widget<GUI::Widget>();

    auto& label = main_widget->add<GUI::Label>("Hello accessibility"_string);
    label.set_relative_rect(20, 20, 240, 24);

    auto& button = main_widget->add<GUI::Button>("Press me"_string);
    button.set_relative_rect(20, 60, 120, 28);
    button.on_click = [&](auto) {
        label.set_text("Button activated"_string);
        label.set_focus(true);
    };

    auto& editor = main_widget->add<GUI::TextEditor>();
    editor.set_relative_rect(20, 110, 340, 150);
    editor.set_text("editable text"sv);

    window->show();
    return app->exec();
}
