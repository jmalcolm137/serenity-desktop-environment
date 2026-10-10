/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibSerenitySystem/SerenitySystem.h>
#include <AK/ByteString.h>
#include <AK/Debug.h>
#include <AK/Optional.h>
#include <AK/ScopeGuard.h>
#include <LibCore/File.h>
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>

namespace SerenitySystem {

ErrorOr<void> update_utmp(StringView tty, pid_t pid, bool create)
{
    // The SDE owns the session/utmp service. Until it exists, record the event
    // and never fail the caller: SerenityOS's /bin/utmpupdate has no host
    // equivalent and losing the utmp record must not block a terminal.
    dbgln("SerenitySystem: utmp {} tty='{}' pid={}", create ? "login"sv : "logout"sv, tty, pid);
    return {};
}

static ErrorOr<pid_t> parent_pid_of(pid_t pid)
{
    auto path = ByteString::formatted("/proc/{}/stat", pid);
    auto file = TRY(Core::File::open(path, Core::File::OpenMode::Read));
    auto data = TRY(file->read_until_eof());

    StringView text { reinterpret_cast<char const*>(data.data()), data.size() };

    // The comm field is parenthesised and may itself contain ')' characters, so
    // the ppid is the second field after the final ')'.
    Optional<size_t> close_paren;
    for (size_t i = 0; i < text.length(); ++i) {
        if (text[i] == ')')
            close_paren = i;
    }
    if (!close_paren.has_value())
        return Error::from_string_literal("malformed /proc/<pid>/stat");

    auto rest = text.substring_view(*close_paren + 1);
    size_t i = 0;
    while (i < rest.length() && rest[i] == ' ')
        ++i;
    if (i >= rest.length())
        return Error::from_string_literal("malformed /proc/<pid>/stat");
    ++i; // Skip the single state character.
    while (i < rest.length() && rest[i] == ' ')
        ++i;

    long ppid = 0;
    bool any_digits = false;
    while (i < rest.length() && rest[i] >= '0' && rest[i] <= '9') {
        ppid = ppid * 10 + (rest[i] - '0');
        ++i;
        any_digits = true;
    }
    if (!any_digits)
        return Error::from_string_literal("malformed /proc/<pid>/stat");
    return static_cast<pid_t>(ppid);
}

ErrorOr<Vector<pid_t>> child_processes(pid_t parent)
{
    Vector<pid_t> children;

    auto* proc = opendir("/proc");
    if (!proc)
        return Error::from_errno(errno);
    ScopeGuard close_proc = [&] { closedir(proc); };

    while (auto* entry = readdir(proc)) {
        char* end = nullptr;
        long pid = strtol(entry->d_name, &end, 10);
        if (end == entry->d_name || *end != '\0' || pid <= 0)
            continue;
        if (pid == parent)
            continue;

        auto ppid = parent_pid_of(static_cast<pid_t>(pid));
        if (ppid.is_error())
            continue;
        if (ppid.value() != parent)
            continue;

        TRY(children.try_append(static_cast<pid_t>(pid)));
    }

    return children;
}

}
