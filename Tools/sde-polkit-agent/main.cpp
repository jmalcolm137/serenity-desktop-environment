/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// sde-polkit-agent — the SDE's graphical PolicyKit authentication agent.
//
// Registers on the system bus for the login session, serves
// org.freedesktop.PolicyKit1.AuthenticationAgent, queues concurrent requests,
// and delegates PAM conversations to PolkitHelper + AuthSession.
//
// Send SIGUSR1 to clear cached session passwords immediately (e.g. on screen
// lock or suspend).

#include "AuthSession.h"
#include "PasswordCache.h"
#include "PolkitHelper.h"
#include "dbus.h"

#include <AK/ByteString.h>
#include <AK/Debug.h>
#include <AK/Format.h>
#include <AK/Optional.h>
#include <AK/StringBuilder.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibCore/Environment.h>
#include <LibCore/EventLoop.h>
#include <LibMain/Main.h>

#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <unistd.h>

namespace {

constexpr StringView authority_name = "org.freedesktop.PolicyKit1"sv;
constexpr StringView authority_path = "/org/freedesktop/PolicyKit1/Authority"sv;
constexpr StringView authority_interface = "org.freedesktop.PolicyKit1.Authority"sv;
constexpr StringView agent_path = "/org/serenity/SDE/AuthenticationAgent"sv;
constexpr StringView agent_interface = "org.freedesktop.PolicyKit1.AuthenticationAgent"sv;

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

ByteString detect_session_locale()
{
    for (auto var : Array { "LC_ALL"sv, "LC_MESSAGES"sv, "LANG"sv }) {
        if (auto value = Core::Environment::get(var); value.has_value() && !value->is_empty())
            return value->to_byte_string();
    }
    return "en_US.UTF-8"sv;
}

AK::Duration configured_cache_ttl()
{
    if (auto value = Core::Environment::get("SDE_POLKIT_CACHE_TTL_SECONDS"sv); value.has_value()) {
        if (auto seconds = value->to_number<i64>(); seconds.has_value() && *seconds >= 0)
            return AK::Duration::from_seconds(*seconds);
    }
    return SDE::PasswordCache::default_ttl;
}

ByteString pid_file_path()
{
    if (auto runtime_dir = Core::Environment::get("XDG_RUNTIME_DIR"sv); runtime_dir.has_value() && !runtime_dir->is_empty())
        return ByteString::formatted("{}/sde-polkit-agent.pid", *runtime_dir);
    return {};
}

void write_pid_file(ByteString const& path)
{
    if (path.is_empty())
        return;
    int fd = ::open(path.characters(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0)
        return;
    auto text = ByteString::formatted("{}\n", ::getpid());
    (void)SDE::write_all_fd(fd, text);
    ::close(fd);
}

Vector<ByteString> collect_identities(Portal::Value const& identities)
{
    Vector<ByteString> result;
    uid_t self_uid = ::getuid();
    Optional<ByteString> self_name;

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
        auto* passwd = ::getpwuid(uid);
        if (!passwd)
            continue;

        ByteString name { passwd->pw_name };
        if (uid == self_uid) {
            self_name = move(name);
            continue;
        }
        if (!result.contains_slow(name))
            result.append(move(name));
    }

    if (self_name.has_value())
        result.prepend(move(*self_name));
    return result;
}

ByteString format_details(Portal::Value const* details)
{
    if (!details || !details->is(Portal::Value::Type::Dict))
        return {};
    StringBuilder builder;
    for (auto const& entry : details->dictionary()) {
        auto const& val = entry.value.unwrapped();
        if (!val.is(Portal::Value::Type::String))
            continue;
        if (!builder.is_empty())
            builder.append('\n');
        builder.appendff("{}: {}", entry.key, val.as_string());
    }
    return builder.to_byte_string();
}

class Agent {
public:
    explicit Agent(Portal::Service& service)
        : m_service(service)
        , m_cache(configured_cache_ttl())
        , m_session(m_cache)
        , m_locale(detect_session_locale())
    {
        m_session.on_finished = [this] { start_next_queued(); };
    }

    ~Agent()
    {
        unregister_from_polkit();
        if (m_filter_installed) {
            dbus_connection_remove_filter(m_service.connection(), &Agent::dbus_filter, this);
            m_filter_installed = false;
        }
        m_cache.clear_all();
    }

    ErrorOr<void> start()
    {
        auto session_id = Core::Environment::get("XDG_SESSION_ID"sv);
        if (!session_id.has_value() || session_id->is_empty()) {
            warnln("sde-polkit-agent: XDG_SESSION_ID is not set; cannot register for this session");
            return Error::from_string_literal("no login session");
        }
        m_session_id = session_id->to_byte_string();

        // Export the agent object before registering it: polkitd may call it as
        // soon as RegisterAuthenticationAgent returns.
        m_service.register_interface(
            agent_interface.to_byte_string(),
            [this](Portal::MethodCall& call) { handle_call(call); },
            {},
            {},
            agent_introspection_xml);

        install_authority_watcher();
        return register_with_polkit();
    }

    void clear_password_cache()
    {
        dbgln("sde-polkit-agent: clearing cached session passwords");
        m_cache.clear_all();
    }

