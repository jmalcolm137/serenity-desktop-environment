/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// FileChooserDialog — the SDE file chooser for the freedesktop FileChooser portal
// (R5). It is a plain SerenityOS LibGUI app wrapping GUI::FilePicker: the
// xdg-desktop-portal-serenity backend launches it with the mode/title/folder from
// the portal options, the user picks a path, and it is printed to stdout (exit 0);
// Cancel exits 1 with no output.

#include <LibCore/ArgsParser.h>
#include <LibCore/StandardPaths.h>
#include <LibGUI/Application.h>
#include <LibGUI/FilePicker.h>
#include <LibMain/Main.h>

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    auto app = TRY(GUI::Application::create(arguments));

    StringView mode { "open"sv };
    StringView title { "Open File"sv };
    StringView name { "Untitled"sv };
    ByteString folder = Core::StandardPaths::home_directory();
    bool multiple = false;

    Core::ArgsParser args_parser;
    args_parser.add_option(mode, "Mode: open, save or folder", "mode", 'm', "mode");
    args_parser.add_option(title, "Dialog title", "title", 't', "title");
    args_parser.add_option(name, "Suggested file name", "name", 'n', "name");
    args_parser.add_option(folder, "Starting folder", "folder", 'f', "folder");
    args_parser.add_option(multiple, "Allow multiple files", "multiple");
    args_parser.parse(arguments);

    auto picker_mode = GUI::FilePicker::Mode::Open;
    if (mode == "save"sv)
        picker_mode = GUI::FilePicker::Mode::Save;
    else if (mode == "folder"sv)
        picker_mode = GUI::FilePicker::Mode::OpenFolder;
    else if (multiple)
        picker_mode = GUI::FilePicker::Mode::OpenMultiple;

    auto picker = GUI::FilePicker::construct(nullptr, picker_mode, name, folder);
    picker->set_title(title);

    if (picker->exec() == GUI::Dialog::ExecResult::OK) {
        if (auto const& file = picker->selected_file(); file.has_value())
            outln("{}", file.value());
        return 0;
    }
    return 1;
}
