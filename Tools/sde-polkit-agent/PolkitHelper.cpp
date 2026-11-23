/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "PolkitHelper.h"

#include <AK/Debug.h>
#include <AK/Format.h>
#include <AK/Vector.h>
#include <LibCore/Environment.h>
#include <LibCore/System.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

namespace SDE {

namespace {

constexpr StringView default_helper_socket = "/run/polkit/agent-helper.socket"sv;
constexpr StringView default_helper_binary = "/usr/lib/polkit-1/polkit-agent-helper-1"sv;

bool is_setuid_helper(StringView path)
{
    auto path_string = path.to_byte_string();
    struct stat status { };
    if (::stat(path_string.characters(), &status) != 0)
        return false;
    if (!S_ISREG(status.st_mode) || status.st_uid != 0 || (status.st_mode & S_ISUID) == 0)
        return false;
    return ::access(path_string.characters(), X_OK) == 0;
}

template<typename Callback>
void consume_lines(StringBuilder& buffer, Callback callback)
{
    auto view = buffer.string_view();
    size_t start = 0;
    for (;;) {
        auto newline = view.find('\n', start);
        if (!newline.has_value())
            break;
        callback(view.substring_view(start, *newline - start));
        start = *newline + 1;
    }
    if (start > 0) {
        auto rest = view.substring_view(start);
        buffer.clear();
        buffer.append(rest);
    }
}

}

ByteString environment_or(StringView name, StringView fallback)
{
    if (auto value = Core::Environment::get(name); value.has_value() && !value->is_empty())
        return value->to_byte_string();
    return fallback.to_byte_string();
}

ByteString unescape_helper_text(StringView text)
{
    StringBuilder builder;
    for (size_t i = 0; i < text.length(); ++i) {
        char character = text[i];
        if (character != '\\' || i + 1 >= text.length()) {
            builder.append(character);
            continue;
        }

        char next = text[++i];
        switch (next) {
        case 'a':
            builder.append('\a');
            break;
        case 'b':
            builder.append('\b');
            break;
        case 'f':
            builder.append('\f');
            break;
        case 'n':
            builder.append('\n');
            break;
        case 'r':
            builder.append('\r');
            break;
        case 't':
            builder.append('\t');
            break;
        case 'v':
            builder.append('\v');
            break;
        default:
            if (next >= '0' && next <= '7') {
                int value = next - '0';
                int digits = 1;
                while (digits < 3 && i + 1 < text.length() && text[i + 1] >= '0' && text[i + 1] <= '7') {
                    value = value * 8 + (text[++i] - '0');
                    ++digits;
                }
                builder.append(static_cast<char>(value));
            } else {
                builder.append(next);
            }
            break;
        }
    }
    return builder.to_byte_string();
}

bool write_all_fd(int fd, ReadonlyBytes bytes)
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
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pollfd descriptor { fd, POLLOUT, 0 };
            if (::poll(&descriptor, 1, 1000) <= 0)
                return false;
            continue;
        }
        return false;
    }
    return true;
}

bool write_all_fd(int fd, StringView text)
{
    return write_all_fd(fd, text.bytes());
}

