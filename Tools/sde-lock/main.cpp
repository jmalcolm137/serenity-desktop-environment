/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// sde-lock: the SDE's native session locker.
//
// A plain ext-session-lock-v1 client, as docs/11 §10 requires: no SDE-specific
// protocols, so it locks an SDE session and any other conformant compositor
// (sway, Hyprland, KWin). The UI is drawn with LibGfx onto reusable wl_shm
// buffers; it is deliberately *not* a LibGUI app.

#include "Auth.h"
#include "KeyInput.h"
#include "LockScreen.h"
#include "WaylandSession.h"

#include <AK/ByteString.h>
#include <AK/Debug.h>
#include <AK/Format.h>
#include <AK/Optional.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/DateTime.h>
#include <LibCore/Process.h>

#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>

namespace {

using namespace SDELock;

constexpr long caret_blink_ms = 500;
constexpr long auth_cooldown_ms = 1000;

volatile sig_atomic_t s_signal_unlock = 0;

void on_unlock_signal(int)
{
    s_signal_unlock = 1;
}

long monotonic_ms()
{
    timespec now {};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<long>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}

ByteString resource_root()
{
    if (auto const* env = getenv("SERENITY_RES_ROOT"); env && *env)
        return ByteString(env);
    if (access("/usr/share/serenity/res/fonts", R_OK) == 0)
        return ByteString("/usr/share/serenity/res");
    return {};
}

void notify_polkit_agent_locked()
{
    auto const* runtime_dir = getenv("XDG_RUNTIME_DIR");
    if (!runtime_dir || !*runtime_dir)
        return;
    auto pid_path = ByteString::formatted("{}/sde-polkit-agent.pid", runtime_dir);
    int fd = ::open(pid_path.characters(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return;
    char buf[32] {};
    ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
    ::close(fd);
    if (n <= 0)
        return;
    auto pid = StringView { buf, static_cast<size_t>(n) }.trim_whitespace().to_number<pid_t>();
    if (pid.has_value() && *pid > 1)
        ::kill(*pid, SIGUSR1);
}

void blank_displays()
{
    ByteString dpms = "sde-dpms"sv;
    bool search_path = true;
    if (auto const* env = getenv("SDE_DPMS_BIN"); env && *env) {
        dpms = ByteString(env);
        search_path = false;
    }

    Vector<ByteString> args;
    args.append("off"sv);
    Core::ProcessSpawnOptions options {
        .name = dpms,
        .executable = dpms,
        .search_for_executable_in_path = search_path,
        .arguments = args,
    };
    if (auto proc = Core::Process::spawn(options); !proc.is_error())
        (void)proc.value().disown();
}

struct Options {
    ByteString username;
    ByteString display_name;
    ByteString pam_service;
    ByteString wallpaper_path;
    ByteString clock_format { "%H:%M"sv };
    int grace_seconds { 0 };
    bool allow_signal_unlock { false };
};

class LockApp {
public:
    explicit LockApp(Options options)
        : m_options(move(options))
        , m_authenticator(m_options.pam_service, m_options.username)
        , m_session(m_renderer)
    {
    }

    ErrorOr<int> run()
    {
        TRY(m_session.connect());
        TRY(m_renderer.initialize(resource_root(), m_options.username, m_options.display_name));

        auto wallpaper_path = m_options.wallpaper_path.is_empty() ? LockRenderer::default_wallpaper_path() : m_options.wallpaper_path;
        m_renderer.set_wallpaper(LockRenderer::load_wallpaper(wallpaper_path));

        m_state.display_name = m_options.display_name;
        update_clock(true);
        m_caret_on = true;
        m_state.caret_visible = true;
        m_last_caret_toggle = monotonic_ms();

        wire_session_callbacks();
        m_session.lock();

        while (m_running) {
            if (m_options.allow_signal_unlock && s_signal_unlock) {
                m_session.unlock();
                break;
            }

            long const now = monotonic_ms();
            int const timeout_ms = compute_poll_timeout_ms(now);

            bool auth_ready = false;
            if (!m_session.poll_and_dispatch(m_authenticator.result_fd(), timeout_ms, auth_ready))
                break;

            if (auth_ready)
                on_auth_result();

            long const after = monotonic_ms();
            if (!m_authenticating) {
                if (auto action = m_input.tick_repeat(after); action != KeyAction::None)
                    handle_key_action(action, after);
            }

            update_clock(false);
            update_caret(after);
            m_session.render_dirty(m_state);
        }

        return 0;
    }

private:
    void wire_session_callbacks()
    {
        m_session.on_locked = [this] {
            if (m_options.grace_seconds > 0)
                m_grace_until = monotonic_ms() + static_cast<long>(m_options.grace_seconds) * 1000L;
            notify_polkit_agent_locked();
            dbgln("sde-lock: locked");
        };

        m_session.on_finished = [this] {
            m_running = false;
        };

        m_session.on_keymap = [this](uint32_t format, int fd, uint32_t size) {
            if (m_input.set_keymap_fd(format, fd, size)) {
                m_state.keyboard_layout = m_input.layout_name();
                m_session.mark_all_dirty();
            }
        };

        m_session.on_modifiers = [this](uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group) {
            if (m_input.update_modifiers(depressed, latched, locked, group)) {
                m_state.caps_lock = m_input.caps_lock();
                m_state.keyboard_layout = m_input.layout_name();
                m_session.mark_all_dirty();
            }
        };

        m_session.on_repeat_info = [this](int32_t rate, int32_t delay) {
            m_input.set_repeat_info(rate, delay);
        };

        m_session.on_key = [this](uint32_t keycode, bool pressed) {
            long const now = monotonic_ms();
            if (pressed && check_grace_unlock(now))
                return;
            if (m_authenticating) {
                if (!pressed)
                    m_input.cancel_repeat();
                return;
            }
            auto action = m_input.handle_key(keycode, pressed, now);
            handle_key_action(action, now);
        };

        m_session.on_button_hover_changed = [this](bool hovered) {
            m_state.button_hovered = hovered;
            m_session.mark_all_dirty();
        };

        m_session.on_pointer_activity = [this] {
            (void)check_grace_unlock(monotonic_ms());
        };

        m_session.on_unlock_clicked = [this] {
            submit();
        };
    }

    bool check_grace_unlock(long now)
    {
        if (m_grace_until > 0 && now < m_grace_until) {
            dbgln("sde-lock: unlocked within grace period");
            m_running = false;
            m_session.unlock();
            return true;
        }
        return false;
    }

    void handle_key_action(KeyAction action, long now)
    {
        switch (action) {
        case KeyAction::None:
            return;
        case KeyAction::Submit:
            submit();
            return;
        case KeyAction::BlankDisplay:
            blank_displays();
            return;
        case KeyAction::Changed:
            // Clear stale failure state as soon as the user starts typing again.
            if (m_state.failed) {
                m_state.failed = false;
                m_state.message = {};
            }
            m_state.password_length = m_input.password_length();
            m_caret_on = true;
            m_state.caret_visible = true;
            m_last_caret_toggle = now;
            m_session.mark_all_dirty();
            return;
        }
    }

    void update_clock(bool force)
    {
        auto now = Core::DateTime::now();
        auto clock = now.to_byte_string(m_options.clock_format.view());
        auto date = now.to_byte_string("%A, %d %B %Y"sv);

        if (!force && clock == m_state.clock && date == m_state.date)
            return;
        m_state.clock = move(clock);
        m_state.date = move(date);
        // Secondary monitors also draw the clock outside the active panel rect.
        m_session.mark_all_dirty(true);
    }

    void update_caret(long now)
    {
        if (m_authenticating)
            return;
        if (now - m_last_caret_toggle < caret_blink_ms)
            return;
        m_last_caret_toggle = now;
        m_caret_on = !m_caret_on;
        m_state.caret_visible = m_caret_on;
        m_session.mark_all_dirty();
    }

    int compute_poll_timeout_ms(long now) const
    {
        // Default: wake up at the next minute boundary (or 10s max).
        auto dt = Core::DateTime::now();
        long timeout = max(250L, static_cast<long>(60 - dt.second()) * 1000L);

        if (!m_authenticating) {
            long caret_due = max(0L, (m_last_caret_toggle + caret_blink_ms) - now);
            timeout = min(timeout, caret_due);
        }
        if (auto repeat = m_input.next_repeat_delay_ms(now); repeat.has_value())
            timeout = min(timeout, *repeat);

        return static_cast<int>(clamp(timeout, 1L, 60000L));
    }

    void submit()
    {
        if (m_authenticating || m_input.password_empty())
            return;
        long const now = monotonic_ms();
        if (now < m_cooldown_until)
            return;

        m_input.cancel_repeat();
        auto password_bytes = m_input.take_password_utf8();
        m_state.password_length = 0;

        m_authenticating = true;
        m_state.authenticating = true;
        m_state.failed = false;
        m_state.message = {};
        m_state.caret_visible = false;
        m_session.mark_all_dirty();

        if (!m_authenticator.begin(move(password_bytes))) {
            m_authenticating = false;
            m_state.authenticating = false;
            m_state.failed = true;
            m_state.message = "Authentication is unavailable"sv;
            m_session.mark_all_dirty();
        }
    }

    void on_auth_result()
    {
        ByteString message;
        auto const result = m_authenticator.take_result(message);

        m_authenticating = false;
        m_state.authenticating = false;

        if (result == AuthResult::Success) {
            dbgln("sde-lock: authenticated");
            m_running = false;
            m_session.unlock();
            return;
        }

        dbgln("sde-lock: authentication {}: {}", result == AuthResult::Failure ? "failed" : "error", message);

        m_state.failed = true;
        m_state.message = message.is_empty() ? ByteString("Incorrect password") : move(message);
        m_caret_on = true;
        m_state.caret_visible = true;
        m_last_caret_toggle = monotonic_ms();
        m_cooldown_until = m_last_caret_toggle + auth_cooldown_ms;
        m_session.mark_all_dirty();
    }

    Options m_options;
    Authenticator m_authenticator;
    LockRenderer m_renderer;
    WaylandSession m_session;
    KeyInput m_input;
    UiState m_state;

    bool m_authenticating { false };
    bool m_caret_on { true };
    bool m_running { true };
    long m_last_caret_toggle { 0 };
    long m_cooldown_until { 0 };
    long m_grace_until { 0 };
};

}

int main(int argc, char** argv)
{
    // Prevent core dumps and ptrace inspection of in-memory credentials.
    (void)prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
    ::signal(SIGPIPE, SIG_IGN);

    StringView pam_service_arg;
    StringView username_arg;
    StringView wallpaper_arg;
    StringView clock_format_arg { "%H:%M"sv };
    int grace_seconds = 0;
    bool allow_signal_unlock = false;

    Core::ArgsParser args_parser;
    args_parser.set_general_help("Serenity Desktop Environment native Wayland session locker (ext-session-lock-v1).");
    args_parser.add_option(pam_service_arg, "PAM service to authenticate against (default: login or $SDE_LOCK_PAM_SERVICE)", "pam-service", 0, "name");
    args_parser.add_option(username_arg, "Authenticate as NAME (default: current user)", "username", 'u', "name");
    args_parser.add_option(wallpaper_arg, "Wallpaper image or PWDS .raw cache path", "wallpaper", 'w', "path");
    args_parser.add_option(clock_format_arg, "strftime clock format (default: %H:%M)", "clock-format", 0, "format");
    args_parser.add_option(grace_seconds, "Grace period in seconds where activity unlocks without a password", "grace", 'g', "seconds");
    args_parser.add_option(allow_signal_unlock, "Allow SIGTERM/SIGINT to unlock (for headless testing only)", "allow-signal-unlock", 0);

    Vector<StringView> arg_views;
    for (int i = 0; i < argc; ++i)
        arg_views.append(StringView { argv[i], strlen(argv[i]) });
    args_parser.parse(arg_views);

    // By default, SIGTERM/SIGINT must NEVER unlock a locked session.
    if (allow_signal_unlock) {
        ::signal(SIGTERM, on_unlock_signal);
        ::signal(SIGINT, on_unlock_signal);
    } else {
        ::signal(SIGTERM, SIG_IGN);
        ::signal(SIGINT, SIG_IGN);
    }

    Options options;
    options.grace_seconds = max(0, grace_seconds);
    options.allow_signal_unlock = allow_signal_unlock;
    options.wallpaper_path = wallpaper_arg.to_byte_string();
    options.clock_format = clock_format_arg.is_empty() ? ByteString("%H:%M") : clock_format_arg.to_byte_string();

    if (!pam_service_arg.is_empty())
        options.pam_service = pam_service_arg.to_byte_string();
    else if (auto const* env = getenv("SDE_LOCK_PAM_SERVICE"); env && *env)
        options.pam_service = ByteString(env);
    else
        options.pam_service = ByteString("login");

    struct passwd* pw = nullptr;
    if (!username_arg.is_empty()) {
        options.username = username_arg.to_byte_string();
        pw = ::getpwnam(options.username.characters());
    } else {
        pw = ::getpwuid(::getuid());
        if (!pw || !pw->pw_name) {
            warnln("sde-lock: cannot determine the current user");
            return 1;
        }
        options.username = ByteString(pw->pw_name);
    }

    StringView gecos = (pw && pw->pw_gecos) ? StringView { pw->pw_gecos, strlen(pw->pw_gecos) } : StringView {};
    options.display_name = display_name_from_gecos(options.username, gecos);

    LockApp app(move(options));
    auto result = app.run();
    if (result.is_error()) {
        warnln("sde-lock: {}", result.error());
        return 1;
    }
    return result.value();
}
