/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StringView.h>

namespace Trace {

// A diagnostic event stream, off unless SDE_TRACE_FILE names a file. One JSON
// object per line: {"t":<ms since start>,"cat":"...","msg":"..."}. Events are
// written synchronously on the event loop, so this is for debugging, not for an
// always-on audit log; an unset variable costs one branch.
void initialize();
void finish();
bool enabled();
void event(StringView category, StringView message);

}