void set_fd_nonblocking(int fd)
{
    int flags = ::fcntl(fd, F_GETFL);
    if (flags >= 0)
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

PolkitHelper::~PolkitHelper()
{
    close();
}

bool PolkitHelper::start(ByteString const& username, ByteString const& cookie)
{
    close();
    m_verdict_reached = false;

    auto helper = environment_or("SDE_POLKIT_HELPER_BIN"sv, default_helper_binary);

    // Prefer the traditional setuid helper when it is installed that way.
    // polkit 128's socket-activated helper has a peer-credential/pam_unix
    // regression that rejects correct passwords; the direct setuid helper
    // takes the stable path. Systems without the setuid bit keep using the
    // socket.
    if (is_setuid_helper(helper))
        return spawn_setuid_helper(helper, username, cookie);

    auto socket_path = environment_or("SDE_POLKIT_HELPER_SOCKET"sv, default_helper_socket);
    if (::access(socket_path.characters(), F_OK) == 0) {
        auto socket = Core::LocalSocket::connect(socket_path);
        if (!socket.is_error()) {
            m_socket = socket.release_value();
            auto fd = m_socket->fd();
            if (!fd.has_value()) {
                close();
                return false;
            }
            m_read_fd = *fd;
            m_write_fd = *fd;
            set_fd_nonblocking(m_read_fd);

            // The socket-activated helper reads the user first, then the
            // cookie, both newline terminated.
            if (!write_all_fd(m_write_fd, username) || !write_all_fd(m_write_fd, "\n"sv)
                || !write_all_fd(m_write_fd, cookie) || !write_all_fd(m_write_fd, "\n"sv)) {
                warnln("sde-polkit-agent: could not send the request to the polkit helper");
                close();
                return false;
            }

            m_notifier = Core::Notifier::construct(m_read_fd, Core::Notifier::Type::Read);
            m_notifier->on_activation = [this] { on_readable(); };
            return true;
        }
        dbgln("sde-polkit-agent: cannot connect to {}: {}", socket_path, socket.error());
    }

    return spawn_setuid_helper(helper, username, cookie);
}

bool PolkitHelper::spawn_setuid_helper(ByteString const& helper, ByteString const& username, ByteString const& cookie)
{
    if (::access(helper.characters(), X_OK) != 0) {
        warnln("sde-polkit-agent: no polkit helper at {} and no helper socket", helper);
        return false;
    }

    auto input = Core::System::pipe2(O_CLOEXEC);
    if (input.is_error()) {
        warnln("sde-polkit-agent: cannot create the helper input pipe: {}", input.error());
        return false;
    }
    auto output = Core::System::pipe2(O_CLOEXEC);
    if (output.is_error()) {
        ::close(input.value()[0]);
        ::close(input.value()[1]);
        warnln("sde-polkit-agent: cannot create the helper output pipe: {}", output.error());
        return false;
    }

    int input_read = input.value()[0];
    int input_write = input.value()[1];
    int output_read = output.value()[0];
    int output_write = output.value()[1];

    Vector<ByteString> arguments;
    arguments.append(username);
    Core::ProcessSpawnOptions options {
        .name = helper,
        .executable = helper,
        .arguments = arguments,
    };
    options.file_actions.append(Core::FileAction::DuplicateFile { input_read, STDIN_FILENO });
    options.file_actions.append(Core::FileAction::DuplicateFile { output_write, STDOUT_FILENO });
    options.file_actions.append(Core::FileAction::CloseFile { input_read });
    options.file_actions.append(Core::FileAction::CloseFile { input_write });
    options.file_actions.append(Core::FileAction::CloseFile { output_read });
    options.file_actions.append(Core::FileAction::CloseFile { output_write });

    auto process = Core::Process::spawn(options);
    ::close(input_read);
    ::close(output_write);
    if (process.is_error()) {
        ::close(input_write);
        ::close(output_read);
        warnln("sde-polkit-agent: cannot start {}: {}", helper, process.error());
        return false;
    }

    m_process = process.release_value();
    m_read_fd = output_read;
    m_write_fd = input_write;
    set_fd_nonblocking(m_read_fd);

    // The setuid helper expects the cookie on stdin and keeps reading stdin
    // for the PAM responses; the username is its argv[1]. Leave the write end
    // open for the rest of the session.
    bool wrote_cookie = write_all_fd(m_write_fd, cookie) && write_all_fd(m_write_fd, "\n"sv);
    if (!wrote_cookie) {
        warnln("sde-polkit-agent: could not send the cookie to the polkit helper");
        close();
        return false;
    }

    m_notifier = Core::Notifier::construct(m_read_fd, Core::Notifier::Type::Read);
    m_notifier->on_activation = [this] { on_readable(); };
    return true;
}

bool PolkitHelper::send_response(ReadonlyBytes response)
{
    if (m_write_fd < 0)
        return false;
    if (!write_all_fd(m_write_fd, response) || !write_all_fd(m_write_fd, "\n"sv)) {
        warnln("sde-polkit-agent: could not send the authentication response");
        return false;
    }
    return true;
}

void PolkitHelper::on_readable()
{
    if (m_read_fd < 0)
        return;

    ByteBuffer received;
    char chunk[4096];
    bool eof_or_error = false;
    for (;;) {
        ssize_t count = ::read(m_read_fd, chunk, sizeof(chunk));
        if (count > 0) {
            (void)received.try_append(chunk, static_cast<size_t>(count));
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;

        eof_or_error = true;
        break;
    }

    if (eof_or_error && m_notifier) {
        // Disable, but do not destroy, the notifier from inside its callback;
        // deferred cleanup closes it safely.
        m_notifier->close();
    }

    feed_bytes(received.bytes(), eof_or_error);
}

void PolkitHelper::feed_bytes(ReadonlyBytes data, bool eof)
{
    if (!data.is_empty())
        m_buffer.append(StringView { data });

    // Always drain complete lines BEFORE handling EOF: polkit-agent-helper-1
    // writes "SUCCESS\n" (or "FAILURE\n") and exits immediately, so both the
    // final line and EOF frequently arrive in the same non-blocking read loop.
    consume_lines(m_buffer, [this](StringView raw_line) {
        if (m_verdict_reached)
            return;
        if (handle_line(raw_line))
            m_verdict_reached = true;
    });

    if (eof && !m_verdict_reached) {
        m_verdict_reached = true;
        if (on_unexpected_eof)
            on_unexpected_eof();
    }
}

bool PolkitHelper::handle_line(StringView raw_line)
{
    auto line = unescape_helper_text(raw_line);

    if (line.starts_with("PAM_PROMPT_ECHO_OFF "sv)) {
        if (on_prompt_echo_off)
            on_prompt_echo_off(line.substring(20));
        return false;
    }
    if (line.starts_with("PAM_PROMPT_ECHO_ON "sv)) {
        if (on_prompt_echo_on)
            on_prompt_echo_on(line.substring(19));
        return false;
    }
    if (line.starts_with("PAM_ERROR_MSG "sv)) {
        if (on_error_message)
            on_error_message(line.substring(14));
        return false;
    }
    if (line.starts_with("PAM_TEXT_INFO "sv)) {
        if (on_text_info)
            on_text_info(line.substring(14));
        return false;
    }
    if (line == "SUCCESS"sv) {
        if (on_success)
            on_success();
        return true;
    }
    if (line == "FAILURE"sv) {
        if (on_failure)
            on_failure();
        return true;
    }

    warnln("sde-polkit-agent: unexpected line from the polkit helper: {}", line);
    if (on_unexpected_eof)
        on_unexpected_eof();
    return true;
}

void PolkitHelper::close()
{
    if (m_notifier) {
        m_notifier->close();
        m_notifier = nullptr;
    }
    if (m_socket) {
        m_socket = nullptr;
        m_read_fd = -1;
        m_write_fd = -1;
    }
    if (m_read_fd >= 0) {
        ::close(m_read_fd);
        m_read_fd = -1;
    }
    if (m_write_fd >= 0) {
        ::close(m_write_fd);
        m_write_fd = -1;
    }
    if (m_process.has_value()) {
        auto process = move(*m_process);
        m_process.clear();
        if (process.pid() > 0)
            ::kill(process.pid(), SIGTERM);
        (void)process.wait_for_termination();
    }
    m_buffer.clear();
    m_verdict_reached = false;
}

}
