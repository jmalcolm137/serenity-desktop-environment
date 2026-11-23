/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/Optional.h>
#include <AK/StringView.h>
#include <AK/Vector.h>

#include <xkbcommon/xkbcommon.h>

namespace SDELock {

enum class KeyAction {
    None,
    Changed,
    Submit,
    BlankDisplay, // Escape pressed when password is already empty
};

// Manages XKB keymap/modifiers, key repeat timing, and the in-memory password
// codepoint buffer. All password bytes are wiped with explicit_bzero().
class KeyInput {
public:
    KeyInput();
    ~KeyInput();

    KeyInput(KeyInput const&) = delete;
    KeyInput& operator=(KeyInput const&) = delete;

    // Takes ownership of `fd` and always closes it (even on error).
    bool set_keymap_fd(uint32_t format, int fd, uint32_t size);
    bool update_modifiers(uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group);
    void set_repeat_info(int32_t rate, int32_t delay);
    void cancel_repeat();

    KeyAction handle_key(uint32_t keycode, bool pressed, long now_ms);
    KeyAction tick_repeat(long now_ms);
    Optional<long> next_repeat_delay_ms(long now_ms) const;

    // Pure-logic entry point (also used directly by unit tests).
    KeyAction apply_symbol(xkb_keysym_t sym, bool ctrl, bool alt, StringView utf8);

    size_t password_length() const { return m_password.size(); }
    bool password_empty() const { return m_password.is_empty(); }
    void clear_password();

    // Encodes the password directly into a wipeable UTF-8 ByteBuffer and wipes
    // the internal codepoint vector.
    ByteBuffer take_password_utf8();

    bool caps_lock() const { return m_caps_lock; }
    ByteString const& layout_name() const { return m_layout_name; }
    bool keymap_ready() const { return m_xkb_state != nullptr; }

private:
    KeyAction dispatch_keycode(uint32_t keycode);
    void update_layout_name(uint32_t group);

    xkb_context* m_xkb_context { nullptr };
    xkb_keymap* m_xkb_keymap { nullptr };
    xkb_state* m_xkb_state { nullptr };

    Vector<u32> m_password;
    bool m_caps_lock { false };
    ByteString m_layout_name;

    int32_t m_repeat_rate { 25 };  // keys per second (0 = disabled)
    int32_t m_repeat_delay { 400 }; // milliseconds
    Optional<uint32_t> m_repeat_keycode;
    long m_next_repeat_ms { 0 };
};

}
