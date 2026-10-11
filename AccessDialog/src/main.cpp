/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// AccessDialog — the SDE permission dialog for the freedesktop Access portal
// (R5). It is a plain SerenityOS LibGUI app: the xdg-desktop-portal-serenity
// backend launches it with the request text, the user allows or denies, and the
// response (0 = allowed, 1 = denied) is printed to stdout.

#include <LibCore/ArgsParser.h>
#include <LibGUI/Application.h>
#include <LibGUI/BoxLayout.h>
#include <LibGUI/Button.h>
#include <LibGUI/TextEditor.h>
#include <LibGUI/Widget.h>
#include <LibGUI/Window.h>
#include <LibMain/Main.h>

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    auto app = TRY(GUI::Application::create(arguments));

    StringView window_title { "Permission Request"sv };
    StringView title { "Allow this request?"sv };
    StringView subtitle;
    StringView body;
    Core::ArgsParser args_parser;
    args_parser.add_option(window_title, "Window title", "window-title", 0, "title");
    args_parser.add_option(title, "Request title", "title", 't', "title");
    args_parser.add_option(subtitle, "Request subtitle", "subtitle", 's', "subtitle");
    args_parser.add_option(body, "Request body", "body", 'b', "body");
    args_parser.parse(arguments);

    auto window = GUI::Window::construct();
    window->set_title(window_title);
    window->resize(420, 260);
    window->set_resizable(false);

    auto main_widget = GUI::Widget::construct();
    main_widget->set_layout<GUI::VerticalBoxLayout>(8);
    window->set_main_widget(main_widget);

    // The request text (title/subtitle/body) goes in a read-only editor; the
    // window title carries the request's short title.
    StringBuilder request_text;
    request_text.append(title);
    if (!subtitle.is_empty())
        request_text.appendff("\n{}", subtitle);
    if (!body.is_empty())
        request_text.appendff("\n\n{}", body);
    auto& body_editor = main_widget->add<GUI::TextEditor>();
    body_editor.set_text(request_text.to_byte_string());
    body_editor.set_mode(GUI::TextEditor::Mode::ReadOnly);

    auto& buttons = main_widget->add<GUI::Widget>();
    buttons.set_layout<GUI::HorizontalBoxLayout>(6);
    buttons.set_fixed_height(28);
    auto& allow_button = buttons.add<GUI::Button>("Allow"_string);
    allow_button.set_fixed_width(90);
    auto& deny_button = buttons.add<GUI::Button>("Deny"_string);
    deny_button.set_fixed_width(90);

    int response = 1;
    allow_button.on_click = [&](auto) {
        response = 0;
        app->quit();
    };
    deny_button.on_click = [&](auto) {
        response = 1;
        app->quit();
    };

    window->show();
    app->exec();

    outln("{}", response);
    return 0;
}
