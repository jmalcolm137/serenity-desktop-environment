/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// sde-polkit-agent — the SDE's graphical PolicyKit authentication agent.
//
// It registers on the system bus for the login session and shows the LibGUI
// password dialog (sde-auth-dialog). Polkit's trusted helper performs the actual
// PAM conversation and reports the result to polkitd, so this process never runs
// privileged code and never handles the PAM stack itself.
//
// This is an ordinary Serenity-style service: AK types over the `dbus-1`
// transport (Tools/portal/dbus.h, no GLib/GIO) and a LibCore event loop. The
// helper conversation is the libpolkit-agent one: a line protocol over
// /run/polkit/agent-helper.socket (or a setuid helper's stdin/stdout on systems
// without socket activation).
//
// The checkbox is real: after a successful authentication it keeps the password
// in this process's memory for the rest of the session and answers subsequent
// PAM prompts without showing the dialog. It is cleared if a cached password is
// rejected, and never written to disk.

#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/Debug.h>
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/StringBuilder.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibCore/Environment.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Notifier.h>
#include <LibCore/Process.h>
#include <LibCore/Socket.h>
#include <LibCore/System.h>
#include <LibMain/Main.h>

#include "dbus.h"
#include "dialog_output.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

constexpr StringView authority_name = "org.freedesktop.PolicyKit1"sv;
constexpr StringView authority_path = "/org/freedesktop/PolicyKit1/Authority"sv;
constexpr StringView authority_interface = "org.freedesktop.PolicyKit1.Authority"sv;
constexpr StringView agent_path = "/org/serenity/SDE/AuthenticationAgent"sv;
constexpr StringView agent_interface = "org.freedesktop.PolicyKit1.AuthenticationAgent"sv;

constexpr StringView default_helper_socket = "/run/polkit/agent-helper.socket"sv;
constexpr StringView default_helper_binary = "/usr/lib/polkit-1/polkit-agent-helper-1"sv;

constexpr StringView agent_introspection_xml = R"XML(<interface name="org.freedesktop.PolicyKit1.AuthenticationAgent">
  <method name="BeginAuthentication">
    <arg type="s" name="action_id" direction="in"/>
    <arg type="s" name="message" direction="in"/>
    <arg type="s" name="icon_name" direction="in"/>
    <arg type="a{ss}" name="details" direction="in"/>
    <arg type="s" name="cookie" direction="in"/>
    <arg type="a(sa{sv})" name="identities" direction="in"/>
  </method>
  <method name="CancelAuthentication">
    <arg type="s" name="cookie" direction="in"/>
  </method>
</interface>)XML"sv;

ByteString environment_or(StringView name, StringView fallback)
{
    if (auto value = Core::Environment::get(name); value.has_value() && !value->is_empty())
        return value->to_byte_string();
    return fallback.to_byte_string();
}

bool is_setuid_helper(StringView path)
{
    auto path_string = path.to_byte_string();
    struct stat status { };
    if (stat(path_string.characters(), &status) != 0)
        return false;
    if (!S_ISREG(status.st_mode) || status.st_uid != 0 || (status.st_mode & S_ISUID) == 0)
        return false;
    return access(path_string.characters(), X_OK) == 0;
}

// g_strescape() output (used by the helper) escapes control characters and
// backslashes; decode it before looking at the prefix or showing a message.
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

bool write_all(int fd, ReadonlyBytes bytes)
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

bool write_all(int fd, StringView text)
{
    return write_all(fd, text.bytes());
}

