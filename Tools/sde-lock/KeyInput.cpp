/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "KeyInput.h"

#include <AK/Debug.h>
#include <AK/Utf8View.h>

#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client-protocol.h>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace SDELock {

namespace {

void append_utf8_codepoint(ByteBuffer& buffer, u32 cp)
{
    u8 bytes[4];
    size_t count = 0;
    if (cp <= 0x7F) {
        bytes[0] = static_cast<u8>(cp);
        count = 1;
    } else if (cp <= 0x7FF) {
        bytes[0] = static_cast<u8>(0xC0 | (cp >> 6));
        bytes[1] = static_cast<u8>(0x80 | (cp & 0x3F));
        count = 2;
    } else if (cp <= 0xFFFF) {
        bytes[0] = static_cast<u8>(0xE0 | (cp >> 12));
        bytes[1] = static_cast<u8>(0x80 | ((cp >> 6) & 0x3F));
        bytes[2] = static_cast<u8>(0x80 | (cp & 0x3F));
        count = 3;
    } else if (cp <= 0x10FFFF) {
        bytes[0] = static_cast<u8>(0xF0 | (cp >> 18));
        bytes[1] = static_cast<u8>(0x80 | ((cp >> 12) & 0x3F));
        bytes[2] = static_cast<u8>(0x80 | ((cp >> 6) & 0x3F));
        bytes[3] = static_cast<u8>(0x80 | (cp & 0x3F));
        count = 4;
    }
    if (count > 0)
        (void)buffer.try_append(bytes, count);
    explicit_bzero(bytes, sizeof(bytes));
}

}

KeyInput::KeyInput()
{
    m_xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
}

KeyInput::~KeyInput()
{
    clear_password();
    if (m_xkb_state)
        xkb_state_unref(m_xkb_state);
    if (m_xkb_keymap)
        xkb_keymap_unref(m_xkb_keymap);
    if (m_xkb_context)
        xkb_context_unref(m_xkb_context);
}

bool KeyInput::set_keymap_fd(uint32_t format, int fd, uint32_t size)
{
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 || !m_xkb_context || size == 0) {
        if (fd >= 0)
            close(fd);
        return false;
    }

    auto* map = static_cast<char*>(mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0));
    close(fd);
    if (map == MAP_FAILED)
        return false;

    auto* new_keymap = xkb_keymap_new_from_string(m_xkb_context, map, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map, size);
    if (!new_keymap)
        return false;

    auto* new_state = xkb_state_new(new_keymap);
    if (!new_state) {
        xkb_keymap_unref(new_keymap);
        return false;
    }

    if (m_xkb_state)
        xkb_state_unref(m_xkb_state);
    if (m_xkb_keymap)
        xkb_keymap_unref(m_xkb_keymap);

    m_xkb_keymap = new_keymap;
    m_xkb_state = new_state;
    cancel_repeat();
    update_layout_name(0);
    dbgln("sde-lock: keymap ready");
    return true;
}

bool KeyInput::update_modifiers(uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group)
{
    if (!m_xkb_state)
        return false;

    xkb_state_update_mask(m_xkb_state, depressed, latched, locked, 0, 0, group);
    bool const caps = xkb_state_mod_name_is_active(m_xkb_state, XKB_MOD_NAME_CAPS, XKB_STATE_MODS_LOCKED) > 0;
    auto const old_layout = m_layout_name;
    update_layout_name(group);

    bool changed = (caps != m_caps_lock) || (m_layout_name != old_layout);
    m_caps_lock = caps;
    return changed;
}

void KeyInput::update_layout_name(uint32_t group)
{
    if (!m_xkb_keymap || xkb_keymap_num_layouts(m_xkb_keymap) <= 1) {
        m_layout_name = {};
        return;
    }
    if (auto const* name = xkb_keymap_layout_get_name(m_xkb_keymap, group); name && *name)
        m_layout_name = ByteString(name);
    else
        m_layout_name = {};
}

void KeyInput::set_repeat_info(int32_t rate, int32_t delay)
{
    m_repeat_rate = max(0, rate);
    m_repeat_delay = max(0, delay);
    if (m_repeat_rate == 0)
        cancel_repeat();
}

