/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// sde-auth-dialog — the password/challenge prompt used by sde-polkit-agent.
//
// It is a plain LibGUI client: the compositor supplies the window chrome, and
// the agent consumes the result over a dedicated pipe (--output-fd, defaulting
// to stdout). Output is:
//
//   <response>
//   1|0                  (remember password until lock or session end)
//   USER:<username>      (optional; emitted when multiple identities were given)
//
// Exit status 0 means Authenticate was clicked; 1 means canceled.

#include <AK/ByteString.h>
#include <AK/StringBuilder.h>
#include <AK/Vector.h>
#include <LibCore/ArgsParser.h>
#include <LibGUI/Application.h>
#include <LibGUI/BoxLayout.h>
#include <LibGUI/Button.h>
#include <LibGUI/CheckBox.h>
#include <LibGUI/ComboBox.h>
#include <LibGUI/ImageWidget.h>
#include <LibGUI/ItemListModel.h>
#include <LibGUI/Label.h>
#include <LibGUI/TextBox.h>
#include <LibGUI/TextEditor.h>
#include <LibGUI/Widget.h>
#include <LibGUI/Window.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Color.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Palette.h>
#include <LibGfx/TextAlignment.h>
#include <LibMain/Main.h>
#include <LibXDG/IconTheme.h>

#include <errno.h>
#include <unistd.h>

namespace {

constexpr int dialog_width = 480;

bool write_all(int fd, ReadonlyBytes bytes)
{
    size_t written = 0;
    while (written < bytes.size()) {
        ssize_t count = ::write(fd, bytes.data() + written, bytes.size() - written);
        if (count > 0) {
            written += static_cast<size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        return false;
    }
    return true;
}

int estimate_wrapped_height(StringView text, Gfx::Font const& font, int max_width)
{
    if (text.is_empty() || max_width <= 0)
        return 22;
    int line_height = font.preferred_line_height();
    int total_lines = 0;
    for (auto paragraph : text.split_view('\n', SplitBehavior::KeepEmpty)) {
        if (paragraph.is_empty()) {
            ++total_lines;
            continue;
        }
        int width = font.width_rounded_up(paragraph);
        int lines = (width + max_width - 1) / max_width;
        total_lines += max(1, lines);
    }
    return max(22, total_lines * line_height + 6);
}

RefPtr<Gfx::Bitmap> load_dialog_icon(StringView icon_name)
{
    auto try_xdg = [](StringView name) -> RefPtr<Gfx::Bitmap> {
        if (name.is_empty())
            return {};
        auto path = XDG::find_icon(name, 32);
        if (!path.has_value())
            return {};
        auto bitmap = Gfx::Bitmap::load_from_file(*path, 1, Gfx::IntSize { 32, 32 });
        if (bitmap.is_error())
            return {};
        return bitmap.release_value();
    };

    if (auto bmp = try_xdg(icon_name))
        return bmp;
    if (auto bmp = try_xdg("dialog-password"sv))
        return bmp;
    if (auto bmp = try_xdg("changes-prevent"sv))
        return bmp;

    for (auto fallback : Array { "/res/icons/32x32/key.png"sv, "/res/icons/32x32/lock.png"sv, "/res/icons/16x16/key.png"sv }) {
        if (auto bitmap = Gfx::Bitmap::load_from_file(fallback); !bitmap.is_error())
            return bitmap.release_value();
    }
    return {};
}

}

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    auto app = TRY(GUI::Application::create(arguments));

    StringView title { "Authentication Required"sv };
    StringView message { "Authentication is required to continue."sv };
    StringView username;
    StringView identities_csv;
    StringView action_id;
    StringView details_text;
    StringView icon_name;
    StringView error_text;
    StringView prompt_text;
    bool echo_input = false;
    bool hide_remember = false;
    int output_fd = STDOUT_FILENO;

