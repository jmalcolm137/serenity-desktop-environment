/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "AuthSession.h"
#include "dialog_output.h"

#include <AK/Debug.h>
#include <AK/Format.h>
#include <AK/StringBuilder.h>
#include <LibCore/EventLoop.h>
#include <LibCore/System.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

namespace SDE {

namespace {

constexpr int dialog_output_fd = 3;

}

AuthSession::AuthSession(PasswordCache& cache)
    : m_cache(cache)
{
    wire_helper_callbacks();
}

AuthSession::~AuthSession()
{
    close_dialog();
    m_helper.close();
    wipe(m_last_password);
    wipe(m_pending_switched_password);
}

void AuthSession::wire_helper_callbacks()
{
    m_helper.on_prompt_echo_off = [this](ByteString prompt) {
        if (!m_request.has_value() || m_completion_scheduled)
            return;
        handle_prompt(move(prompt), false);
    };
    m_helper.on_prompt_echo_on = [this](ByteString prompt) {
        if (!m_request.has_value() || m_completion_scheduled)
            return;
        handle_prompt(move(prompt), true);
    };
    m_helper.on_error_message = [this](ByteString message) {
        dbgln("sde-polkit-agent: PAM error: {}", message);
        if (!message.is_empty())
            m_last_error = move(message);
    };
    m_helper.on_text_info = [](ByteString message) {
        dbgln("sde-polkit-agent: PAM info: {}", message);
    };
    m_helper.on_success = [this] {
        complete(true);
    };
    m_helper.on_failure = [this] {
        handle_helper_failure();
    };
    m_helper.on_unexpected_eof = [this] {
        complete(false);
    };
}

void AuthSession::start(AuthRequest request)
{
    m_request = move(request);
    m_username = m_request->identities.first();
    m_last_error = {};
    wipe(m_last_password);
    m_last_password.clear();
    wipe(m_pending_switched_password);
    m_pending_switched_password.clear();
    m_remember = false;
    m_used_cached_password = false;
    m_retried_without_cache = false;
    m_completion_scheduled = false;
    m_attempts = 0;

    if (!m_helper.start(m_username, m_request->cookie))
        complete(false);
}

void AuthSession::cancel()
{
    complete(false);
}

void AuthSession::handle_prompt(ByteString prompt, bool echo_input)
{
    // If the user switched identity in the dialog, we restarted the helper for
    // the new user and buffered the password they just typed.
    if (!echo_input && m_pending_switched_password.has_value()) {
        m_last_password = m_pending_switched_password.release_value();
        m_pending_switched_password.clear();
        m_used_cached_password = false;
        if (!m_helper.send_response(m_last_password->bytes()))
            complete(false);
        return;
    }

    // Only consult the session cache for standard hidden password prompts.
    if (!echo_input && !m_retried_without_cache) {
        if (auto cached = m_cache.get(m_username); cached.has_value()) {
            m_used_cached_password = true;
            m_last_password = cached.release_value();
            if (!m_helper.send_response(m_last_password->bytes()))
                complete(false);
            return;
        }
    }

    spawn_dialog(prompt, echo_input);
}

void AuthSession::spawn_dialog(ByteString const& prompt, bool echo_input)
{
    if (m_dialog_process.has_value() || !m_request.has_value())
        return;

    auto dialog = environment_or("SDE_AUTH_DIALOG"sv, "sde-auth-dialog"sv);
    Vector<ByteString> arguments;
    arguments.append("--output-fd"sv);
    arguments.append(ByteString::number(dialog_output_fd));
    arguments.append("--message"sv);
    arguments.append(m_request->message);
    if (!m_username.is_empty()) {
        arguments.append("--username"sv);
        arguments.append(m_username);
    }
    if (m_request->identities.size() > 1) {
        StringBuilder csv;
        for (size_t i = 0; i < m_request->identities.size(); ++i) {
            if (i > 0)
                csv.append(',');
            csv.append(m_request->identities[i]);
        }
        arguments.append("--identities"sv);
        arguments.append(csv.to_byte_string());
    }
    if (!m_request->action_id.is_empty()) {
        arguments.append("--action-id"sv);
        arguments.append(m_request->action_id);
    }
    if (!m_request->details.is_empty()) {
        arguments.append("--details"sv);
        arguments.append(m_request->details);
    }
    if (!m_request->icon_name.is_empty()) {
        arguments.append("--icon"sv);
        arguments.append(m_request->icon_name);
    }
    if (!m_last_error.is_empty()) {
        arguments.append("--error"sv);
        arguments.append(m_last_error);
    }
    if (!prompt.is_empty()) {
        arguments.append("--prompt"sv);
        arguments.append(prompt);
    }
    if (echo_input) {
        arguments.append("--echo"sv);
        arguments.append("--hide-remember"sv);
    }

    auto pipe = Core::System::pipe2(O_CLOEXEC);
    if (pipe.is_error()) {
        warnln("sde-polkit-agent: cannot create the dialog pipe: {}", pipe.error());
        complete(false);
        return;
    }
    int read_fd = pipe.value()[0];
    int write_fd = pipe.value()[1];

    // Ensure write_fd != dialog_output_fd so DuplicateFile clears FD_CLOEXEC
    // on dialog_output_fd and CloseFile cleanly closes write_fd in the child.
    if (write_fd == dialog_output_fd) {
        int relocated = ::fcntl(write_fd, F_DUPFD_CLOEXEC, dialog_output_fd + 1);
        ::close(write_fd);
        if (relocated < 0) {
            ::close(read_fd);
            warnln("sde-polkit-agent: cannot relocate dialog output fd");
            complete(false);
            return;
        }
        write_fd = relocated;
    }

    Core::ProcessSpawnOptions options {
        .name = dialog,
        .executable = dialog,
        .search_for_executable_in_path = true,
        .arguments = arguments,
    };
    options.file_actions.append(Core::FileAction::DuplicateFile { write_fd, dialog_output_fd });
    options.file_actions.append(Core::FileAction::CloseFile { read_fd });
    options.file_actions.append(Core::FileAction::CloseFile { write_fd });

    auto process = Core::Process::spawn(options);
    ::close(write_fd);
    if (process.is_error()) {
        ::close(read_fd);
        warnln("sde-polkit-agent: cannot run {}: {}", dialog, process.error());
        complete(false);
        return;
    }

    m_dialog_process = process.release_value();
    m_dialog_read_fd = read_fd;
    m_dialog_echo_mode = echo_input;
    wipe(m_dialog_buffer);
    set_fd_nonblocking(m_dialog_read_fd);
    m_dialog_notifier = Core::Notifier::construct(m_dialog_read_fd, Core::Notifier::Type::Read);
    m_dialog_notifier->on_activation = [this] { on_dialog_readable(); };
}

void AuthSession::on_dialog_readable()
{
    if (m_dialog_read_fd < 0)
        return;

    char chunk[4096];
    for (;;) {
        ssize_t count = ::read(m_dialog_read_fd, chunk, sizeof(chunk));
        if (count > 0) {
            (void)m_dialog_buffer.try_append(chunk, static_cast<size_t>(count));
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;

        break;
    }

    // Wipe stack chunk before returning.
    explicit_bzero(chunk, sizeof(chunk));

    // Disable, but do not destroy, the notifier from inside its callback.
    if (m_dialog_notifier)
        m_dialog_notifier->close();
    if (m_dialog_read_fd >= 0) {
        ::close(m_dialog_read_fd);
        m_dialog_read_fd = -1;
    }

    auto output = move(m_dialog_buffer);
    m_dialog_buffer.clear();
    if (!m_dialog_process.has_value()) {
        wipe(output);
        return;
    }
    auto process = m_dialog_process.release_value();
    m_dialog_process.clear();

    // Reap and parse after this notifier callback returns: destroying the
    // dialog state from inside its own dispatch is not safe.
    Core::deferred_invoke([this, output = move(output), process = move(process)]() mutable {
        m_dialog_notifier = nullptr;
        auto result = process.wait_for_termination();
        handle_dialog_output(move(output), !result.is_error() && result.value());
    });
}

void AuthSession::handle_dialog_output(ByteBuffer output, bool exited_successfully)
{
    auto parsed = exited_successfully ? parse_dialog_output_secure(output.bytes()) : Optional<SecureDialogOutput> {};
    wipe(output);

    if (!m_request.has_value()) {
        if (parsed.has_value())
            parsed->wipe();
        return;
    }

    if (!exited_successfully || !parsed.has_value()) {
        if (parsed.has_value())
            parsed->wipe();
        complete(false);
        return;
    }

    // Check if the user selected a different identity in the ComboBox.
    if (parsed->selected_user.has_value()
        && *parsed->selected_user != m_username
        && m_request->identities.contains_slow(*parsed->selected_user)) {
        m_username = *parsed->selected_user;
        m_last_error = {};
        m_remember = m_dialog_echo_mode ? false : parsed->remember;
        m_used_cached_password = false;
        m_retried_without_cache = false;
        wipe(m_pending_switched_password);
        m_pending_switched_password = move(parsed->password);
        restart_helper_deferred();
        return;
    }

    wipe(m_last_password);
    m_last_password = move(parsed->password);
    m_remember = m_dialog_echo_mode ? false : parsed->remember;
    m_used_cached_password = false;

    if (!m_helper.send_response(m_last_password->bytes()))
        complete(false);
}

void AuthSession::handle_helper_failure()
{
    // A cached password can expire (the user changed it). Retry without cache
    // and without counting against the manual attempt limit.
    if (m_used_cached_password && !m_retried_without_cache) {
        m_cache.clear(m_username);
        m_retried_without_cache = true;
        m_used_cached_password = false;
        wipe(m_last_password);
        m_last_password.clear();
        m_remember = false;
        m_last_error = {};
        restart_helper_deferred();
        return;
    }

    // Allow inline retries on wrong password before failing the polkit request.
    if (++m_attempts < max_attempts) {
        if (m_last_error.is_empty())
            m_last_error = "Incorrect password. Please try again."sv;
        m_retried_without_cache = true;
        m_used_cached_password = false;
        wipe(m_last_password);
        m_last_password.clear();
        m_remember = false;
        restart_helper_deferred();
        return;
    }

    complete(false);
}

void AuthSession::restart_helper_deferred()
{
    m_completion_scheduled = true;
    Core::deferred_invoke([this] {
        m_completion_scheduled = false;
        if (!m_request.has_value())
            return;
        if (m_helper.start(m_username, m_request->cookie))
            return;
        complete(false);
    });
}

void AuthSession::complete(bool gained)
{
    if (m_completion_scheduled)
        return;
    m_completion_scheduled = true;
    Core::deferred_invoke([this, gained] { finish(gained); });
}

void AuthSession::finish(bool gained)
{
    m_helper.close();
    close_dialog();

    if (gained) {
        if (m_remember && m_last_password.has_value())
            m_cache.set(m_username, m_last_password->bytes());
    } else if (m_used_cached_password) {
        m_cache.clear(m_username);
    }

    wipe(m_last_password);
    m_last_password.clear();
    wipe(m_pending_switched_password);
    m_pending_switched_password.clear();
    m_remember = false;
    m_used_cached_password = false;
    m_retried_without_cache = false;
    m_attempts = 0;
    m_username = {};
    m_last_error = {};

    if (m_request.has_value()) {
        auto request = m_request.release_value();
        m_request.clear();
        if (request.pending)
            request.pending->reply();
    }
    m_completion_scheduled = false;

    if (on_finished)
        on_finished();
}

void AuthSession::close_dialog()
{
    if (m_dialog_notifier) {
        m_dialog_notifier->close();
        m_dialog_notifier = nullptr;
    }
    if (m_dialog_read_fd >= 0) {
        ::close(m_dialog_read_fd);
        m_dialog_read_fd = -1;
    }
    if (m_dialog_process.has_value()) {
        auto process = move(*m_dialog_process);
        m_dialog_process.clear();
        if (process.pid() > 0)
            ::kill(process.pid(), SIGTERM);
        (void)process.wait_for_termination();
    }
    wipe(m_dialog_buffer);
}

void AuthSession::wipe(Optional<ByteBuffer>& buffer)
{
    if (!buffer.has_value())
        return;
    wipe(*buffer);
}

void AuthSession::wipe(ByteBuffer& buffer)
{
    if (!buffer.is_empty())
        explicit_bzero(buffer.data(), buffer.size());
    buffer.clear();
}

}
