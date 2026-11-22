/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/StringView.h>
#include <AK/Vector.h>

namespace XDG {

// XDG Base Directory Specification (v0.8):
// Relative paths in XDG_* environment variables are ignored per the spec.
ByteString home_directory();
ByteString config_home();
ByteString data_home();
ByteString state_home();
ByteString cache_home();
ByteString runtime_dir();

Vector<ByteString> config_dirs();
Vector<ByteString> data_dirs();

// SDE-namespaced directories. SDE_STATE_DIR (used by tests and explicit
// overrides) takes precedence for both config and state when set.
ByteString sde_config_dir();
ByteString sde_state_dir();
ByteString sde_cache_dir();

// Existing files matching `relative_path` in priority order (user home first,
// then system dirs).
Vector<ByteString> find_config_files(StringView relative_path);
Vector<ByteString> find_data_files(StringView relative_path);

// Active desktop names from $XDG_CURRENT_DESKTOP (colon-separated; defaults to
// {"SDE"} when unset or empty).
Vector<ByteString> current_desktops();

}
