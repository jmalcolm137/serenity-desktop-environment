/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Model.h"

// R5 IME (docs/22-input-method.md; InputMethod.cpp): text-input-v3 +
// input-method-v2. The compositor relays text-input state between the focused
// client and the (single) input method, and routes keys to the input method's
// keyboard grab.
struct ImeState {
    wlr_text_input_manager_v3* text_input_manager { nullptr };
    wl_listener text_input_new { };
    wlr_input_method_manager_v2* input_method_manager { nullptr };
    wl_listener input_method_new { };
    Vector<NonnullOwnPtr<TextInput>> text_inputs;
    Vector<NonnullOwnPtr<InputPopup>> input_popups;
    Vector<NonnullOwnPtr<InputMethodClient>> input_methods;
    wlr_input_method_v2* input_method { nullptr };
    // Scene tree for input-method popups (candidate windows), above the shell.
    wlr_scene_tree* popup_tree { nullptr };
    wl_listener grab_destroy { };
};
