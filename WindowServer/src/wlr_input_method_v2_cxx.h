/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

// <wlr/types/wlr_input_method_v2.h> cannot be compiled by a C++ compiler: its
// `wlr_input_method_v2_state` struct has a member literally named `delete`, which
// is a C++ keyword. The preprocessor does not care about keywords, so rename that
// one member (the header's only `delete` token) while including it; refer to the
// field as `delete_surrounding` from C++ code. Keep this the one place that pulls
// in the input-method header.
extern "C" {
#define delete delete_surrounding
#include <wlr/types/wlr_input_method_v2.h>
#undef delete
}
