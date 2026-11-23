/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Regression & unit tests for sde-polkit-agent:
//   1. Dialog output parsing (unchecked "0" flag, LibWM diagnostics, USER: line,
//      secure ByteBuffer parsing, empty/malformed output).
//   2. PolkitHelper line framing & EOF-before-consume_lines regression:
//      "SUCCESS\n" arriving in the same read loop as EOF must invoke on_success
//      (so session password caching actually runs) and NOT on_unexpected_eof.
//   3. PasswordCache per-user isolation, TTL expiration, and clearing.
//
// The driver is scripts/run-polkit-dialog-output-test.sh.
#include "../Tools/sde-polkit-agent/PasswordCache.h"
#include "../Tools/sde-polkit-agent/PolkitHelper.h"
#include "../Tools/sde-polkit-agent/dialog_output.h"
#include <AK/Format.h>
#include <LibMain/Main.h>

namespace {

int g_failures = 0;

void check(char const* name, bool condition)
{
    if (!condition) {
        warnln("FAIL: {}", name);
        ++g_failures;
    }
}

void expect(char const* name, StringView output, Optional<ByteString> password, Optional<bool> remember, Optional<ByteString> user = {})
{
    auto parsed = SDE::parse_dialog_output(output);

    if (!password.has_value()) {
        if (parsed.has_value()) {
            warnln("FAIL: {}: expected no result, got password='{}' remember={}", name, parsed->password, parsed->remember);
            ++g_failures;
        }
        return;
    }

    if (!parsed.has_value()) {
        warnln("FAIL: {}: expected a result", name);
        ++g_failures;
        return;
    }
    if (parsed->password != *password) {
        warnln("FAIL: {}: password is '{}', expected '{}'", name, parsed->password, *password);
        ++g_failures;
    }
    if (parsed->remember != *remember) {
        warnln("FAIL: {}: remember is {}, expected {}", name, parsed->remember, *remember);
        ++g_failures;
    }
    if (parsed->selected_user != user) {
        warnln("FAIL: {}: selected_user mismatch", name);
        ++g_failures;
    }
}

void test_dialog_output()
{
    // The regression: an unchecked checkbox writes "0" on the second line.
    expect("unchecked", "hunter2\n0\n"sv, ByteString { "hunter2" }, false);
    expect("checked", "hunter2\n1\n"sv, ByteString { "hunter2" }, true);

    // LibWM diagnostics may be printed before the dialog's two lines.
    expect("diagnostics", "LibWM: connected\nLibWM: window presented\nhunter2\n0\n"sv, ByteString { "hunter2" }, false);
    expect("diagnostics-checked", "LibWM: connected\nhunter2\n1\n"sv, ByteString { "hunter2" }, true);

    // Multi-identity output with trailing USER:<name> line.
    expect("with-user", "hunter2\n1\nUSER:root\n"sv, ByteString { "hunter2" }, true, ByteString { "root" });
    expect("with-user-unchecked", "hunter2\n0\nUSER:alice\n"sv, ByteString { "hunter2" }, false, ByteString { "alice" });
    expect("user-only-malformed", "0\nUSER:root\n"sv, {}, {});

    // Empty passwords and a password that looks like the flag.
    expect("empty", "\n0\n"sv, ByteString {}, false);
    expect("flag-like-password", "0\n0\n"sv, ByteString { "0" }, false);

    // Missing/invalid flag, and no output at all.
    expect("missing-flag", "hunter2\n"sv, {}, {});
    expect("invalid-flag", "hunter2\n2\n"sv, {}, {});
    expect("empty-output", ""sv, {}, {});

    // Accepting a final line without a newline keeps the parser robust.
    expect("no-trailing-newline", "hunter2\n0"sv, ByteString { "hunter2" }, false);

    // Secure ByteBuffer variant.
    auto secure = SDE::parse_dialog_output_secure("s3cr3t\n1\nUSER:bob\n"sv.bytes());
    check("secure-has-value", secure.has_value());
    if (secure.has_value()) {
        check("secure-password", StringView { secure->password.data(), secure->password.size() } == "s3cr3t"sv);
        check("secure-remember", secure->remember);
        check("secure-user", secure->selected_user == ByteString { "bob" });
        secure->wipe();
        check("secure-wiped", secure->password.is_empty());
    }
}

void test_helper_eof_race()
{
    // Primary regression: polkit-agent-helper-1 writes "SUCCESS\n" and exits
    // immediately, so the read loop observes both "SUCCESS\n" and EOF in one
    // turn. on_success MUST fire and on_unexpected_eof MUST NOT fire.
    {
        SDE::PolkitHelper helper;
        int successes = 0;
        int failures = 0;
        int eofs = 0;
        helper.on_success = [&] { ++successes; };
        helper.on_failure = [&] { ++failures; };
        helper.on_unexpected_eof = [&] { ++eofs; };

        helper.feed_bytes("SUCCESS\n"sv.bytes(), true);
        check("success-with-eof: on_success called", successes == 1);
        check("success-with-eof: on_failure not called", failures == 0);
        check("success-with-eof: on_unexpected_eof not called", eofs == 0);
    }

    // Same race with FAILURE\n + immediate EOF.
    {
        SDE::PolkitHelper helper;
        int successes = 0;
        int failures = 0;
        int eofs = 0;
        helper.on_success = [&] { ++successes; };
        helper.on_failure = [&] { ++failures; };
        helper.on_unexpected_eof = [&] { ++eofs; };

        helper.feed_bytes("FAILURE\n"sv.bytes(), true);
        check("failure-with-eof: on_failure called", failures == 1);
        check("failure-with-eof: on_success not called", successes == 0);
        check("failure-with-eof: on_unexpected_eof not called", eofs == 0);
    }

    // Chunked lines + PAM prompts + error messages + unescaping.
    {
        SDE::PolkitHelper helper;
        ByteString echo_off_prompt;
        ByteString echo_on_prompt;
        ByteString error_msg;
        int successes = 0;
        int eofs = 0;
        helper.on_prompt_echo_off = [&](ByteString p) { echo_off_prompt = move(p); };
        helper.on_prompt_echo_on = [&](ByteString p) { echo_on_prompt = move(p); };
        helper.on_error_message = [&](ByteString m) { error_msg = move(m); };
        helper.on_success = [&] { ++successes; };
        helper.on_unexpected_eof = [&] { ++eofs; };

        helper.feed_bytes("PAM_PROMPT_ECHO_OFF Pass"sv.bytes(), false);
        helper.feed_bytes("word:\\n\nPAM_PROMPT_ECHO_ON OTP: \nPAM_ERROR_MSG Bad\\040pass\nSUC"sv.bytes(), false);
        helper.feed_bytes("CESS\n"sv.bytes(), true);

        check("chunked-echo-off", echo_off_prompt == "Password:\n"sv);
        check("chunked-echo-on", echo_on_prompt == "OTP: "sv);
        check("chunked-error-msg", error_msg == "Bad pass"sv);
        check("chunked-success", successes == 1);
        check("chunked-no-eof", eofs == 0);
    }

    // True unexpected EOF (no verdict line) must still fire on_unexpected_eof.
    {
        SDE::PolkitHelper helper;
        int eofs = 0;
        helper.on_unexpected_eof = [&] { ++eofs; };
        helper.feed_bytes("PAM_TEXT_INFO Hello\n"sv.bytes(), true);
        check("unexpected-eof", eofs == 1);
    }
}

void test_password_cache()
{
    auto t0 = MonotonicTime::now();
    SDE::PasswordCache cache(AK::Duration::from_seconds(60));

    check("cache-set-alice", cache.set("alice", "pw-alice"sv.bytes(), t0));
    check("cache-set-root", cache.set("root", "pw-root"sv.bytes(), t0));
    check("cache-size-2", cache.size() == 2);

    auto alice = cache.get("alice"sv, t0 + AK::Duration::from_seconds(30));
    auto root = cache.get("root"sv, t0 + AK::Duration::from_seconds(30));
    check("cache-alice-hit", alice.has_value() && StringView { alice->data(), alice->size() } == "pw-alice"sv);
    check("cache-root-hit", root.has_value() && StringView { root->data(), root->size() } == "pw-root"sv);

    // Lookup refreshed timestamp to t0+30, so t0+80 is still within 60s TTL.
    auto alice_warm = cache.get("alice"sv, t0 + AK::Duration::from_seconds(80));
    check("cache-alice-warm", alice_warm.has_value());

    // After TTL elapses without access, entry expires and is evicted.
    auto root_expired = cache.get("root"sv, t0 + AK::Duration::from_seconds(95));
    check("cache-root-expired", !root_expired.has_value());
    check("cache-alice-still-present", cache.size() == 1);

    // clear_all wipes everything (used on SIGUSR1 lock/suspend).
    cache.clear_all();
    check("cache-cleared", cache.size() == 0);
    check("cache-alice-gone", !cache.get("alice"sv, t0 + AK::Duration::from_seconds(85)).has_value());
}

}

ErrorOr<int> serenity_main(Main::Arguments)
{
    test_dialog_output();
    test_helper_eof_race();
    test_password_cache();

    if (g_failures != 0) {
        warnln("{} check(s) failed", g_failures);
        return 1;
    }
    outln("ok: polkit dialog output keeps the password separate from the remember flag");
    return 0;
}
