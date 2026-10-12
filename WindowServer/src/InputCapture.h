/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

struct Server;

// InputCapture portal support (R5, docs/17-input-capture.md).
//
// The compositor owns the EIS implementation (libeis) that backs
// org.freedesktop.impl.portal.InputCapture, and exposes the private
// serenity_input_capture protocol so the trusted portal backend can start a
// session and obtain its EIS fd. Only offered when SDE_INPUT_CAPTURE=1.
namespace InputCapture {

void initialize(Server& server);

}
