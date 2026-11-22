/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Optional.h>
#include <AK/StringView.h>

namespace XDG {

// freedesktop.org Sound Theme & Naming Specification (v0.1):
//   - Searches $XDG_DATA_HOME/sounds and $XDG_DATA_DIRS/sounds.
//   - Parses index.theme (Inherits=, Directories=), follows Inherits=
//     recursively, and always falls back to "freedesktop".
//   - Implements '-' suffix truncation fallback per spec
//     (e.g. "message-new-instant" -> "message-new" -> "message").
//   - Probes .oga, .ogg, .wav extensions.
Optional<ByteString> find_sound(StringView event_name, StringView theme_name = "freedesktop"sv);

}
