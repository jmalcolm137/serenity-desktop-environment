/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Auth.h"

#include <AK/Assertions.h>
#include <AK/ByteBuffer.h>
#include <AK/StdLibExtras.h>
#include <AK/StringBuilder.h>
#include <LibThreading/Thread.h>

#include <errno.h>
#include <fcntl.h>
#include <security/pam_appl.h>
#include <string.h>
#include <unistd.h>

namespace SDELock {

namespace {

struct ConversationData {
    ByteString password;
    ByteString username;
    ByteString message;

    void note(char const* text)
    {
        if (!text || !*text)
            return;
        if (!message.is_empty())
            message = ByteString::formatted("{} {}", message, text);
        else
            message = ByteString(text);
    }
};

// PAM conversation: answer the password and user prompts, collect the messages.
// PAM owns (and frees) the response array; we only allocate it.
int conversation(int count, pam_message const** messages, pam_response** responses, void* data)
{
    auto* context = static_cast<ConversationData*>(data);
    auto* reply = static_cast<pam_response*>(calloc(static_cast<size_t>(count), sizeof(pam_response)));
    if (!reply)
        return PAM_BUF_ERR;

    for (int i = 0; i < count; ++i) {
        switch (messages[i]->msg_style) {
        case PAM_PROMPT_ECHO_OFF:
            reply[i].resp = strdup(context->password.characters());
            break;
        case PAM_PROMPT_ECHO_ON:
            reply[i].resp = strdup(context->username.characters());
            break;
        case PAM_ERROR_MSG:
        case PAM_TEXT_INFO:
            context->note(messages[i]->msg);
            break;
        default:
            free(reply);
            return PAM_CONV_ERR;
        }

        // The two prompt styles above must produce a response; the informational
        // ones must not.
        if (messages[i]->msg_style <= PAM_PROMPT_ECHO_ON && !reply[i].resp) {
            free(reply);
            return PAM_BUF_ERR;
        }
    }

    *responses = reply;
    return PAM_SUCCESS;
}

AuthResult result_from_pam_status(int status)
{
    switch (status) {
    case PAM_SUCCESS:
        return AuthResult::Success;
    case PAM_AUTH_ERR:
    case PAM_CRED_INSUFFICIENT:
    case PAM_AUTHINFO_UNAVAIL:
    case PAM_USER_UNKNOWN:
    case PAM_MAXTRIES:
        return AuthResult::Failure;
    default:
        return AuthResult::Error;
    }
}

}

Authenticator::Authenticator(ByteString service, ByteString username)
    : m_service(move(service))
    , m_username(move(username))
{
    if (pipe2(m_result_pipe, O_CLOEXEC | O_NONBLOCK) != 0) {
        m_result_pipe[0] = -1;
        m_result_pipe[1] = -1;
    }
}

Authenticator::~Authenticator()
{
    // A running attempt holds `this`; the process is exiting, so leaving the pipe
    // open is safer than closing it under the thread.
    if (m_busy.load())
        return;
    if (m_result_pipe[0] >= 0)
        close(m_result_pipe[0]);
    if (m_result_pipe[1] >= 0)
        close(m_result_pipe[1]);
}

Optional<int> Authenticator::result_fd() const
{
    if (m_result_pipe[0] < 0)
        return {};
    return m_result_pipe[0];
}

bool Authenticator::begin(String password)
{
    if (m_busy.exchange(true))
        return false;
    if (m_result_pipe[0] < 0 || m_result_pipe[1] < 0) {
        m_busy.store(false);
        return false;
    }

    // Drain a result nobody consumed before starting the next attempt.
    char discard[64];
    while (read(m_result_pipe[0], discard, sizeof(discard)) > 0)
        ;

    // The password lives in one buffer owned by the worker; it is wiped as soon as
    // PAM is done with it. (The strdup()ed copies PAM keeps are PAM's to free.)
    auto password_buffer = MUST(ByteBuffer::copy(password.bytes()));

    m_thread = Threading::Thread::construct([this, password_buffer = move(password_buffer)]() mutable -> intptr_t {
        ConversationData context;
        context.username = m_username;
        context.password = ByteString { reinterpret_cast<char const*>(password_buffer.data()), password_buffer.size() };

        pam_conv conv {
            .conv = conversation,
            .appdata_ptr = &context,
        };

        pam_handle_t* handle = nullptr;
        int status = pam_start(m_service.characters(), m_username.characters(), &conv, &handle);
        if (status == PAM_SUCCESS) {
            status = pam_authenticate(handle, PAM_DISALLOW_NULL_AUTHTOK);
            if (status == PAM_SUCCESS)
                status = pam_acct_mgmt(handle, 0);
        }

        ByteString message = context.message;
        if (message.is_empty()) {
            if (status == PAM_SUCCESS)
                message = {};
            else if (result_from_pam_status(status) == AuthResult::Failure)
                message = "Incorrect password";
            else
                message = handle ? ByteString(pam_strerror(handle, status)) : ByteString("Authentication is unavailable");
        }

        if (handle)
            pam_end(handle, status);

        explicit_bzero(password_buffer.data(), password_buffer.size());
        explicit_bzero(const_cast<char*>(context.password.characters()), context.password.length());
        context.password = {};

        auto result = result_from_pam_status(status);
        finish(result, move(message));
        return 0;
    }, "sde-lock-auth"sv);

    m_thread->start();
    m_thread->detach();
    return true;
}

void Authenticator::finish(AuthResult result, ByteString message)
{
    if (m_result_pipe[1] < 0) {
        m_busy.store(false);
        return;
    }

    // One write below PIPE_BUF is atomic, so the reader gets a whole record.
    static constexpr size_t max_message = 192;
    u8 record[5 + max_message];
    size_t length = min(message.length(), max_message);
    record[0] = static_cast<u8>(result);
    auto length32 = static_cast<u32>(length);
    memcpy(record + 1, &length32, sizeof(length32));
    memcpy(record + 5, message.characters(), length);

    (void)write(m_result_pipe[1], record, 5 + length);
    m_busy.store(false);
}

AuthResult Authenticator::take_result(ByteString& message)
{
    u8 record[5 + 192];
    ssize_t n = read(m_result_pipe[0], record, sizeof(record));
    m_busy.store(false);
    if (n < 5)
        return AuthResult::Error;

    u32 length = 0;
    memcpy(&length, record + 1, sizeof(length));
    if (length > static_cast<u32>(n - 5))
        length = static_cast<u32>(n - 5);
    message = ByteString { reinterpret_cast<char const*>(record + 5), length };
    return static_cast<AuthResult>(record[0]);
}

}
