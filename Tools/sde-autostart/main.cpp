/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// sde-autostart — freedesktop.org Desktop Application Autostart launcher.
//
// Discovers enabled .desktop entries across $XDG_CONFIG_HOME/autostart and
// $XDG_CONFIG_DIRS/autostart (with per-user basename override, Hidden,
// OnlyShowIn/NotShowIn, TryExec, Path and Terminal support) and launches them
// detached in their own session.

#include <AK/ByteString.h>
#include <AK/Optional.h>
#include <AK/StringBuilder.h>
#include <AK/Vector.h>
#include <LibCore/ArgsParser.h>
#include <LibMain/Main.h>
#include <LibXDG/Autostart.h>

#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

static void spawn_entry(XDG::AutostartEntry const& entry)
{
    Vector<ByteString> argv;
    if (entry.terminal) {
        char const* terminal = getenv("SDE_TERMINAL");
        argv.append(terminal && *terminal ? ByteString(terminal) : ByteString { "Terminal" });
        argv.append("-e");
    }
    for (auto const& arg : entry.argv)
        argv.append(arg);

    if (argv.is_empty())
        return;

    pid_t pid = ::fork();
    if (pid < 0) {
        warnln("sde-autostart: fork failed for {}: {}", entry.id, strerror(errno));
        return;
    }
    if (pid > 0)
        return;

    // Detach into a new session so autostart apps outlive this launcher and do
    // not inherit our controlling terminal or stdio.
    (void)::setsid();
    int devnull = ::open("/dev/null", O_RDWR);
    if (devnull >= 0) {
        (void)::dup2(devnull, STDIN_FILENO);
        (void)::dup2(devnull, STDOUT_FILENO);
        (void)::dup2(devnull, STDERR_FILENO);
        if (devnull > STDERR_FILENO)
            ::close(devnull);
    }

    if (entry.working_directory.has_value() && !entry.working_directory->is_empty())
        (void)::chdir(entry.working_directory->characters());

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
    StringView user_dir_override;

    Core::ArgsParser args_parser;
    args_parser.add_option(dry_run, "Print resolved autostart entries without launching them", "dry-run", 'n');
    args_parser.add_option(user_dir_override, "Override $XDG_CONFIG_HOME/autostart directory", "dir", 'd', "path");
    args_parser.parse(arguments);

    Optional<ByteString> dir_override;
    if (!user_dir_override.is_empty())
        dir_override = user_dir_override.to_byte_string();

    auto entries = XDG::discover_autostart_entries(move(dir_override));
    for (auto const& entry : entries) {
        StringBuilder cmd;
        for (size_t i = 0; i < entry.argv.size(); ++i) {
            if (i > 0)
                cmd.append(' ');
            cmd.append(entry.argv[i]);
        }
        if (dry_run) {
            outln("{}\t{}", entry.id, cmd.string_view());
        } else {
            outln("SDE session: autostart {}: {}", entry.id, cmd.string_view());
            spawn_entry(entry);
        }
    }

    return 0;
}
