/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// sde-auth-dialog — the password prompt used by sde-polkit-agent.
//
// It is a plain LibGUI client: the compositor supplies the window chrome, and
// the agent consumes the result over stdout. Output is two lines:
//
//   <password>
//   1|0          (remember for this session)
//
// Exit status 0 means Authenticate was clicked; 1 means canceled.

#include <LibCore/ArgsParser.h>
#include <LibGUI/Application.h>
#include <LibGUI/BoxLayout.h>
#include <LibGUI/Button.h>
#include <LibGUI/CheckBox.h>
#include <LibGUI/Label.h>
#include <LibGUI/TextBox.h>
#include <LibGUI/Widget.h>
#include <LibGUI/Window.h>
#include <LibGfx/TextAlignment.h>
#include <LibMain/Main.h>

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    auto app = TRY(GUI::Application::create(arguments));

    StringView title { "Authentication Required"sv };
    StringView message { "Authentication is required to continue."sv };
    StringView username;

    Core::ArgsParser args_parser;
    args_parser.add_option(title, "Window title", "title", 0, "title");
    args_parser.add_option(message, "Request message", "message", 'm', "message");
    args_parser.add_option(username, "Account name", "username", 'u', "name");
    args_parser.parse(arguments);

    auto window = GUI::Window::construct();
    window->set_title(title);
    window->resize(460, 220);
    window->set_resizable(false);

    auto main_widget = GUI::Widget::construct();
    main_widget->set_layout<GUI::VerticalBoxLayout>(GUI::Margins { 12 }, 8);
    main_widget->set_fill_with_background_color(true);
    window->set_main_widget(main_widget);

    auto& heading = main_widget->add<GUI::Label>();
    heading.set_text(TRY(String::from_utf8(title)));
    heading.set_text_alignment(Gfx::TextAlignment::CenterLeft);
    heading.set_fixed_height(22);

    auto& body = main_widget->add<GUI::Label>();
    body.set_text(TRY(String::from_utf8(message)));
    body.set_text_alignment(Gfx::TextAlignment::CenterLeft);
    body.set_text_wrapping(Gfx::TextWrapping::Wrap);
    body.set_fixed_height(44);

    if (!username.is_empty()) {
        auto& account = main_widget->add<GUI::Label>();
        account.set_text(TRY(String::formatted("Authenticating as {}", username)));
        account.set_text_alignment(Gfx::TextAlignment::CenterLeft);
        account.set_fixed_height(18);
    }

    auto& password_box = main_widget->add<GUI::PasswordBox>();
    password_box.set_fixed_height(26);
    password_box.set_placeholder("Password"sv);

    auto& remember_box = main_widget->add<GUI::CheckBox>("Do not ask again this session"_string);
    remember_box.set_checked(false);
    remember_box.set_fixed_height(22);

    auto& buttons = main_widget->add<GUI::Widget>();
    buttons.set_layout<GUI::HorizontalBoxLayout>(6);
    buttons.set_fixed_height(28);
    buttons.add_spacer();

    auto& cancel_button = buttons.add<GUI::Button>("Cancel"_string);
    cancel_button.set_fixed_width(96);

    auto& ok_button = buttons.add<GUI::Button>("Authenticate"_string);
    ok_button.set_fixed_width(112);
    ok_button.set_default(true);

    bool accepted = false;
    auto& password = password_box;
    auto& remember = remember_box;

    ok_button.on_click = [&](auto) {
        accepted = true;
        app->quit();
    };
    cancel_button.on_click = [&](auto) {
        accepted = false;
        app->quit();
    };
    password_box.on_escape_pressed = [&] {
        accepted = false;
        app->quit();
    };

    window->show();
    password_box.set_focus(true);
    app->exec();

    if (!accepted)
        return 1;

    outln("{}", password.text());
    outln("{}", remember.is_checked() ? 1 : 0);
    return 0;
}
