/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Model.h"

// Input devices (InputDevices.cpp, Keyboard.cpp): keyboards, pointers, touch and
// switches, the virtual-input managers the headless tests drive, and the
// native-app glue protocols that act on them. InputDevices.cpp handles hotplug;
// Keyboard.cpp the keyboards and the seat capabilities they imply.
struct InputState {
    Vector<NonnullOwnPtr<Keyboard>> keyboards;
    // Pointer devices, so input.ini's libinput settings can be (re)applied when
    // the config changes. See R4.6.
    Vector<wlr_input_device*> pointer_devices;
    Vector<wlr_input_device*> touch_devices;
    // Live virtual touch devices (serenity-virtual-input); the seat advertises
    // the touch capability while any exists.
    int virtual_touch_count { 0 };
    Vector<NonnullOwnPtr<Pointer>> pointer_objects;
    Vector<NonnullOwnPtr<SwitchDevice>> switch_objects;
    // wp-pointer-gestures-v1: touchpad swipe/pinch/hold.
    wlr_pointer_gestures_v1* pointer_gestures { nullptr };
    // Virtual input (wlr virtual-keyboard / virtual-pointer), so headless tests
    // can script keyboard and pointer events. See docs/09 §2.4.
    wlr_virtual_keyboard_manager_v1* virtual_keyboard_manager { nullptr };
    wlr_virtual_pointer_manager_v1* virtual_pointer_manager { nullptr };
    // R5: keyboard shortcut inhibition (native-app compatibility; the keyboard
    // path asks it before handling a compositor shortcut).
    wlr_keyboard_shortcuts_inhibit_manager_v1* keyboard_shortcuts_inhibit { nullptr };
    wl_listener new_input { };
    wl_listener new_virtual_keyboard { };
    wl_listener new_virtual_pointer { };
};
