/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/Optional.h>
#include <AK/StringView.h>
#include <AK/Vector.h>

// Host helpers shared by the portal modules: the session environment, the state
// directory the portals own, and running the configured helper programs (the
// LibGUI dialogs, the session opener/notifier).
namespace Portal {

// A session environment variable, if set.
Optional<ByteString> environment(StringView name);

// $XDG_RUNTIME_DIR (or /tmp when the session has none).
ByteString runtime_dir();

// A file in the portal's state directory: SDE_STATE_DIR, else
// $XDG_CONFIG_HOME/sde, else $HOME/.config/sde. The directory is created.
ByteString state_file(StringView name);

// Run a command, optionally capturing its (trimmed) stdout.
ErrorOr<Optional<ByteString>> run_command(Vector<ByteString> const& arguments, bool capture_stdout);

// Start a command without waiting for it (fire and forget). Returns an error only
// when the process could not be started.
ErrorOr<void> spawn_async(Vector<ByteString> const& arguments);

}
