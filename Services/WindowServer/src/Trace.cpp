/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Trace.h"

#include <AK/JsonObject.h>
#include <LibCore/File.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace Trace {

static OwnPtr<Core::File> s_file;
static u64 s_start_ms;

static u64 monotonic_ms()
{
    timespec now { };
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<u64>(now.tv_sec) * 1000 + static_cast<u64>(now.tv_nsec) / 1'000'000;
}

void initialize()
{
    if (s_file)
        return;
    auto const* path = getenv("SDE_TRACE_FILE");
    if (!path || !*path)
        return;
    auto file = Core::File::open(StringView { path, strlen(path) }, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate);
    if (file.is_error()) {
        warnln("WindowServer: could not open trace file '{}': {}", path, file.error());
        return;
    }
    s_file = file.release_value();
    s_start_ms = monotonic_ms();
    event("trace"sv, "started"sv);
}

void finish()
{
    s_file = nullptr;
}

bool enabled()
{
    return s_file != nullptr;
}

void event(StringView category, StringView message)
{
    if (!s_file)
        return;
    JsonObject object;
    object.set("t", static_cast<long long>(monotonic_ms() - s_start_ms));
    object.set("cat", category);
    object.set("msg", message);
    auto line = ByteString::formatted("{}\n", object.to_byte_string());
    (void)s_file->write_until_depleted(line.bytes());
}

}
