/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/ByteString.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AK/String.h>
namespace Threading {
class Thread;
}

namespace SDELock {

enum class AuthResult {
    Success,
    Failure,
    Error,
};

// One PAM authentication attempt at a time, on its own thread: pam_authenticate()
// can block (LDAP, an SSSD lookup, a smartcard prompt), and a lock screen must keep
// painting and answering the compositor while it does.
class Authenticator {
public:
    Authenticator(ByteString service, ByteString username);
    ~Authenticator();

    Authenticator(Authenticator const&) = delete;
    Authenticator& operator=(Authenticator const&) = delete;

    // Hands the password to PAM. Returns false when an attempt is already running.
    bool begin(String password);
    bool busy() const { return m_busy.load(); }

    // Readable when an attempt has finished; poll() it alongside the Wayland fd.
    Optional<int> result_fd() const;

    // Consumes the finished attempt. Only call when result_fd() is readable.
    AuthResult take_result(ByteString& message);

private:
    void finish(AuthResult, ByteString message);

    ByteString m_service;
    ByteString m_username;
    int m_result_pipe[2] { -1, -1 };
    RefPtr<Threading::Thread> m_thread;
    Atomic<bool> m_busy { false };
};

}
