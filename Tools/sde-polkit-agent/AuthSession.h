/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "PasswordCache.h"
#include "PolkitHelper.h"
#include "dbus.h"

#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/Function.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibCore/Notifier.h>
#include <LibCore/Process.h>

namespace SDE {

struct AuthRequest {
    RefPtr<Portal::DeferredReply> pending;
    ByteString action_id;
    ByteString message;
    ByteString icon_name;
    ByteString details;
    ByteString cookie;
    Vector<ByteString> identities;
};

// Runs a single polkit BeginAuthentication request: drives PolkitHelper,
// consults PasswordCache, spawns sde-auth-dialog over a dedicated output FD,
// handles retries with inline errors, identity switching, and PAM_PROMPT_ECHO_ON.
class AuthSession {
public:
    static constexpr int max_attempts = 3;

    explicit AuthSession(PasswordCache& cache);
    ~AuthSession();

    bool is_active() const { return m_request.has_value(); }
    ByteString const& cookie() const { return m_request.has_value() ? m_request->cookie : m_empty_cookie; }

    void start(AuthRequest request);
    void cancel();

    Function<void()> on_finished;

private:
    void wire_helper_callbacks();
    void handle_prompt(ByteString prompt, bool echo_input);
    void spawn_dialog(ByteString const& prompt, bool echo_input);
    void on_dialog_readable();
    void handle_dialog_output(ByteBuffer output, bool exited_successfully);
    void handle_helper_failure();
    void restart_helper_deferred();
    void complete(bool gained);
    void finish(bool gained);
    void close_dialog();

    static void wipe(Optional<ByteBuffer>& buffer);
    static void wipe(ByteBuffer& buffer);

    PasswordCache& m_cache;
    PolkitHelper m_helper;

    Optional<AuthRequest> m_request;
    ByteString m_empty_cookie;
    ByteString m_username;
    ByteString m_last_error;

    Optional<Core::Process> m_dialog_process;
    int m_dialog_read_fd { -1 };
    RefPtr<Core::Notifier> m_dialog_notifier;
    ByteBuffer m_dialog_buffer;
    bool m_dialog_echo_mode { false };

    Optional<ByteBuffer> m_last_password;
    Optional<ByteBuffer> m_pending_switched_password;
    bool m_remember { false };
    bool m_used_cached_password { false };
    bool m_retried_without_cache { false };
    bool m_completion_scheduled { false };
    int m_attempts { 0 };
};

}
