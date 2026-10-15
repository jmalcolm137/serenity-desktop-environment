/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Host helpers shared by the portal modules (see host.h).

#include "host.h"

#include <AK/Array.h>
#include <LibCore/Environment.h>
#include <LibCore/File.h>
#include <LibCore/Process.h>
#include <LibCore/System.h>

#include <fcntl.h>
#include <unistd.h>

namespace Portal {

Optional<ByteString> environment(StringView name)
{
    if (auto value = Core::Environment::get(name); value.has_value())
        return value->to_byte_string();
    return {};
}

ByteString runtime_dir()
{
    if (auto value = environment("XDG_RUNTIME_DIR"sv); value.has_value() && !value->is_empty())
        return value.value();
    return ByteString { "/tmp" };
}

ByteString state_file(StringView name)
{
    ByteString base;
    if (auto state = environment("SDE_STATE_DIR"sv); state.has_value() && !state->is_empty())
        base = state.value();
    else if (auto config = environment("XDG_CONFIG_HOME"sv); config.has_value() && !config->is_empty())
        base = ByteString::formatted("{}/sde", config.value());
    else if (auto home = environment("HOME"sv); home.has_value() && !home->is_empty())
        base = ByteString::formatted("{}/.config/sde", home.value());
    else
        base = ByteString { "/tmp/sde" };

    (void)Core::System::mkdir(base, 0700);
    return ByteString::formatted("{}/{}", base, name);
}

ErrorOr<Optional<ByteString>> run_command(Vector<ByteString> const& arguments, bool capture_stdout)
{
    if (arguments.is_empty())
        return Error::from_errno(EINVAL);

    // Core::Process::spawn puts the executable in argv[0] itself, so the
    // arguments it is given are the ones *after* argv[0].
    Vector<ByteString> child_arguments;
    for (size_t i = 1; i < arguments.size(); ++i)
        child_arguments.append(arguments[i]);

    Core::ProcessSpawnOptions options {
        .name = arguments.first(),
        .executable = arguments.first(),
        .search_for_executable_in_path = true,
        .arguments = child_arguments,
    };

    int read_fd = -1;
    int write_fd = -1;
    if (capture_stdout) {
        auto pipe = TRY(Core::System::pipe2(O_CLOEXEC));
        read_fd = pipe[0];
        write_fd = pipe[1];
        options.file_actions.append(Core::FileAction::DuplicateFile { write_fd, STDOUT_FILENO });
        options.file_actions.append(Core::FileAction::CloseFile { read_fd });
    }

    auto process = TRY(Core::Process::spawn(options));
    if (write_fd >= 0)
        ::close(write_fd);

    Optional<ByteString> output;
    if (read_fd >= 0) {
        auto file = TRY(Core::File::adopt_fd(read_fd, Core::File::OpenMode::Read));
        auto contents = TRY(file->read_until_eof());
        StringView text { reinterpret_cast<char const*>(contents.data()), contents.size() };
        output = text.trim_whitespace().to_byte_string();
    }

    (void)process.wait_for_termination();
    return output;
}

void spawn_async(Vector<ByteString> const& arguments)
{
    if (arguments.is_empty())
        return;

    // Core::Process::spawn puts the executable in argv[0] itself, so the arguments
    // it is given are the ones *after* argv[0].
    Vector<ByteString> child_arguments;
    for (size_t i = 1; i < arguments.size(); ++i)
        child_arguments.append(arguments[i]);

    Core::ProcessSpawnOptions options {
        .name = arguments.first(),
        .executable = arguments.first(),
        .search_for_executable_in_path = true,
        .arguments = child_arguments,
    };
    (void)Core::Process::spawn(options);
}

}
