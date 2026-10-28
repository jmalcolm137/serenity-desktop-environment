/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Optional.h>
#include <AK/StringView.h>
#include <AK/Vector.h>

namespace SDE {

struct DialogOutput {
    ByteString password;
    bool remember { false };
};

// sde-auth-dialog writes the password and then the remember flag, one per line.
// LibWM may print diagnostics before those lines, so the flag is the last line
// and the password is the line before it. Requiring the flag is important: an
// unchecked checkbox writes "0", which must not be mistaken for the password.
inline Optional<DialogOutput> parse_dialog_output(StringView output)
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

    if (lines.size() < 2)
        return {};

    auto flag = lines.last();
    if (flag != "0"sv && flag != "1"sv)
        return {};

    return DialogOutput { ByteString { lines[lines.size() - 2] }, flag == "1"sv };
}

}