void set_nonblocking(int fd)
{
    int flags = ::fcntl(fd, F_GETFL);
    if (flags >= 0)
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

struct Target {
    uid_t uid { 0 };
    ByteString username;
};

Optional<Target> select_identity(Portal::Value const& identities)
{
    // Prefer the session user's own identity; otherwise authenticate the first
    // unix user polkitd offered.
    Optional<Target> first;
    for (auto const& entry : identities.container()) {
        if (!entry.is(Portal::Value::Type::Struct))
            continue;
        auto const& fields = entry.container();
        if (fields.size() < 2)
            continue;
        if (!fields[0].is(Portal::Value::Type::String) || fields[0].as_string() != "unix-user"sv)
            continue;
        if (!fields[1].is(Portal::Value::Type::Dict))
            continue;

        auto uid = static_cast<uid_t>(fields[1].uint32_or("uid"sv, 0));
        auto* passwd = getpwuid(uid);
        if (!passwd)
            continue;

        Target target { uid, ByteString { passwd->pw_name } };
        if (uid == getuid())
            return target;
        if (!first.has_value())
            first = move(target);
    }
    return first;
}

class Agent {
public:
    explicit Agent(Portal::Service& service)
        : m_service(service)
    {
    }

    ~Agent()
    {
        wipe(m_last_password);
        clear_cached_password();
    }

    ErrorOr<void> register_with_polkit()
    {
        auto session_id = Core::Environment::get("XDG_SESSION_ID"sv);
        if (!session_id.has_value() || session_id->is_empty()) {
            warnln("sde-polkit-agent: XDG_SESSION_ID is not set; cannot register for this session");
            return Error::from_string_literal("no login session");
        }

        Portal::Dictionary session_details;
        session_details.append({ "session-id"sv, Portal::Value::make_variant(Portal::Value::make_string(session_id->to_byte_string())) });
        auto subject = Portal::Value::make_struct({
            Portal::Value::make_string("unix-session"),
            Portal::Value::make_dict(move(session_details)),
        });

        // Export the agent object before registering it: polkitd may call it as
        // soon as RegisterAuthenticationAgent returns.
        m_service.register_interface(
            agent_interface.to_byte_string(),
            [this](Portal::MethodCall& call) { handle_call(call); },
            {},
            {},
            agent_introspection_xml);

        auto* message = dbus_message_new_method_call(
            authority_name.to_byte_string().characters(),
            authority_path.to_byte_string().characters(),
            authority_interface.to_byte_string().characters(),
            "RegisterAuthenticationAgent");
        if (!message)
            return Error::from_string_literal("could not allocate the registration message");

        DBusMessageIter iterator;
        dbus_message_iter_init_append(message, &iterator);
        subject.append_to(&iterator);
        Portal::Value::make_string("en_US.UTF-8").append_to(&iterator);
        Portal::Value::make_string(agent_path.to_byte_string()).append_to(&iterator);

        DBusError error;
        dbus_error_init(&error);
        auto* reply = dbus_connection_send_with_reply_and_block(m_service.connection(), message, 5000, &error);
        dbus_message_unref(message);
        if (!reply) {
            if (dbus_error_is_set(&error)) {
                warnln("sde-polkit-agent: registration failed: {} ({})", error.name, error.message);
                dbus_error_free(&error);
            } else {
                warnln("sde-polkit-agent: registration failed: no reply from polkitd");
            }
            return Error::from_string_literal("registration failed");
        }
        dbus_message_unref(reply);
        return {};
    }

private:
    void handle_call(Portal::MethodCall& call)
    {
        if (call.member() == "BeginAuthentication"sv) {
            begin_authentication(call);
            return;
        }
        if (call.member() == "CancelAuthentication"sv) {
            cancel_authentication(call);
            return;
        }
        call.reply_error(Portal::ErrorName::UnknownMethod, "unknown authentication agent method"sv);
    }

    void begin_authentication(Portal::MethodCall& call)
    {
        if (m_pending) {
            call.reply_error(Portal::ErrorName::Failed, "another authentication is already in progress"sv);
            return;
        }

        auto const* identities = call.argument(5);
        if (!identities || !identities->is(Portal::Value::Type::Array)) {
            call.reply_error(Portal::ErrorName::InvalidArgs, "missing identities"sv);
            return;
        }

        auto target = select_identity(*identities);
        if (!target.has_value()) {
            call.reply_error(Portal::ErrorName::Failed, "no supported identity"sv);
            return;
        }

        m_pending = call.take();
        m_cookie = call.string_argument(4);
        m_message = call.string_argument(1);
        m_username = move(target->username);
        m_last_password.clear();
        m_remember = false;
        m_used_cached_password = false;
        m_retried_without_cache = false;

        if (!start_helper()) {
            complete(false);
            return;
        }
    }

    void cancel_authentication(Portal::MethodCall& call)
    {
        auto cookie = call.string_argument(0);
        if (m_pending && cookie == m_cookie)
            cancel();
        call.reply();
    }

    bool start_helper()
    {
        close_helper();

        auto helper = environment_or("SDE_POLKIT_HELPER_BIN"sv, default_helper_binary);

        // Prefer the traditional setuid helper when it is installed that way.
        // polkit 128's socket-activated helper has a peer-credential/pam_unix
        // regression that rejects correct passwords; the direct setuid helper
        // takes the stable path. Systems without the setuid bit keep using the
        // socket.
        if (is_setuid_helper(helper))
            return spawn_helper();

        auto socket_path = environment_or("SDE_POLKIT_HELPER_SOCKET"sv, default_helper_socket);
        if (access(socket_path.characters(), F_OK) == 0) {
            auto socket = Core::LocalSocket::connect(socket_path);
            if (!socket.is_error()) {
                m_helper_socket = socket.release_value();
                auto fd = m_helper_socket->fd();
                if (!fd.has_value()) {
                    close_helper();
                    return false;
                }
                m_helper_read_fd = *fd;
                m_helper_write_fd = *fd;
                set_nonblocking(m_helper_read_fd);

                // The socket-activated helper reads the user first, then the
                // cookie, both newline terminated.
                if (!write_all(m_helper_write_fd, m_username) || !write_all(m_helper_write_fd, "\n"sv)
                    || !write_all(m_helper_write_fd, m_cookie) || !write_all(m_helper_write_fd, "\n"sv)) {
                    warnln("sde-polkit-agent: could not send the request to the polkit helper");
                    close_helper();
                    return false;
                }

                m_helper_notifier = Core::Notifier::construct(m_helper_read_fd, Core::Notifier::Type::Read);
                m_helper_notifier->on_activation = [this] { on_helper_readable(); };
                return true;
            }
            dbgln("sde-polkit-agent: cannot connect to {}: {}", socket_path, socket.error());
        }

        return spawn_helper();
    }

    bool spawn_helper()
    {
        auto helper = environment_or("SDE_POLKIT_HELPER_BIN"sv, default_helper_binary);
        if (access(helper.characters(), X_OK) != 0) {
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
        arguments.append(m_username);
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

        m_helper_process = process.release_value();
        m_helper_read_fd = output_read;
        m_helper_write_fd = input_write;
        set_nonblocking(m_helper_read_fd);

        // The setuid helper expects the cookie on stdin and keeps reading stdin
        // for the PAM responses; the username is its argv[1]. Leave the write end
        // open for the rest of the session.
        bool wrote_cookie = write_all(m_helper_write_fd, m_cookie) && write_all(m_helper_write_fd, "\n"sv);
        if (!wrote_cookie) {
            warnln("sde-polkit-agent: could not send the cookie to the polkit helper");
            close_helper();
            return false;
        }

        m_helper_notifier = Core::Notifier::construct(m_helper_read_fd, Core::Notifier::Type::Read);
        m_helper_notifier->on_activation = [this] { on_helper_readable(); };
        return true;
    }

    void on_helper_readable()
    {
        if (m_helper_read_fd < 0)
            return;

        char chunk[4096];
        for (;;) {
            ssize_t count = ::read(m_helper_read_fd, chunk, sizeof(chunk));
            if (count > 0) {
                m_helper_buffer.append(StringView { chunk, static_cast<size_t>(count) });
                continue;
            }
            if (count < 0 && errno == EINTR)
                continue;
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                break;

            // EOF (or a read error): the helper is gone without a final verdict.
            // Disable, but do not destroy, the notifier from inside its callback;
            // the deferred finish() closes it.
            if (m_helper_notifier)
                m_helper_notifier->close();
            complete(false);
            return;
        }

        consume_lines(m_helper_buffer, [this](StringView raw_line) {
            if (!m_pending || m_completion_scheduled)
                return;
            handle_helper_line(raw_line);
        });
    }

    void handle_helper_line(StringView raw_line)
    {
        auto line = unescape_helper_text(raw_line);

        if (line.starts_with("PAM_PROMPT_ECHO_OFF "sv)) {
            request_password();
            return;
        }
        if (line.starts_with("PAM_PROMPT_ECHO_ON "sv)) {
            send_response(m_username);
            return;
        }
        if (line.starts_with("PAM_ERROR_MSG "sv) || line.starts_with("PAM_TEXT_INFO "sv)) {
            dbgln("sde-polkit-agent: {}", line);
            return;
        }
        if (line == "SUCCESS"sv) {
            complete(true);
            return;
        }
        if (line == "FAILURE"sv) {
            helper_failed();
            return;
        }

        warnln("sde-polkit-agent: unexpected line from the polkit helper: {}", line);
        complete(false);
    }

    void request_password()
    {
        if (m_cached_password.has_value()) {
            auto copy = ByteBuffer::copy(m_cached_password->bytes());
            if (copy.is_error()) {
                clear_cached_password();
            } else {
                m_used_cached_password = true;
                m_last_password = copy.release_value();
                send_response(m_last_password->bytes());
                return;
            }
        }

        if (m_dialog_process.has_value())
            return;

        auto dialog = environment_or("SDE_AUTH_DIALOG"sv, "sde-auth-dialog"sv);
        Vector<ByteString> arguments;
        arguments.append("--message"sv);
        arguments.append(m_message);
        if (!m_username.is_empty()) {
            arguments.append("--username"sv);
            arguments.append(m_username);
        }

        auto pipe = Core::System::pipe2(O_CLOEXEC);
        if (pipe.is_error()) {
            warnln("sde-polkit-agent: cannot create the dialog pipe: {}", pipe.error());
            complete(false);
            return;
        }
        int read_fd = pipe.value()[0];
        int write_fd = pipe.value()[1];

        Core::ProcessSpawnOptions options {
            .name = dialog,
            .executable = dialog,
            .search_for_executable_in_path = true,
            .arguments = arguments,
        };
        options.file_actions.append(Core::FileAction::DuplicateFile { write_fd, STDOUT_FILENO });
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
        m_dialog_buffer.clear();
        set_nonblocking(m_dialog_read_fd);
        m_dialog_notifier = Core::Notifier::construct(m_dialog_read_fd, Core::Notifier::Type::Read);
        m_dialog_notifier->on_activation = [this] { on_dialog_readable(); };
    }

    void on_dialog_readable()
    {
        if (m_dialog_read_fd < 0)
            return;

        char chunk[4096];
        for (;;) {
            ssize_t count = ::read(m_dialog_read_fd, chunk, sizeof(chunk));
            if (count > 0) {
                m_dialog_buffer.append(StringView { chunk, static_cast<size_t>(count) });
                continue;
            }
            if (count < 0 && errno == EINTR)
                continue;
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                return;

            break;
        }

        // Disable, but do not destroy, the notifier from inside its callback.
        if (m_dialog_notifier)
            m_dialog_notifier->close();
        if (m_dialog_read_fd >= 0) {
            ::close(m_dialog_read_fd);
            m_dialog_read_fd = -1;
        }

        auto output = m_dialog_buffer.string_view().to_byte_string();
        if (!m_dialog_process.has_value())
            return;
        auto process = m_dialog_process.release_value();
        m_dialog_process.clear();

        // Reap and parse after this notifier callback returns: destroying the
        // dialog state from inside its own dispatch is not safe.
        Core::deferred_invoke([this, output = move(output), process = move(process)]() mutable {
            m_dialog_notifier = nullptr;
            auto result = process.wait_for_termination();
            handle_dialog_output(output, !result.is_error() && result.value());
        });
    }

    void handle_dialog_output(ByteString const& output, bool exited_successfully)
    {
        if (!m_pending)
            return;

        if (!exited_successfully) {
            complete(false);
            return;
        }

        auto parsed = SDE::parse_dialog_output(output);
        if (!parsed.has_value()) {
            complete(false);
            return;
        }

        auto copy = ByteBuffer::copy(parsed->password.bytes());
        if (copy.is_error()) {
            complete(false);
            return;
        }
        m_last_password = copy.release_value();
        m_remember = parsed->remember;
        m_used_cached_password = false;
        send_response(m_last_password->bytes());
    }

    void helper_failed()
    {
        // A cached password can expire (the user changed it). Retry once with a
        // prompt instead of failing the whole authorization. The retry restarts
        // the helper, so it must run after this notifier callback returns.
        if (m_used_cached_password && !m_retried_without_cache) {
            clear_cached_password();
            m_retried_without_cache = true;
            m_used_cached_password = false;
            wipe(m_last_password);
            m_last_password.clear();
            m_remember = false;
            m_completion_scheduled = true;
            Core::deferred_invoke([this] {
                m_completion_scheduled = false;
                if (m_pending && start_helper())
                    return;
                if (m_pending)
                    complete(false);
            });
            return;
        }
        complete(false);
    }

    void send_response(ReadonlyBytes response)
    {
        if (m_helper_write_fd < 0)
            return;
        if (!write_all(m_helper_write_fd, response) || !write_all(m_helper_write_fd, "\n"sv)) {
            warnln("sde-polkit-agent: could not send the authentication response");
            complete(false);
        }
    }

    void send_response(StringView response)
    {
        send_response(response.bytes());
    }

    void complete(bool gained)
    {
        // The helper and dialog notifiers call this from their own dispatch, and
        // destroying a notifier from inside its callback is not safe. Finish on
        // the next loop turn instead.
        if (m_completion_scheduled)
            return;
        m_completion_scheduled = true;
        Core::deferred_invoke([this, gained] { finish(gained); });
    }

    void finish(bool gained)
    {
        close_helper();
        close_dialog();

        if (gained) {
            if (m_remember && m_last_password.has_value())
                set_cached_password(m_last_password->bytes());
        } else if (m_used_cached_password) {
            clear_cached_password();
        }

        wipe(m_last_password);
        m_last_password.clear();
        m_remember = false;
        m_used_cached_password = false;
        m_retried_without_cache = false;
        m_username = {};
        m_message = {};
        m_cookie = {};

        if (m_pending) {
            auto pending = move(m_pending);
            pending->reply();
        }
        m_completion_scheduled = false;
    }

    void cancel()
    {
        complete(false);
    }

    void set_cached_password(ReadonlyBytes password)
    {
        clear_cached_password();
        auto copy = ByteBuffer::copy(password);
        if (!copy.is_error())
            m_cached_password = copy.release_value();
    }

    void clear_cached_password()
    {
        if (!m_cached_password.has_value())
            return;
        wipe(m_cached_password);
        m_cached_password.clear();
    }

    static void wipe(Optional<ByteBuffer>& buffer)
    {
        if (!buffer.has_value())
            return;
        explicit_bzero(buffer->data(), buffer->size());
    }

    void close_helper()
    {
        if (m_helper_notifier) {
            m_helper_notifier->close();
            m_helper_notifier = nullptr;
        }
        if (m_helper_socket) {
            m_helper_socket = nullptr;
            m_helper_read_fd = -1;
            m_helper_write_fd = -1;
        }
        if (m_helper_read_fd >= 0) {
            ::close(m_helper_read_fd);
            m_helper_read_fd = -1;
        }
        if (m_helper_write_fd >= 0) {
            ::close(m_helper_write_fd);
            m_helper_write_fd = -1;
        }
        if (m_helper_process.has_value()) {
            auto process = move(*m_helper_process);
            m_helper_process.clear();
            if (process.pid() > 0)
                ::kill(process.pid(), SIGTERM);
            (void)process.wait_for_termination();
        }
        m_helper_buffer.clear();
    }

    void close_dialog()
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
        m_dialog_buffer.clear();
    }

    template<typename Callback>
    static void consume_lines(StringBuilder& buffer, Callback callback)
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

    Portal::Service& m_service;
    RefPtr<Portal::DeferredReply> m_pending;
    ByteString m_cookie;
    ByteString m_message;
    ByteString m_username;

    OwnPtr<Core::LocalSocket> m_helper_socket;
    Optional<Core::Process> m_helper_process;
    int m_helper_read_fd { -1 };
    int m_helper_write_fd { -1 };
    RefPtr<Core::Notifier> m_helper_notifier;
    StringBuilder m_helper_buffer;

    Optional<Core::Process> m_dialog_process;
    int m_dialog_read_fd { -1 };
    RefPtr<Core::Notifier> m_dialog_notifier;
    StringBuilder m_dialog_buffer;

    Optional<ByteBuffer> m_last_password;
    Optional<ByteBuffer> m_cached_password;
    bool m_remember { false };
    bool m_used_cached_password { false };
    bool m_retried_without_cache { false };
    bool m_completion_scheduled { false };
};

}

ErrorOr<int> serenity_main(Main::Arguments)
{
    // A helper that exits early must not take the agent down with SIGPIPE.
    ::signal(SIGPIPE, SIG_IGN);

    Core::EventLoop event_loop;

    auto service = Portal::Service::create_on_bus(DBUS_BUS_SYSTEM, {}, agent_path.to_byte_string());
    if (service.is_error()) {
        warnln("sde-polkit-agent: cannot connect to the system bus: {}", service.error());
        return 1;
    }

    auto service_ptr = service.release_value();
    Agent agent(*service_ptr);
    if (auto result = agent.register_with_polkit(); result.is_error())
        return 1;

    dbgln("sde-polkit-agent: registered for the current session at {}", agent_path);

    Core::EventLoop::register_signal(SIGTERM, [](int) { Core::EventLoop::current().quit(0); });
    Core::EventLoop::register_signal(SIGINT, [](int) { Core::EventLoop::current().quit(0); });

    return event_loop.exec();
}
