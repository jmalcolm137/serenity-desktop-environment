/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// AppChooserDialog — the SDE application chooser for the freedesktop AppChooser
// portal (R5). It is a plain SerenityOS LibGUI app: the xdg-desktop-portal-serenity
// backend launches it with the ordered application choices, the user picks one,
// and the chosen index is printed to stdout (exit 0); Cancel exits 1.

#include <LibCore/ArgsParser.h>
#include <LibGUI/Application.h>
#include <LibGUI/BoxLayout.h>
#include <LibGUI/Button.h>
#include <LibGUI/Widget.h>
#include <LibGUI/Window.h>
#include <LibMain/Main.h>

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    auto app = TRY(GUI::Application::create(arguments));

    StringView title { "Choose an Application"sv };
    StringView choices_arg;
    Core::ArgsParser args_parser;
    args_parser.add_option(title, "Dialog title", "title", 't', "title");
    args_parser.add_option(choices_arg, "Newline-separated choices", "choices", 'c', "choices");
    args_parser.parse(arguments);

    auto window = GUI::Window::construct();
    window->set_title(title);
    window->resize(360, 80 + 30 * 4);
    window->set_resizable(false);

    auto main_widget = GUI::Widget::construct();
    main_widget->set_layout<GUI::VerticalBoxLayout>(6);
    // As with GML dialogs, the root widget paints the window background.
    main_widget->set_fill_with_background_color(true);
    window->set_main_widget(main_widget);

    int index = 0;
    int result = -1;
    for (auto choice : choices_arg.split_view('\n')) {
        if (choice.is_empty())
            continue;
        int const this_index = index++;
        auto& button = main_widget->add<GUI::Button>(TRY(String::from_utf8(choice)));
        button.set_fixed_height(28);
        button.on_click = [&, this_index](auto) {
            result = this_index;
            app->quit();
        };
    }

    auto& cancel_button = main_widget->add<GUI::Button>("Cancel"_string);
    cancel_button.set_fixed_height(28);
    cancel_button.on_click = [&](auto) {
        result = -1;
        app->quit();
    };

    window->show();
    app->exec();

    if (result >= 0) {
        outln("{}", result);
        return 0;
    }
    return 1;
}
