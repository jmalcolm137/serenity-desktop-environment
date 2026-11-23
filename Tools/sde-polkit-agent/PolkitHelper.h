/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Function.h>
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/RefPtr.h>
#include <AK/Span.h>
#include <AK/StringBuilder.h>
#include <AK/StringView.h>
#include <LibCore/Notifier.h>
#include <LibCore/Process.h>
#include <LibCore/Socket.h>

namespace SDE {

// Decodes g_strescape() sequences emitted by polkit-agent-helper-1.
ByteString unescape_helper_text(StringView text);

// Writes all bytes to a (possibly non-blocking) fd, polling for POLLOUT on EAGAIN.
bool write_all_fd(int fd, ReadonlyBytes bytes);
bool write_all_fd(int fd, StringView text);
void set_fd_nonblocking(int fd);
ByteString environment_or(StringView name, StringView fallback);

// Manages the line-oriented transport to polkit's trusted helper
// (either the setuid polkit-agent-helper-1 binary or the systemd-activated
// /run/polkit/agent-helper.socket).
class PolkitHelper {
public:
    PolkitHelper() = default;
    ~PolkitHelper();

    bool start(ByteString const& username, ByteString const& cookie);
    bool send_response(ReadonlyBytes response);
    void close();

    // Processes raw bytes from the helper stream and optional EOF.
    // Always drains complete lines before treating EOF as an unexpected
    // disconnect (fixes the SUCCESS\n + immediate-EOF race).
    void feed_bytes(ReadonlyBytes data, bool eof);

    Function<void(ByteString prompt)> on_prompt_echo_off;
    Function<void(ByteString prompt)> on_prompt_echo_on;
    Function<void(ByteString message)> on_error_message;
    Function<void(ByteString message)> on_text_info;
    Function<void()> on_success;
    Function<void()> on_failure;
    Function<void()> on_unexpected_eof;

private:
    bool spawn_setuid_helper(ByteString const& helper_path, ByteString const& username, ByteString const& cookie);
    void on_readable();
    bool handle_line(StringView raw_line);

    OwnPtr<Core::LocalSocket> m_socket;
    Optional<Core::Process> m_process;
    int m_read_fd { -1 };
    int m_write_fd { -1 };
    RefPtr<Core::Notifier> m_notifier;
    StringBuilder m_buffer;
    bool m_verdict_reached { false };
};

}
