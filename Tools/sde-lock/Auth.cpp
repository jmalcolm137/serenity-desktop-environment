/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Auth.h"

#include <AK/Assertions.h>
#include <AK/StdLibExtras.h>
#include <LibThreading/Thread.h>

#include <errno.h>
#include <fcntl.h>
#include <security/pam_appl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

namespace SDELock {

namespace {

struct ConversationData {
    ByteBuffer password;
    ByteString username;
    ByteString message;

    void wipe_password()
    {
        if (!password.is_empty())
            explicit_bzero(password.data(), password.size());
        password.clear();
    }

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

char* copy_c_string(ReadonlyBytes bytes)
{
    auto* out = static_cast<char*>(malloc(bytes.size() + 1));
    if (!out)
        return nullptr;
    if (!bytes.is_empty())
        memcpy(out, bytes.data(), bytes.size());
    out[bytes.size()] = '\0';
    return out;
}

void free_and_wipe_responses(pam_response* reply, int count)
{
    if (!reply)
        return;
    for (int i = 0; i < count; ++i) {
        if (reply[i].resp) {
            explicit_bzero(reply[i].resp, strlen(reply[i].resp));
            free(reply[i].resp);
        }
    }
    free(reply);
}

// PAM conversation: answer the password and user prompts, collect the messages.
// PAM owns (and frees) the response array on PAM_SUCCESS; on error we wipe and
// free every response allocated so far.
int conversation(int count, pam_message const** messages, pam_response** responses, void* data)
{
    if (count <= 0 || !messages || !responses || !data)
        return PAM_CONV_ERR;

    auto* context = static_cast<ConversationData*>(data);
    auto* reply = static_cast<pam_response*>(calloc(static_cast<size_t>(count), sizeof(pam_response)));
    if (!reply)
        return PAM_BUF_ERR;

    for (int i = 0; i < count; ++i) {
        int const style = messages[i]->msg_style;
        switch (style) {
        case PAM_PROMPT_ECHO_OFF:
            reply[i].resp = copy_c_string(context->password.bytes());
            if (!reply[i].resp) {
                free_and_wipe_responses(reply, i);
                return PAM_BUF_ERR;
            }
            break;
        case PAM_PROMPT_ECHO_ON:
            reply[i].resp = copy_c_string(context->username.bytes());
            if (!reply[i].resp) {
                free_and_wipe_responses(reply, i);
                return PAM_BUF_ERR;
            }
            break;
        case PAM_ERROR_MSG:
        case PAM_TEXT_INFO:
            context->note(messages[i]->msg);
            break;
        default:
            free_and_wipe_responses(reply, i);
            return PAM_CONV_ERR;
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

Authenticator::SharedState::~SharedState()
{
    if (result_pipe[0] >= 0)
        close(result_pipe[0]);
    if (result_pipe[1] >= 0)
        close(result_pipe[1]);
}

void Authenticator::SharedState::finish(AuthResult result, ByteString message)
{
    if (result_pipe[1] < 0) {
        busy.store(false);
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

    (void)write(result_pipe[1], record, 5 + length);
    busy.store(false);
}

Authenticator::Authenticator(ByteString service, ByteString username)
    : m_service(move(service))
    , m_username(move(username))
    , m_state(adopt_ref(*new SharedState))
{
    if (pipe2(m_state->result_pipe, O_CLOEXEC | O_NONBLOCK) != 0) {
        m_state->result_pipe[0] = -1;
        m_state->result_pipe[1] = -1;
    }
}

Authenticator::~Authenticator() = default;

bool Authenticator::busy() const
{
    return m_state->busy.load();
}

Optional<int> Authenticator::result_fd() const
{
    if (m_state->result_pipe[0] < 0)
        return {};
    return m_state->result_pipe[0];
}

bool Authenticator::begin(ByteBuffer password)
{
    auto wipe_local = [&] {
        if (!password.is_empty())
            explicit_bzero(password.data(), password.size());
        password.clear();
    };

    if (m_state->busy.exchange(true)) {
        wipe_local();
        return false;
    }
    if (m_state->result_pipe[0] < 0 || m_state->result_pipe[1] < 0) {
        m_state->busy.store(false);
        wipe_local();
        return false;
    }

    // Drain any stale result before starting the next attempt.
    char discard[64];
    while (read(m_state->result_pipe[0], discard, sizeof(discard)) > 0)
        ;

    auto state = m_state;
    auto service = m_service;
    auto username = m_username;

    m_thread = Threading::Thread::construct([state = move(state), service = move(service), username = move(username), password = move(password)]() mutable -> intptr_t {
        ConversationData context;
        context.username = move(username);
        context.password = move(password);

        pam_conv conv {
            .conv = conversation,
            .appdata_ptr = &context,
        };

        pam_handle_t* handle = nullptr;
        int status = pam_start(service.characters(), context.username.characters(), &conv, &handle);
        if (status == PAM_SUCCESS) {
            status = pam_authenticate(handle, PAM_DISALLOW_NULL_AUTHTOK);
            if (status == PAM_SUCCESS) {
                status = pam_acct_mgmt(handle, 0);
                if (status == PAM_SUCCESS)
                    (void)pam_setcred(handle, PAM_REINITIALIZE_CRED);
            }
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

        context.wipe_password();

        auto result = result_from_pam_status(status);
        state->finish(result, move(message));
        return 0;
    }, "sde-lock-auth"sv);

    m_thread->start();
    m_thread->detach();
    return true;
}

AuthResult Authenticator::take_result(ByteString& message)
{
    u8 record[5 + 192];
    ssize_t n = read(m_state->result_pipe[0], record, sizeof(record));
    m_state->busy.store(false);
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
