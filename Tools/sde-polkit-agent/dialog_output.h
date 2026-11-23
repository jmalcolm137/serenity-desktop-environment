/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/Optional.h>
#include <AK/Span.h>
#include <AK/StringView.h>
#include <AK/Vector.h>

#include <string.h>

namespace SDE {

struct DialogOutput {
    ByteString password;
    bool remember { false };
    Optional<ByteString> selected_user;
};

struct SecureDialogOutput {
    ByteBuffer password;
    bool remember { false };
    Optional<ByteString> selected_user;

    void wipe()
    {
        if (!password.is_empty())
            explicit_bzero(password.data(), password.size());
        password.clear();
    }
};

namespace Detail {

inline Vector<StringView> split_dialog_lines(StringView output)
{
    Vector<StringView> lines;
    size_t start = 0;
    for (;;) {
        auto newline = output.find('\n', start);
        if (!newline.has_value()) {
            if (start < output.length())
                lines.append(output.substring_view(start));
            break;
        }
        lines.append(output.substring_view(start, *newline - start));
        start = *newline + 1;
    }
    return lines;
}

struct ParsedSlices {
    StringView password;
    bool remember { false };
    Optional<StringView> selected_user;
};

inline Optional<ParsedSlices> parse_dialog_slices(StringView output)
{
    auto lines = split_dialog_lines(output);
    if (lines.size() < 2)
        return {};

    Optional<StringView> selected_user;
    if (lines.last().starts_with("USER:"sv)) {
        auto user = lines.take_last().substring_view(5);
        if (!user.is_empty())
            selected_user = user;
        if (lines.size() < 2)
            return {};
    }

    auto flag = lines.last();
    if (flag != "0"sv && flag != "1"sv)
        return {};

    return ParsedSlices {
        .password = lines[lines.size() - 2],
        .remember = flag == "1"sv,
        .selected_user = selected_user,
    };
}

}

// sde-auth-dialog writes the password and then the remember flag, one per line
// (plus an optional trailing "USER:<name>" line when multiple identities were
// offered). LibWM may print diagnostics before those lines when stdout is used,
// so the structured lines are read from the end. Requiring the flag is
// important: an unchecked checkbox writes "0", which must not be mistaken for
// the password.
inline Optional<DialogOutput> parse_dialog_output(StringView output)
{
    auto parsed = Detail::parse_dialog_slices(output);
    if (!parsed.has_value())
        return {};
    Optional<ByteString> user;
    if (parsed->selected_user.has_value())
        user = ByteString { *parsed->selected_user };
    return DialogOutput { ByteString { parsed->password }, parsed->remember, move(user) };
}

// Variant that stores the password in a wipeable ByteBuffer instead of an
// immutable ByteString, so callers can explicit_bzero() all plaintext copies.
inline Optional<SecureDialogOutput> parse_dialog_output_secure(ReadonlyBytes output)
{
    StringView view { output };
    auto parsed = Detail::parse_dialog_slices(view);
    if (!parsed.has_value())
        return {};
    auto copy = ByteBuffer::copy(parsed->password.bytes());
    if (copy.is_error())
        return {};
    Optional<ByteString> user;
    if (parsed->selected_user.has_value())
        user = ByteString { *parsed->selected_user };
    return SecureDialogOutput { copy.release_value(), parsed->remember, move(user) };
}

}
