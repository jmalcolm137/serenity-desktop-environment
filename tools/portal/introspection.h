/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/StringView.h>

// The D-Bus introspection XML for each impl-portal interface, so the backend
// can answer org.freedesktop.DBus.Introspectable (as GDBus did for us before).
namespace Portal {

Optional<StringView> interface_introspection_xml(StringView interface_name);

}
