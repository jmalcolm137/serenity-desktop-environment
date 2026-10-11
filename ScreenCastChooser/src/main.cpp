/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// ScreenCastChooser — the SDE screen-share source picker (R5). It is a plain
// SerenityOS LibGUI app: the xdg-desktop-portal-serenity backend launches it with
// the available sources as arguments, the user picks one, and the chosen index is
// printed to stdout (exit 0); Cancel exits 1 with no output.

#include <LibCore/ArgsParser.h>
#include <LibGUI/Application.h>
#include <LibGUI/BoxLayout.h>
#include <LibGUI/Button.h>
#include <LibGUI/ItemListModel.h>
#include <LibGUI/ListView.h>
#include <LibGUI/Widget.h>
#include <LibGUI/Window.h>
#include <LibMain/Main.h>

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    auto app = TRY(GUI::Application::create(arguments));

    Vector<String> sources;
    StringView title { "Screen Share"sv };
    Core::ArgsParser args_parser;
    args_parser.add_option(title, "Window title", "title", 't', "title");
    args_parser.add_positional_argument(sources, "Sources to choose from", "source", Core::ArgsParser::Required::No);
    args_parser.parse(arguments);

    if (sources.is_empty())
        sources.append("Entire screen"_string);

    auto window = GUI::Window::construct();
    window->set_title(title);
    window->resize(380, 280);
    window->set_resizable(false);

    auto main_widget = GUI::Widget::construct();
    main_widget->set_layout<GUI::VerticalBoxLayout>(6);
    // As with GML dialogs, the root widget paints the window background.
    main_widget->set_fill_with_background_color(true);
    window->set_main_widget(main_widget);

    auto& list = main_widget->add<GUI::ListView>();
    list.set_model(*GUI::ItemListModel<String>::create(sources));
    list.set_cursor(list.model()->index(0, 0), GUI::AbstractView::SelectionUpdate::Set);

    auto& buttons = main_widget->add<GUI::Widget>();
    buttons.set_layout<GUI::HorizontalBoxLayout>(6);
    buttons.set_fixed_height(28);

    auto& share_button = buttons.add<GUI::Button>("Share"_string);
    share_button.set_fixed_width(90);
    auto& cancel_button = buttons.add<GUI::Button>("Cancel"_string);
    cancel_button.set_fixed_width(90);

    int result = 1;
    share_button.on_click = [&](auto) {
        result = 0;
        app->quit();
    };
    cancel_button.on_click = [&](auto) {
        result = 1;
        app->quit();
    };

    window->show();
    app->exec();

    if (result == 0)
        outln("{}", list.selection().first().row());
    return result;
}
