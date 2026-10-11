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
#include <LibGUI/Label.h>
#include <LibGUI/TextEditor.h>
#include <LibGUI/Widget.h>
#include <LibGUI/Window.h>
#include <LibGfx/TextAlignment.h>
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
    window->resize(420, 240);
    window->set_resizable(false);

    auto main_widget = GUI::Widget::construct();
    main_widget->set_layout<GUI::VerticalBoxLayout>(8);
    // As with GML dialogs, the root widget paints the window background.
    main_widget->set_fill_with_background_color(true);
    window->set_main_widget(main_widget);

    auto add_label = [&](StringView text, int height, Gfx::TextAlignment alignment) -> ErrorOr<void> {
        auto label = TRY(GUI::Label::try_create());
        label->set_text(TRY(String::from_utf8(text)));
        label->set_text_alignment(alignment);
        label->set_fixed_height(height);
        return main_widget->try_add_child(*label);
    };

    TRY(add_label(title, 20, Gfx::TextAlignment::CenterLeft));
    if (!subtitle.is_empty())
        TRY(add_label(subtitle, 18, Gfx::TextAlignment::CenterLeft));

    auto body_editor = TRY(GUI::TextEditor::try_create());
    body_editor->set_text(body);
    body_editor->set_mode(GUI::TextEditor::Mode::ReadOnly);
    TRY(main_widget->try_add_child(*body_editor));

    auto buttons = TRY(GUI::Widget::try_create());
    buttons->set_layout<GUI::HorizontalBoxLayout>(6);
    buttons->set_fixed_height(28);
    auto allow_button = TRY(GUI::Button::try_create());
    allow_button->set_text("Allow"_string);
    allow_button->set_fixed_width(90);
    TRY(buttons->try_add_child(*allow_button));
    auto deny_button = TRY(GUI::Button::try_create());
    deny_button->set_text("Deny"_string);
    deny_button->set_fixed_width(90);
    TRY(buttons->try_add_child(*deny_button));
    TRY(main_widget->try_add_child(*buttons));

    int response = 1;
    allow_button->on_click = [&](auto) {
        response = 0;
        app->quit();
    };
    deny_button->on_click = [&](auto) {
        response = 1;
        app->quit();
    };

    window->show();
    app->exec();

    outln("{}", response);
    return 0;
}