    Core::ArgsParser args_parser;
    args_parser.add_option(title, "Window title", "title", 0, "title");
    args_parser.add_option(message, "Request message", "message", 'm', "message");
    args_parser.add_option(username, "Account name", "username", 'u', "name");
    args_parser.add_option(identities_csv, "Comma-separated list of candidate usernames", "identities", 0, "names");
    args_parser.add_option(action_id, "Polkit action ID", "action-id", 0, "id");
    args_parser.add_option(details_text, "Newline-separated details", "details", 0, "text");
    args_parser.add_option(icon_name, "Icon name", "icon", 0, "name");
    args_parser.add_option(error_text, "Inline error message", "error", 0, "text");
    args_parser.add_option(prompt_text, "Input prompt placeholder", "prompt", 0, "text");
    args_parser.add_option(echo_input, "Show typed input (for PAM_PROMPT_ECHO_ON)", "echo", 0);
    args_parser.add_option(hide_remember, "Hide the session remember checkbox", "hide-remember", 0);
    args_parser.add_option(output_fd, "File descriptor for structured output", "output-fd", 0, "fd");
    args_parser.parse(arguments);

    Vector<String> identity_list;
    if (!identities_csv.is_empty()) {
        for (auto part : identities_csv.split_view(',')) {
            auto trimmed = part.trim_whitespace();
            if (!trimmed.is_empty())
                TRY(identity_list.try_append(TRY(String::from_utf8(trimmed))));
        }
    }
    if (identity_list.is_empty() && !username.is_empty())
        TRY(identity_list.try_append(TRY(String::from_utf8(username))));

    auto window = GUI::Window::construct();
    window->set_title(title);
    window->set_resizable(false);

    auto main_widget = GUI::Widget::construct();
    main_widget->set_layout<GUI::VerticalBoxLayout>(GUI::Margins { 14 }, 8);
    main_widget->set_fill_with_background_color(true);
    window->set_main_widget(main_widget);

    // Header row: optional 32x32 icon + title & wrapping message body.
    auto& header_row = main_widget->add<GUI::Widget>();
    header_row.set_layout<GUI::HorizontalBoxLayout>(12);

    auto icon_bitmap = load_dialog_icon(icon_name);
    int text_column_width = dialog_width - 28;
    if (icon_bitmap) {
        auto& icon_widget = header_row.add<GUI::ImageWidget>();
        icon_widget.set_bitmap(icon_bitmap);
        icon_widget.set_should_stretch(true);
        icon_widget.set_fixed_size(32, 32);
        text_column_width -= 44;
    }

    auto& header_text = header_row.add<GUI::Widget>();
    header_text.set_layout<GUI::VerticalBoxLayout>(4);

    auto& heading = header_text.add<GUI::Label>();
    heading.set_text(TRY(String::from_utf8(title)));
    heading.set_text_alignment(Gfx::TextAlignment::CenterLeft);
    heading.set_font(heading.font().bold_variant());
    heading.set_fixed_height(20);

    auto& body = header_text.add<GUI::Label>();
    body.set_text(TRY(String::from_utf8(message)));
    body.set_text_alignment(Gfx::TextAlignment::TopLeft);
    body.set_text_wrapping(Gfx::TextWrapping::Wrap);
    int body_height = estimate_wrapped_height(message, body.font(), text_column_width);
    body.set_fixed_height(body_height);

    int header_height = max(icon_bitmap ? 36 : 24, 20 + 4 + body_height);
    header_row.set_fixed_height(header_height);

    int total_height = 28 + header_height;

    // Inline error banner (shown on retry / PAM_ERROR_MSG).
    if (!error_text.is_empty()) {
        auto& error_label = main_widget->add<GUI::Label>();
        error_label.set_text(TRY(String::from_utf8(error_text)));
        error_label.set_text_alignment(Gfx::TextAlignment::CenterLeft);
        error_label.set_text_wrapping(Gfx::TextWrapping::Wrap);
        auto error_palette = error_label.palette();
        error_palette.set_color(Gfx::ColorRole::WindowText, Gfx::Color(180, 28, 28));
        error_label.set_palette(error_palette);
        int error_height = estimate_wrapped_height(error_text, error_label.font(), dialog_width - 28);
        error_label.set_fixed_height(error_height);
        total_height += 8 + error_height;
    }

