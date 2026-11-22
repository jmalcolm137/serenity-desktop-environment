/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// sde-open — freedesktop.org MIME/URI opener backed by LibXDG.
//
// Resolves the MIME type via shared-mime-info (globs2/aliases/subclasses) and
// looks up the default .desktop application via mimeapps.list, expanding Exec=
// field codes (%f/%F/%u/%U/%i/%c/%k), Path= and Terminal=true. Used as the
// SDE_PORTAL_OPENER for xdg-desktop-portal-serenity and usable directly from
// scripts or LaunchServer.

#include <AK/ByteString.h>
#include <AK/StringBuilder.h>
#include <AK/Vector.h>
#include <LibCore/ArgsParser.h>
#include <LibMain/Main.h>
#include <LibXDG/DesktopEntry.h>
#include <LibXDG/MimeApps.h>
#include <LibXDG/MimeDatabase.h>
#include <LibXDG/RecentlyUsed.h>

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static ErrorOr<int> spawn_desktop_entry(XDG::DesktopEntry const& entry, Vector<ByteString> const& targets)
{
    auto expanded = entry.expand_exec(targets);
    if (expanded.is_empty())
        return Error::from_string_literal("desktop entry has empty Exec=");

    Vector<ByteString> argv;
    if (entry.terminal) {
        char const* terminal = getenv("SDE_TERMINAL");
        argv.append(terminal && *terminal ? ByteString(terminal) : ByteString { "Terminal" });
        argv.append("-e");
    }
    for (auto& arg : expanded)
        argv.append(move(arg));

    pid_t pid = ::fork();
    if (pid < 0)
        return Error::from_errno(errno);
    if (pid > 0)
        return 0;

    (void)::setsid();
    int devnull = ::open("/dev/null", O_RDWR);
    if (devnull >= 0) {
        (void)::dup2(devnull, STDIN_FILENO);
        (void)::dup2(devnull, STDOUT_FILENO);
        (void)::dup2(devnull, STDERR_FILENO);
        if (devnull > STDERR_FILENO)
            ::close(devnull);
    }

    if (!entry.path.is_empty())
        (void)::chdir(entry.path.characters());

    Vector<char*> c_argv;
    c_argv.ensure_capacity(argv.size() + 1);
    for (auto& arg : argv)
        c_argv.unchecked_append(const_cast<char*>(arg.characters()));
    c_argv.unchecked_append(nullptr);

    ::execvp(c_argv[0], c_argv.data());
    _exit(127);
}

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    bool dry_run = false;
    bool print_mime = false;
    StringView target;

    Core::ArgsParser args_parser;
    args_parser.add_option(dry_run, "Print resolved desktop entry and command without launching", "dry-run", 'n');
    args_parser.add_option(print_mime, "Print only the resolved MIME type", "mime", 'm');
    args_parser.add_positional_argument(target, "File path or URI to open", "target");
    args_parser.parse(arguments);

    auto mime_db = XDG::MimeDatabase::load();
    auto mime = mime_db.mime_type_for_uri(target);
    if (print_mime) {
        outln("{}", mime);
        return 0;
    }

    auto app = XDG::default_application_for_mime(mime, mime_db);
    if (!app.has_value()) {
        warnln("sde-open: no application registered for '{}' ({})", mime, target);
        return 1;
    }

    Vector<ByteString> targets;
    targets.append(target.to_byte_string());

    if (dry_run) {
        auto argv = app->expand_exec(targets);
        StringBuilder cmd;
        for (size_t i = 0; i < argv.size(); ++i) {
            if (i > 0)
                cmd.append(' ');
            cmd.append(argv[i]);
        }
        outln("{}\t{}\t{}", mime, app->id, cmd.string_view());
        return 0;
    }

    (void)XDG::record_recently_used(target, mime, app->name, app->exec);
    return spawn_desktop_entry(app.value(), targets);
}