    void unregister_from_polkit()
    {
        if (!m_registered || m_session_id.is_empty())
            return;
        m_registered = false;

        auto subject = make_session_subject();
        auto* message = dbus_message_new_method_call(
            authority_name.to_byte_string().characters(),
            authority_path.to_byte_string().characters(),
            authority_interface.to_byte_string().characters(),
            "UnregisterAuthenticationAgent");
        if (!message)
            return;

        DBusMessageIter iterator;
        dbus_message_iter_init_append(message, &iterator);
        subject.append_to(&iterator);
        Portal::Value::make_string(agent_path.to_byte_string()).append_to(&iterator);

        DBusError error;
        dbus_error_init(&error);
        if (auto* reply = dbus_connection_send_with_reply_and_block(m_service.connection(), message, 2000, &error))
            dbus_message_unref(reply);
        else if (dbus_error_is_set(&error))
            dbus_error_free(&error);
        dbus_message_unref(message);
    }

private:
    Portal::Value make_session_subject() const
    {
        Portal::Dictionary session_details;
        session_details.append({ "session-id"sv, Portal::Value::make_variant(Portal::Value::make_string(m_session_id)) });
        return Portal::Value::make_struct({
            Portal::Value::make_string("unix-session"),
            Portal::Value::make_dict(move(session_details)),
        });
    }

    ErrorOr<void> register_with_polkit()
    {
        auto subject = make_session_subject();
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
        Portal::Value::make_string(m_locale).append_to(&iterator);
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
        m_registered = true;
        return {};
    }

    void install_authority_watcher()
    {
        DBusError error;
        dbus_error_init(&error);
        dbus_bus_add_match(
            m_service.connection(),
            "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='org.freedesktop.PolicyKit1'",
            &error);
        if (dbus_error_is_set(&error)) {
            warnln("sde-polkit-agent: could not watch polkitd restarts: {}", error.message);
            dbus_error_free(&error);
            return;
        }
        if (dbus_connection_add_filter(m_service.connection(), &Agent::dbus_filter, this, nullptr))
            m_filter_installed = true;
    }

    static DBusHandlerResult dbus_filter(DBusConnection*, DBusMessage* message, void* data)
    {
        if (!dbus_message_is_signal(message, "org.freedesktop.DBus", "NameOwnerChanged"))
            return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

        char const* name = nullptr;
        char const* old_owner = nullptr;
        char const* new_owner = nullptr;
        if (!dbus_message_get_args(message, nullptr,
                DBUS_TYPE_STRING, &name,
                DBUS_TYPE_STRING, &old_owner,
                DBUS_TYPE_STRING, &new_owner,
                DBUS_TYPE_INVALID))
            return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

        if (!name || StringView { name, strlen(name) } != authority_name)
            return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

        auto* self = static_cast<Agent*>(data);
        if (new_owner && *new_owner != '\0') {
            dbgln("sde-polkit-agent: polkitd restarted (owner {}), re-registering", new_owner);
            Core::deferred_invoke([self] {
                self->m_registered = false;
                if (auto result = self->register_with_polkit(); result.is_error())
                    warnln("sde-polkit-agent: re-registration failed: {}", result.error());
            });
        } else {
            self->m_registered = false;
        }
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

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
        auto const* identities_arg = call.argument(5);
        if (!identities_arg || !identities_arg->is(Portal::Value::Type::Array)) {
            call.reply_error(Portal::ErrorName::InvalidArgs, "missing identities"sv);
            return;
        }

        auto identities = collect_identities(*identities_arg);
        if (identities.is_empty()) {
            call.reply_error(Portal::ErrorName::Failed, "no supported identity"sv);
            return;
        }

        SDE::AuthRequest request {
            .pending = call.take(),
            .action_id = call.string_argument(0),
            .message = call.string_argument(1),
            .icon_name = call.string_argument(2),
            .details = format_details(call.argument(3)),
            .cookie = call.string_argument(4),
            .identities = move(identities),
        };

        if (m_session.is_active()) {
            m_queue.append(move(request));
            return;
        }

        m_session.start(move(request));
    }

    void cancel_authentication(Portal::MethodCall& call)
    {
        auto cookie = call.string_argument(0);
        if (m_session.is_active() && m_session.cookie() == cookie) {
            m_session.cancel();
        } else {
            for (size_t i = 0; i < m_queue.size(); ++i) {
                if (m_queue[i].cookie == cookie) {
                    auto cancelled = m_queue.take(i);
                    if (cancelled.pending)
                        cancelled.pending->reply();
                    break;
                }
            }
        }
        call.reply();
    }

    void start_next_queued()
    {
        if (m_session.is_active() || m_queue.is_empty())
            return;
        auto next = m_queue.take_first();
        m_session.start(move(next));
    }

    Portal::Service& m_service;
    SDE::PasswordCache m_cache;
    SDE::AuthSession m_session;
    Vector<SDE::AuthRequest> m_queue;
    ByteString m_session_id;
    ByteString m_locale;
    bool m_registered { false };
    bool m_filter_installed { false };
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
    if (auto result = agent.start(); result.is_error())
        return 1;

    auto pid_path = pid_file_path();
    write_pid_file(pid_path);

    dbgln("sde-polkit-agent: registered for the current session at {}", agent_path);

    Core::EventLoop::register_signal(SIGTERM, [](int) { Core::EventLoop::current().quit(0); });
    Core::EventLoop::register_signal(SIGINT, [](int) { Core::EventLoop::current().quit(0); });
    Core::EventLoop::register_signal(SIGUSR1, [&agent](int) { agent.clear_password_cache(); });

    int exit_code = event_loop.exec();
    agent.unregister_from_polkit();
    if (!pid_path.is_empty())
        ::unlink(pid_path.characters());
    return exit_code;
}
