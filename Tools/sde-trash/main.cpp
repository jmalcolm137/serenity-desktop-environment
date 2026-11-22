/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// sde-trash — freedesktop.org Trash CLI ($XDG_DATA_HOME/Trash).
//
// Usage:
//   sde-trash trash <path...>
//   sde-trash list
//   sde-trash restore <id>
//   sde-trash empty

#include <AK/Format.h>
#include <LibMain/Main.h>
#include <LibXDG/Trash.h>

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    if (arguments.strings.size() < 2) {
        warnln("usage: sde-trash <trash <path...> | list | restore <id> | empty>");
        return 2;
    }

    auto cmd = arguments.strings[1];
    if (cmd == "trash"sv) {
        if (arguments.strings.size() < 3) {
            warnln("usage: sde-trash trash <path...>");
            return 2;
        }
        for (size_t i = 2; i < arguments.strings.size(); ++i) {
            auto id = TRY(XDG::trash_file(arguments.strings[i]));
            outln("{}", id);
        }
        return 0;
    }
    if (cmd == "list"sv) {
        for (auto const& entry : XDG::list_trash())
            outln("{}\t{}\t{}", entry.id, entry.deletion_date, entry.original_path);
        return 0;
    }
    if (cmd == "restore"sv) {
        if (arguments.strings.size() != 3) {
            warnln("usage: sde-trash restore <id>");
            return 2;
        }
        TRY(XDG::restore_trashed_file(arguments.strings[2]));
        return 0;
    }
    if (cmd == "empty"sv) {
        TRY(XDG::empty_trash());
        return 0;
    }

    warnln("sde-trash: unknown subcommand '{}'", cmd);
    return 2;
}