    // Account row: static label for a single user, ComboBox when multiple identities exist.
    RefPtr<GUI::ComboBox> identity_combo;
    if (identity_list.size() > 1) {
        auto& account_row = main_widget->add<GUI::Widget>();
        account_row.set_layout<GUI::HorizontalBoxLayout>(8);
        account_row.set_fixed_height(24);

        auto& account_label = account_row.add<GUI::Label>("Authenticate as:"_string);
        account_label.set_text_alignment(Gfx::TextAlignment::CenterLeft);
        account_label.set_fixed_width(110);

        auto& combo = account_row.add<GUI::ComboBox>();
        combo.set_model(*GUI::ItemListModel<String>::create(identity_list));
        combo.set_only_allow_values_from_model(true);
        size_t initial_index = 0;
        if (!username.is_empty()) {
            for (size_t i = 0; i < identity_list.size(); ++i) {
                if (identity_list[i].bytes_as_string_view() == username) {
                    initial_index = i;
                    break;
                }
            }
        }
        combo.set_selected_index(initial_index);
        identity_combo = combo;
        total_height += 8 + 24;
    } else if (identity_list.size() == 1) {
        auto& account = main_widget->add<GUI::Label>();
        account.set_text(TRY(String::formatted("Authenticating as {}", identity_list[0])));
        account.set_text_alignment(Gfx::TextAlignment::CenterLeft);
        account.set_fixed_height(18);
        total_height += 8 + 18;
    }

    // Input row: PasswordBox + Show/Hide toggle (or plain TextBox for PAM_PROMPT_ECHO_ON).
    auto& input_row = main_widget->add<GUI::Widget>();
    input_row.set_layout<GUI::HorizontalBoxLayout>(6);
    input_row.set_fixed_height(26);

    auto placeholder = !prompt_text.is_empty() ? prompt_text : (echo_input ? "Response"sv : "Password"sv);
    RefPtr<GUI::PasswordBox> password_box;
    RefPtr<GUI::TextBox> visible_box;
    RefPtr<GUI::Button> toggle_button;
    bool showing_plaintext = echo_input;

    if (echo_input) {
        auto& box = input_row.add<GUI::TextBox>();
        box.set_fixed_height(26);
        box.set_placeholder(placeholder);
        visible_box = box;
    } else {
        auto& masked = input_row.add<GUI::PasswordBox>();
        masked.set_fixed_height(26);
        masked.set_placeholder(placeholder);
        password_box = masked;

        auto& plain = input_row.add<GUI::TextBox>();
        plain.set_fixed_height(26);
        plain.set_placeholder(placeholder);
        plain.set_visible(false);
        visible_box = plain;

        auto& toggle = input_row.add<GUI::Button>("Show"_string);
        toggle.set_fixed_width(58);
        toggle.set_fixed_height(26);
        toggle_button = toggle;
    }
    total_height += 8 + 26;

    // Remember checkbox (hidden for non-password PAM prompts).
    RefPtr<GUI::CheckBox> remember_box;
    if (!echo_input && !hide_remember) {
        auto& box = main_widget->add<GUI::CheckBox>("Remember password until screen locks or session ends"_string);
        box.set_checked(false);
        box.set_fixed_height(22);
        remember_box = box;
        total_height += 8 + 22;
    }

    // Optional collapsible details section (action_id + details dictionary).
    bool has_details = !action_id.is_empty() || !details_text.is_empty();
    constexpr int details_panel_height = 72;
    RefPtr<GUI::TextEditor> details_editor;
    RefPtr<GUI::Button> details_button;
    bool details_expanded = false;

