/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "dbus.h"

// R5 GlobalShortcuts backend (org.freedesktop.impl.portal.GlobalShortcuts): the
// requested shortcuts are stored in the SDE config system (global-shortcuts.ini)
// for the compositor to bind, and activations come back over a session FIFO
// which this backend turns into the Activated signal.
namespace GlobalShortcuts {

void register_interface(Portal::Service&);

}
