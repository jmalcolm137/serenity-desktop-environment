/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibXDG/BaseDirs.h>
#include <LibXDG/MimeDatabase.h>

#include <AK/HashTable.h>
#include <AK/LexicalPath.h>
#include <LibCore/File.h>

#include <fnmatch.h>
#include <sys/stat.h>

namespace XDG {

static ByteString ascii_lowercase(StringView s)
{
    return s.to_byte_string().to_lowercase();
}

void MimeDatabase::load_mime_dir(ByteString const& mime_dir)
{
    // 1. globs2: weight:mime/type:pattern[:flags]
    if (auto file = Core::File::open(ByteString::formatted("{}/globs2", mime_dir), Core::File::OpenMode::Read); !file.is_error()) {
        if (auto contents = file.value()->read_until_eof(); !contents.is_error()) {
            StringView text { contents.value() };
            for (auto raw_line : text.lines()) {
                auto line = raw_line.trim_whitespace();
                if (line.is_empty() || line.starts_with('#'))
                    continue;
                auto parts = line.split_view(':', SplitBehavior::KeepEmpty);
                if (parts.size() < 3)
                    continue;
                auto weight = parts[0].to_number<int>();
                if (!weight.has_value())
                    continue;
                bool case_sensitive = parts.size() >= 4 && parts[3].contains("cs"sv);
                m_globs.append({
                    .weight = weight.value(),
                    .mime_type = ascii_lowercase(parts[1]),
                    .pattern = parts[2].to_byte_string(),
                    .case_sensitive = case_sensitive,
                });
            }
        }
    }

    // 2. aliases: alias canonical
    if (auto file = Core::File::open(ByteString::formatted("{}/aliases", mime_dir), Core::File::OpenMode::Read); !file.is_error()) {
        if (auto contents = file.value()->read_until_eof(); !contents.is_error()) {
            StringView text { contents.value() };
            for (auto raw_line : text.lines()) {
                auto line = raw_line.trim_whitespace();
                if (line.is_empty() || line.starts_with('#'))
                    continue;
                auto space = line.find(' ');
                if (!space.has_value())
                    continue;
                auto alias = ascii_lowercase(line.substring_view(0, space.value()).trim_whitespace());
                auto canonical = ascii_lowercase(line.substring_view(space.value() + 1).trim_whitespace());
                if (!alias.is_empty() && !canonical.is_empty() && !m_aliases.contains(alias))
                    m_aliases.set(move(alias), move(canonical));
            }
        }
    }

    // 3. subclasses: child parent
    if (auto file = Core::File::open(ByteString::formatted("{}/subclasses", mime_dir), Core::File::OpenMode::Read); !file.is_error()) {
        if (auto contents = file.value()->read_until_eof(); !contents.is_error()) {
            StringView text { contents.value() };
            for (auto raw_line : text.lines()) {
                auto line = raw_line.trim_whitespace();
                if (line.is_empty() || line.starts_with('#'))
                    continue;
                auto space = line.find(' ');
                if (!space.has_value())
                    continue;
                auto child = ascii_lowercase(line.substring_view(0, space.value()).trim_whitespace());
                auto parent = ascii_lowercase(line.substring_view(space.value() + 1).trim_whitespace());
                if (!child.is_empty() && !parent.is_empty()) {
                    auto& parents = m_subclasses.ensure(child);
                    if (!parents.contains_slow(parent))
                        parents.append(move(parent));
                }
            }
        }
    }
}

MimeDatabase MimeDatabase::load()
{
    MimeDatabase db;
    db.load_mime_dir(ByteString::formatted("{}/mime", data_home()));
    for (auto const& dir : data_dirs())
        db.load_mime_dir(ByteString::formatted("{}/mime", dir));
    return db;
}

ByteString MimeDatabase::canonical_mime_type(StringView mime_type) const
{
    auto lower = ascii_lowercase(mime_type.trim_whitespace());
    if (auto it = m_aliases.find(lower); it != m_aliases.end())
        return it->value;
    return lower;
}

Vector<ByteString> MimeDatabase::superclasses(StringView mime_type) const
{
    Vector<ByteString> result;
    HashTable<ByteString> visited;
    auto start = canonical_mime_type(mime_type);
    visited.set(start);

    Vector<ByteString> queue;
    queue.append(start);
    size_t head = 0;
    while (head < queue.size()) {
        auto current = queue[head++];
        auto it = m_subclasses.find(current);
        if (it == m_subclasses.end())
            continue;
        for (auto const& parent_raw : it->value) {
            auto parent = canonical_mime_type(parent_raw);
            if (visited.contains(parent))
                continue;
            visited.set(parent);
            result.append(parent);
            queue.append(move(parent));
        }
    }
    return result;
}

static ByteString fallback_mime_for_extension(StringView basename)
{
    auto dot = basename.find_last('.');
    if (!dot.has_value() || dot.value() + 1 >= basename.length())
        return "application/octet-stream";
    auto ext = ascii_lowercase(basename.substring_view(dot.value() + 1));
    if (ext == "txt"sv || ext == "log"sv || ext == "ini"sv || ext == "conf"sv)
        return "text/plain";
    if (ext == "md"sv)
        return "text/markdown";
    if (ext == "html"sv || ext == "htm"sv)
        return "text/html";
    if (ext == "pdf"sv)
        return "application/pdf";
    if (ext == "png"sv)
        return "image/png";
    if (ext == "jpg"sv || ext == "jpeg"sv)
        return "image/jpeg";
    if (ext == "gif"sv)
        return "image/gif";
    if (ext == "svg"sv)
        return "image/svg+xml";
    if (ext == "json"sv)
        return "application/json";
    if (ext == "mp4"sv)
        return "video/mp4";
    if (ext == "mp3"sv)
        return "audio/mpeg";
    if (ext == "wav"sv)
        return "audio/wav";
    return "application/octet-stream";
}

ByteString MimeDatabase::mime_type_for_filename(StringView path_or_filename) const
{
    auto basename = LexicalPath::basename(path_or_filename);
    if (basename.is_empty())
        return "application/octet-stream";

    GlobRule const* best = nullptr;
    for (auto const& rule : m_globs) {
        int flags = rule.case_sensitive ? 0 : FNM_CASEFOLD;
        if (::fnmatch(rule.pattern.characters(), basename.characters(), flags) != 0)
            continue;
        if (!best
            || rule.weight > best->weight
            || (rule.weight == best->weight && rule.pattern.length() > best->pattern.length())) {
            best = &rule;
        }
    }

    if (best)
        return canonical_mime_type(best->mime_type);
    return fallback_mime_for_extension(basename);
}

ByteString MimeDatabase::mime_type_for_uri(StringView uri_or_path) const
{
    auto trimmed = uri_or_path.trim_whitespace();
    if (trimmed.is_empty())
        return "application/octet-stream";

    // Scheme URI (e.g. https://..., mailto:...)
    if (auto colon = trimmed.find(':'); colon.has_value() && colon.value() > 0) {
        auto scheme = trimmed.substring_view(0, colon.value());
        bool valid_scheme = true;
        for (char c : scheme) {
            bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                || (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.';
            if (!ok) {
                valid_scheme = false;
                break;
            }
        }
        if (valid_scheme) {
            auto lower_scheme = ascii_lowercase(scheme);
            if (lower_scheme == "file"sv && trimmed.starts_with("file://"sv)) {
                auto local_path = trimmed.substring_view(7);
                struct stat st {};
                if (::stat(local_path.to_byte_string().characters(), &st) == 0 && S_ISDIR(st.st_mode))
                    return "inode/directory";
                return mime_type_for_filename(local_path);
            }
            return ByteString::formatted("x-scheme-handler/{}", lower_scheme);
        }
    }

    struct stat st {};
    if (::stat(trimmed.to_byte_string().characters(), &st) == 0 && S_ISDIR(st.st_mode))
        return "inode/directory";

    return mime_type_for_filename(trimmed);
}

}
