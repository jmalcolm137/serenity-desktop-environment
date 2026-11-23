/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>

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
//
// SharedState is RefCounted so a detached worker thread can safely finish even
// if the lock client exits while PAM is still blocked.
class Authenticator {
public:
    Authenticator(ByteString service, ByteString username);
    ~Authenticator();

    Authenticator(Authenticator const&) = delete;
    Authenticator& operator=(Authenticator const&) = delete;

    // Hands a wipeable UTF-8 password buffer to PAM. Always wipes `password`
    // before returning or on worker completion. Returns false when an attempt is
    // already running.
    bool begin(ByteBuffer password);
    bool busy() const;

    // Readable when an attempt has finished; poll() it alongside the Wayland fd.
    Optional<int> result_fd() const;

    // Consumes the finished attempt. Only call when result_fd() is readable.
    AuthResult take_result(ByteString& message);

private:
    struct SharedState : public RefCounted<SharedState> {
        int result_pipe[2] { -1, -1 };
        Atomic<bool> busy { false };

        ~SharedState();
        void finish(AuthResult, ByteString message);
    };

    ByteString m_service;
    ByteString m_username;
    NonnullRefPtr<SharedState> m_state;
    RefPtr<Threading::Thread> m_thread;
};

}