    if (has_details) {
        StringBuilder details_builder;
        if (!action_id.is_empty())
            details_builder.appendff("Action: {}\n", action_id);
        if (!details_text.is_empty())
            details_builder.append(details_text);

        auto& editor = main_widget->add<GUI::TextEditor>();
        editor.set_mode(GUI::TextEditor::Mode::ReadOnly);
        editor.set_text(details_builder.string_view());
        editor.set_fixed_height(details_panel_height);
        editor.set_visible(false);
        details_editor = editor;
    }

    // Action buttons row.
    auto& buttons = main_widget->add<GUI::Widget>();
    buttons.set_layout<GUI::HorizontalBoxLayout>(6);
    buttons.set_fixed_height(28);

    if (has_details) {
        auto& toggle = buttons.add<GUI::Button>("Details ▸"_string);
        toggle.set_fixed_width(86);
        details_button = toggle;
    }

    buttons.add_spacer();

    auto& cancel_button = buttons.add<GUI::Button>("Cancel"_string);
    cancel_button.set_fixed_width(92);

    auto& ok_button = buttons.add<GUI::Button>("Authenticate"_string);
    ok_button.set_fixed_width(112);
    ok_button.set_default(true);
    ok_button.set_enabled(false);
    total_height += 8 + 28;

    int base_height = total_height;
    window->resize(dialog_width, base_height);

    auto current_text = [&]() -> ByteString {
        if (showing_plaintext && visible_box)
            return visible_box->text();
        if (password_box)
            return password_box->text();
        return {};
    };

    auto update_ok_enabled = [&] {
        ok_button.set_enabled(!current_text().is_empty());
    };

    bool accepted = false;
    auto submit = [&] {
        if (current_text().is_empty())
            return;
        accepted = true;
        app->quit();
    };
    auto cancel = [&] {
        accepted = false;
        app->quit();
    };

    if (password_box) {
        password_box->on_change = [&] { update_ok_enabled(); };
        password_box->on_return_pressed = [&] { submit(); };
        password_box->on_escape_pressed = [&] { cancel(); };
    }
    if (visible_box) {
        visible_box->on_change = [&] { update_ok_enabled(); };
        visible_box->on_return_pressed = [&] { submit(); };
        visible_box->on_escape_pressed = [&] { cancel(); };
    }

    if (toggle_button && password_box && visible_box) {
        toggle_button->on_click = [&](auto) {
            if (showing_plaintext) {
                password_box->set_text(visible_box->text());
                visible_box->set_visible(false);
                password_box->set_visible(true);
                password_box->set_focus(true);
                toggle_button->set_text("Show"_string);
                showing_plaintext = false;
            } else {
                visible_box->set_text(password_box->text());
                password_box->set_visible(false);
                visible_box->set_visible(true);
                visible_box->set_focus(true);
                toggle_button->set_text("Hide"_string);
                showing_plaintext = true;
            }
            update_ok_enabled();
        };
    }

    if (details_button && details_editor) {
        details_button->on_click = [&](auto) {
            details_expanded = !details_expanded;
            details_editor->set_visible(details_expanded);
            details_button->set_text(details_expanded ? "Details ▾"_string : "Details ▸"_string);
            window->resize(dialog_width, details_expanded ? base_height + details_panel_height + 8 : base_height);
        };
    }

    ok_button.on_click = [&](auto) { submit(); };
    cancel_button.on_click = [&](auto) { cancel(); };

    window->show();
    if (showing_plaintext && visible_box)
        visible_box->set_focus(true);
    else if (password_box)
        password_box->set_focus(true);

    app->exec();

    if (!accepted)
        return 1;

    StringBuilder output;
    output.append(current_text());
    output.append('\n');
    output.append((remember_box && remember_box->is_checked()) ? '1' : '0');
    output.append('\n');
    if (identity_combo) {
        auto selected = identity_combo->text();
        if (!selected.is_empty()) {
            output.append("USER:"sv);
            output.append(selected);
            output.append('\n');
        }
    }

    if (!write_all(output_fd, output.string_view().bytes()))
        return 1;
    return 0;
}
