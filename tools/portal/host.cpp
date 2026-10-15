/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Host helpers shared by the portal modules (see host.h).

#include "host.h"

#include <AK/Array.h>
#include <AK/Function.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/StringBuilder.h>
#include <AK/Vector.h>
#include <LibCore/Environment.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibCore/Notifier.h>
#include <LibCore/Process.h>
#include <LibCore/System.h>

#include <errno.h>
#include <fcntl.h>
#include <sys/wait.h>
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

namespace {

// One in-flight asynchronous command. Held in running_commands() for its whole
// life, so the notifier callback always has a valid object, and released a turn
// after the callback (destroying a notifier from inside its own dispatch is not
// safe).
struct AsyncCommand {
    explicit AsyncCommand(Core::Process&& process)
        : process(move(process))
    {
    }

    Core::Process process;
    Function<void(Optional<ByteString>)> on_done;
    RefPtr<Core::Notifier> notifier;
    StringBuilder buffer;
    int read_fd { -1 };
};

Vector<NonnullOwnPtr<AsyncCommand>>& running_commands()
{
    static Vector<NonnullOwnPtr<AsyncCommand>> commands;
    return commands;
}

void async_command_readable(AsyncCommand& command)
{
    char chunk[4096];
    for (;;) {
        ssize_t read_count = ::read(command.read_fd, chunk, sizeof(chunk));
        if (read_count > 0) {
            command.buffer.append(StringView { chunk, static_cast<size_t>(read_count) });
            continue;
        }
        if (read_count < 0 && errno == EINTR)
            continue;
        if (read_count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;
        break; // EOF (or a read error): the helper is finishing
    }

    int wait_status = 0;
    (void)::waitpid(command.process.pid(), &wait_status, 0);
    if (command.notifier)
        command.notifier->close();
    if (command.read_fd >= 0) {
        ::close(command.read_fd);
        command.read_fd = -1;
    }

    auto output = Optional<ByteString> { command.buffer.string_view().trim_whitespace().to_byte_string() };
    auto on_done = move(command.on_done);
    on_done(output);

    // Drop the command on the next loop turn: its notifier is being dispatched
    // right now, so destroying it here would be unsafe.
    auto* command_pointer = &command;
    Core::deferred_invoke([command_pointer] {
        auto& commands = running_commands();
        for (size_t i = 0; i < commands.size(); ++i) {
            if (commands[i].ptr() == command_pointer) {
                commands.remove(i);
                return;
            }
        }
    });
}

}

void run_command_async(Vector<ByteString> arguments, Function<void(Optional<ByteString>)> on_done)
{
    if (arguments.is_empty()) {
        on_done(Optional<ByteString> {});
        return;
    }

    Vector<ByteString> child_arguments;
    for (size_t i = 1; i < arguments.size(); ++i)
        child_arguments.append(arguments[i]);

    Core::ProcessSpawnOptions options {
        .name = arguments.first(),
        .executable = arguments.first(),
        .search_for_executable_in_path = true,
        .arguments = child_arguments,
    };

    auto pipe = Core::System::pipe2(O_CLOEXEC | O_NONBLOCK);
    if (pipe.is_error()) {
        on_done(Optional<ByteString> {});
        return;
    }
    int read_fd = pipe.value()[0];
    int write_fd = pipe.value()[1];
    options.file_actions.append(Core::FileAction::DuplicateFile { write_fd, STDOUT_FILENO });
    options.file_actions.append(Core::FileAction::CloseFile { read_fd });

    auto process = Core::Process::spawn(options);
    ::close(write_fd);
    if (process.is_error()) {
        ::close(read_fd);
        on_done(Optional<ByteString> {});
        return;
    }

    auto command = make<AsyncCommand>(process.release_value());
    command->on_done = move(on_done);
    command->read_fd = read_fd;
    auto* command_pointer = command.ptr();
    command->notifier = Core::Notifier::construct(read_fd, Core::Notifier::Type::Read);
    command->notifier->on_activation = [command_pointer] { async_command_readable(*command_pointer); };
    running_commands().append(move(command));
}

ErrorOr<void> spawn_async(Vector<ByteString> const& arguments)
{
    if (arguments.is_empty())
        return Error::from_errno(EINVAL);

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
    auto process = TRY(Core::Process::spawn(options));
    // We do not wait for it: it is a helper (an opener, a launcher) whose exit
    // status says nothing about the request.
    (void)process.disown();
    return {};
}

}