void KeyInput::cancel_repeat()
{
    m_repeat_keycode.clear();
    m_next_repeat_ms = 0;
}

KeyAction KeyInput::handle_key(uint32_t keycode, bool pressed, long now_ms)
{
    if (!pressed) {
        if (m_repeat_keycode.has_value() && *m_repeat_keycode == keycode)
            cancel_repeat();
        return KeyAction::None;
    }

    if (!m_xkb_state)
        return KeyAction::None;

    auto action = dispatch_keycode(keycode);
    if (action == KeyAction::Changed && m_repeat_rate > 0 && m_xkb_keymap
        && xkb_keymap_key_repeats(m_xkb_keymap, keycode + 8)) {
        m_repeat_keycode = keycode;
        m_next_repeat_ms = now_ms + m_repeat_delay;
    } else {
        cancel_repeat();
    }
    return action;
}

KeyAction KeyInput::tick_repeat(long now_ms)
{
    if (!m_repeat_keycode.has_value() || m_repeat_rate <= 0 || !m_xkb_state)
        return KeyAction::None;
    if (now_ms < m_next_repeat_ms)
        return KeyAction::None;

    long const interval = max(1L, 1000L / m_repeat_rate);
    m_next_repeat_ms = now_ms + interval;
    auto action = dispatch_keycode(*m_repeat_keycode);
    if (action != KeyAction::Changed)
        cancel_repeat();
    return action;
}

Optional<long> KeyInput::next_repeat_delay_ms(long now_ms) const
{
    if (!m_repeat_keycode.has_value() || m_repeat_rate <= 0)
        return {};
    return max(0L, m_next_repeat_ms - now_ms);
}

KeyAction KeyInput::dispatch_keycode(uint32_t keycode)
{
    auto const sym = xkb_state_key_get_one_sym(m_xkb_state, keycode + 8);
    bool const ctrl = xkb_state_mod_name_is_active(m_xkb_state, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_DEPRESSED) > 0;
    bool const alt = xkb_state_mod_name_is_active(m_xkb_state, XKB_MOD_NAME_ALT, XKB_STATE_MODS_DEPRESSED) > 0;

    char buffer[64];
    int const length = xkb_state_key_get_utf8(m_xkb_state, keycode + 8, buffer, sizeof(buffer));
    StringView utf8 = length > 0 ? StringView { buffer, static_cast<size_t>(length) } : StringView {};

    auto action = apply_symbol(sym, ctrl, alt, utf8);
    explicit_bzero(buffer, sizeof(buffer));
    return action;
}

KeyAction KeyInput::apply_symbol(xkb_keysym_t sym, bool ctrl, bool alt, StringView utf8)
{
    if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter)
        return KeyAction::Submit;

    if (sym == XKB_KEY_BackSpace) {
        if (m_password.is_empty())
            return KeyAction::None;
        m_password.last() = 0;
        m_password.take_last();
        return KeyAction::Changed;
    }

    if (sym == XKB_KEY_Escape) {
        if (m_password.is_empty())
            return KeyAction::BlankDisplay;
        clear_password();
        return KeyAction::Changed;
    }

    if (sym == XKB_KEY_Delete || (ctrl && (sym == XKB_KEY_u || sym == XKB_KEY_U))) {
        if (m_password.is_empty())
            return KeyAction::None;
        clear_password();
        return KeyAction::Changed;
    }

    // Never turn Ctrl/Alt chords into password text.
    if (ctrl || alt || utf8.is_empty())
        return KeyAction::None;

    bool changed = false;
    Utf8View view { utf8 };
    for (auto codepoint : view) {
        if (codepoint < 0x20 || codepoint == 0x7f)
            continue;
        m_password.append(codepoint);
        changed = true;
    }
    return changed ? KeyAction::Changed : KeyAction::None;
}

void KeyInput::clear_password()
{
    if (!m_password.is_empty())
        explicit_bzero(m_password.data(), m_password.size() * sizeof(u32));
    m_password.clear();
}

ByteBuffer KeyInput::take_password_utf8()
{
    ByteBuffer out;
    for (auto codepoint : m_password)
        append_utf8_codepoint(out, codepoint);
    clear_password();
    return out;
}

}
