/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "dbus.h"

// The freedesktop Inhibit portal's backend (org.freedesktop.impl.portal.Inhibit):
// an application asks the session not to end, suspend, idle or switch away. The
// impl method returns no value, so the result travels through the Request object
// exported at the call's handle (org.freedesktop.impl.portal.Request); the
// inhibition lasts until the caller closes it. See docs/15 §5.
namespace Inhibit {

void register_interface(Portal::Service&);

}
