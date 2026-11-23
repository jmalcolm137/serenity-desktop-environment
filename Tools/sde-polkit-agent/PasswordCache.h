/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/Span.h>
#include <AK/StringView.h>
#include <AK/Time.h>

#include <string.h>

namespace SDE {

// In-memory, per-user password cache for sde-polkit-agent.
//
// Passwords are never written to disk, are wiped with explicit_bzero() when
// evicted/expired/cleared, and are keyed by username so authenticating as root
// and as the session user do not overwrite each other.
class PasswordCache {
public:
    // Default idle TTL: 15 minutes. A zero duration disables TTL expiry
    // (entries live until lock/suspend SIGUSR1, failed auth, or agent exit).
    static constexpr AK::Duration default_ttl = AK::Duration::from_seconds(15 * 60);

    explicit PasswordCache(AK::Duration ttl = default_ttl)
        : m_ttl(ttl)
    {
    }

    ~PasswordCache()
    {
        clear_all();
    }

    void set_ttl(AK::Duration ttl) { m_ttl = ttl; }
    AK::Duration ttl() const { return m_ttl; }

    bool set(ByteString const& username, ReadonlyBytes password, MonotonicTime now = MonotonicTime::now())
    {
        if (username.is_empty())
            return false;
        clear(username);
        auto copy = ByteBuffer::copy(password);
        if (copy.is_error())
            return false;
        m_entries.set(username, Entry { copy.release_value(), now });
        return true;
    }

    Optional<ByteBuffer> get(StringView username, MonotonicTime now = MonotonicTime::now())
    {
        auto it = m_entries.find(username);
        if (it == m_entries.end())
            return {};

        if (!m_ttl.is_zero() && (now - it->value.stored_at) >= m_ttl) {
            wipe_buffer(it->value.password);
            m_entries.remove(it);
            return {};
        }

        auto copy = ByteBuffer::copy(it->value.password.bytes());
        if (copy.is_error())
            return {};
        // Refresh timestamp on successful lookup so active sessions stay warm.
        it->value.stored_at = now;
        return copy.release_value();
    }

    void clear(StringView username)
    {
        auto it = m_entries.find(username);
        if (it == m_entries.end())
            return;
        wipe_buffer(it->value.password);
        m_entries.remove(it);
    }

    void clear_all()
    {
        for (auto& [key, entry] : m_entries)
            wipe_buffer(entry.password);
        m_entries.clear();
    }

    size_t size() const { return m_entries.size(); }

private:
    struct Entry {
        ByteBuffer password;
        MonotonicTime stored_at;
    };

    static void wipe_buffer(ByteBuffer& buffer)
    {
        if (!buffer.is_empty())
            explicit_bzero(buffer.data(), buffer.size());
        buffer.clear();
    }

    AK::Duration m_ttl { default_ttl };
    HashMap<ByteString, Entry> m_entries;
};

}
