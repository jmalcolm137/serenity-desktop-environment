/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "dbus.h"

// The freedesktop USB portal's backend (org.freedesktop.impl.portal.Usb): the
// frontend (xdg-desktop-portal) enumerates the connected devices (udev) and opens
// the device file for the app; this backend decides whether the app may have it.
namespace Usb {

void register_interface(Portal::Service&);

}
