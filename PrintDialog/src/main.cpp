/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// PrintDialog — the SDE print dialog for the freedesktop Print portal (R5). It is
// a plain SerenityOS LibGUI app: the xdg-desktop-portal-serenity backend launches
// it with the available printers, the user picks a printer and copies, and the
// choice is printed to stdout as "<printer> <copies>" (exit 0); Cancel exits 1.

#include <LibCore/ArgsParser.h>
#include <LibGUI/Application.h>
#include <LibGUI/BoxLayout.h>
#include <LibGUI/Button.h>
#include <LibGUI/ComboBox.h>
#include <LibGUI/ItemListModel.h>
#include <LibGUI/Label.h>
#include <LibGUI/SpinBox.h>
#include <LibGUI/Widget.h>
#include <LibGUI/Window.h>
#include <LibMain/Main.h>

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    auto app = TRY(GUI::Application::create(arguments));

    StringView title { "Print"sv };
    StringView printer_list { "Print to File"sv };
    Core::ArgsParser args_parser;
    args_parser.add_option(title, "Dialog title", "title", 't', "title");
    args_parser.add_option(printer_list, "Comma-separated printers", "printers", 'p', "printers");
    args_parser.parse(arguments);

    Vector<String> printers;
    for (auto printer : printer_list.split_view(','))
        TRY(printers.try_append(TRY(String::from_utf8(printer.trim_whitespace()))));
    if (printers.is_empty())
        TRY(printers.try_append("Print to File"_string));

    auto window = GUI::Window::construct();
    window->set_title(title);
    window->resize(360, 180);
    window->set_resizable(false);

    auto main_widget = GUI::Widget::construct();
    main_widget->set_layout<GUI::VerticalBoxLayout>(8);
    window->set_main_widget(main_widget);

    auto& printer_row = main_widget->add<GUI::Widget>();
    printer_row.set_layout<GUI::HorizontalBoxLayout>(6);
    auto& printer_combo = printer_row.add<GUI::ComboBox>();
    printer_combo.set_model(*GUI::ItemListModel<String>::create(printers));
    printer_combo.set_selected_index(0);

    auto& copies_row = main_widget->add<GUI::Widget>();
    copies_row.set_layout<GUI::HorizontalBoxLayout>(6);
    auto& copies_spinbox = copies_row.add<GUI::SpinBox>();
    copies_spinbox.set_range(1, 99);

    auto& buttons = main_widget->add<GUI::Widget>();
    buttons.set_layout<GUI::HorizontalBoxLayout>(6);
    buttons.set_fixed_height(28);
    auto& print_button = buttons.add<GUI::Button>("Print"_string);
    print_button.set_fixed_width(90);
    auto& cancel_button = buttons.add<GUI::Button>("Cancel"_string);
    cancel_button.set_fixed_width(90);

    int result = 1;
    print_button.on_click = [&](auto) {
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
        outln("{}\t{}", printer_combo.text(), copies_spinbox.value());
    return result;
}
