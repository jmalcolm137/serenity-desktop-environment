/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Regression: sde-polkit-agent parsed sde-auth-dialog's output as
//
//   <password>
//   1|0
//
// but when the "Do not ask again this session" checkbox was unchecked it used
// the last line ("0") as the password. Every default polkit authentication
// therefore sent "0" to pam_unix and failed with a correct password.
//
// This checks the parser with both checkbox states, with LibWM diagnostics
// before the password, with an empty password, and with malformed output.
//
// The driver is scripts/run-polkit-dialog-output-test.sh.
#include "../Tools/sde-polkit-agent/dialog_output.h"
#include <AK/Format.h>
#include <LibMain/Main.h>

namespace {

int g_failures = 0;

void expect(char const* name, StringView output, Optional<ByteString> password, Optional<bool> remember)
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
}

}

ErrorOr<int> serenity_main(Main::Arguments)
{
    // The regression: an unchecked checkbox writes "0" on the second line.
    expect("unchecked", "hunter2\n0\n"sv, ByteString { "hunter2" }, false);
    expect("checked", "hunter2\n1\n"sv, ByteString { "hunter2" }, true);

    // LibWM diagnostics may be printed before the dialog's two lines.
    expect("diagnostics", "LibWM: connected\nLibWM: window presented\nhunter2\n0\n"sv, ByteString { "hunter2" }, false);
    expect("diagnostics-checked", "LibWM: connected\nhunter2\n1\n"sv, ByteString { "hunter2" }, true);

    // Empty passwords and a password that looks like the flag.
    expect("empty", "\n0\n"sv, ByteString {}, false);
    expect("flag-like-password", "0\n0\n"sv, ByteString { "0" }, false);

    // Missing/invalid flag, and no output at all.
    expect("missing-flag", "hunter2\n"sv, {}, {});
    expect("invalid-flag", "hunter2\n2\n"sv, {}, {});
    expect("empty-output", ""sv, {}, {});

    // The dialog uses outln(), but accepting a final line without a newline is
    // harmless and keeps the parser robust.
    expect("no-trailing-newline", "hunter2\n0"sv, ByteString { "hunter2" }, false);

    if (g_failures != 0) {
        warnln("{} check(s) failed", g_failures);
        return 1;
    }
    outln("ok: polkit dialog output keeps the password separate from the remember flag");
    return 0;
}
