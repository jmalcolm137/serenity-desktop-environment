/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibXDG/BaseDirs.h>
#include <LibXDG/RecentlyUsed.h>

#include <AK/StringBuilder.h>
#include <LibCore/Directory.h>
#include <LibCore/File.h>

#include <errno.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

namespace XDG {

static constexpr size_t max_stored_recent_entries = 200;

static ByteString current_utc_iso8601()
{
    time_t now = ::time(nullptr);
    struct tm tm_now {};
    ::gmtime_r(&now, &tm_now);
    char buf[32];
    ::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_now);
    return ByteString(buf);
}

static ByteString xml_escape(StringView input)
{
    StringBuilder out;
    for (char c : input) {
        switch (c) {
        case '&':
            out.append("&amp;"sv);
            break;
        case '<':
            out.append("&lt;"sv);
            break;
        case '>':
            out.append("&gt;"sv);
            break;
        case '"':
            out.append("&quot;"sv);
            break;
        case '\'':
            out.append("&apos;"sv);
            break;
        default:
            out.append(c);
            break;
        }
    }
    return out.to_byte_string();
}

static ByteString xml_unescape(StringView input)
{
    StringBuilder out;
    for (size_t i = 0; i < input.length(); ++i) {
        if (input[i] == '&') {
            auto rest = input.substring_view(i);
            if (rest.starts_with("&amp;"sv)) {
                out.append('&');
                i += 4;
                continue;
            }
            if (rest.starts_with("&lt;"sv)) {
                out.append('<');
                i += 3;
                continue;
            }
            if (rest.starts_with("&gt;"sv)) {
                out.append('>');
                i += 3;
                continue;
            }
            if (rest.starts_with("&quot;"sv)) {
                out.append('"');
                i += 5;
                continue;
            }
            if (rest.starts_with("&apos;"sv)) {
                out.append('\'');
                i += 5;
                continue;
            }
        }
        out.append(input[i]);
    }
    return out.to_byte_string();
}

static Optional<ByteString> extract_attr(StringView tag, StringView attr_name)
{
    auto needle = ByteString::formatted("{}=\"", attr_name);
    auto pos = tag.find(needle);
    if (!pos.has_value())
        return {};
    size_t start = pos.value() + needle.length();
    auto end = tag.find('"', start);
    if (!end.has_value())
        return {};
    return xml_unescape(tag.substring_view(start, end.value() - start));
}

static ByteString normalize_uri(StringView uri_or_path)
{
    if (uri_or_path.contains("://"sv))
        return uri_or_path.to_byte_string();
    if (uri_or_path.starts_with('/'))
        return ByteString::formatted("file://{}", uri_or_path);
    return uri_or_path.to_byte_string();
}

ByteString recently_used_path()
{
    return ByteString::formatted("{}/recently-used.xbel", data_home());
}

Vector<RecentEntry> read_recently_used(size_t max_entries)
{
    Vector<RecentEntry> entries;
    auto file = Core::File::open(recently_used_path(), Core::File::OpenMode::Read);
    if (file.is_error())
        return entries;
    auto contents = file.value()->read_until_eof();
    if (contents.is_error())
        return entries;

    StringView text { contents.value() };
    size_t cursor = 0;
    while (cursor < text.length() && entries.size() < max_entries) {
        auto bm_start = text.find("<bookmark "sv, cursor);
        if (!bm_start.has_value())
            break;
        auto bm_end = text.find("</bookmark>"sv, bm_start.value());
        if (!bm_end.has_value())
            break;

        auto block = text.substring_view(bm_start.value(), bm_end.value() - bm_start.value());
        auto header_end = block.find('>');
        if (header_end.has_value()) {
            auto header = block.substring_view(0, header_end.value());
            auto href = extract_attr(header, "href"sv);
            if (href.has_value() && !href->is_empty()) {
                RecentEntry entry;
                entry.uri = href.release_value();
                entry.added = extract_attr(header, "added"sv).value_or({});
                entry.modified = extract_attr(header, "modified"sv).value_or({});
                entry.visited = extract_attr(header, "visited"sv).value_or({});

                if (auto mime_pos = block.find("<mime:mime-type "sv); mime_pos.has_value()) {
                    if (auto mime_end = block.find('>', mime_pos.value()); mime_end.has_value()) {
                        auto mime_tag = block.substring_view(mime_pos.value(), mime_end.value() - mime_pos.value());
                        entry.mime_type = extract_attr(mime_tag, "type"sv).value_or({});
                    }
                }
                if (auto app_pos = block.find("<bookmark:application "sv); app_pos.has_value()) {
                    if (auto app_end = block.find('>', app_pos.value()); app_end.has_value()) {
                        auto app_tag = block.substring_view(app_pos.value(), app_end.value() - app_pos.value());
                        entry.app_name = extract_attr(app_tag, "name"sv).value_or({});
                        entry.app_exec = extract_attr(app_tag, "exec"sv).value_or({});
                    }
                }
                entries.append(move(entry));
            }
        }
        cursor = bm_end.value() + 11;
    }
    return entries;
}

ErrorOr<void> record_recently_used(
    StringView uri_or_path,
    StringView mime_type,
    StringView app_name,
    StringView app_exec)
{
    auto uri = normalize_uri(uri_or_path.trim_whitespace());
    if (uri.is_empty())
        return {};

    auto now = current_utc_iso8601();
    auto existing = read_recently_used(max_stored_recent_entries);

    ByteString added = now;
    Vector<RecentEntry> updated;
    updated.append({
        .uri = uri,
        .mime_type = mime_type.is_empty() ? ByteString { "application/octet-stream" } : mime_type.to_byte_string(),
        .added = now,
        .modified = now,
        .visited = now,
        .app_name = app_name.is_empty() ? ByteString { "SDE" } : app_name.to_byte_string(),
        .app_exec = app_exec.is_empty() ? ByteString { "sde-open %u" } : app_exec.to_byte_string(),
    });

    for (auto& entry : existing) {
        if (entry.uri == uri) {
            if (!entry.added.is_empty())
                added = entry.added;
            continue;
        }
        if (updated.size() < max_stored_recent_entries)
            updated.append(move(entry));
    }
    updated[0].added = move(added);

    auto data_dir = data_home();
    TRY(Core::Directory::create(data_dir, Core::Directory::CreateDirectories::Yes));

    StringBuilder xml;
    xml.append("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"sv);
    xml.append("<xbel version=\"1.0\"\n"
               "      xmlns:bookmark=\"http://www.freedesktop.org/standards/desktop-bookmarks\"\n"
               "      xmlns:mime=\"http://www.freedesktop.org/standards/shared-mime-info\">\n"sv);
    for (auto const& e : updated) {
        xml.appendff("  <bookmark href=\"{}\" added=\"{}\" modified=\"{}\" visited=\"{}\">\n",
            xml_escape(e.uri), xml_escape(e.added), xml_escape(e.modified), xml_escape(e.visited));
        xml.append("    <info>\n"
                   "      <metadata owner=\"http://freedesktop.org\">\n"sv);
        xml.appendff("        <mime:mime-type type=\"{}\"/>\n", xml_escape(e.mime_type));
        xml.append("        <bookmark:applications>\n"sv);
        xml.appendff("          <bookmark:application name=\"{}\" exec=\"{}\" modified=\"{}\" count=\"1\"/>\n",
            xml_escape(e.app_name), xml_escape(e.app_exec), xml_escape(e.modified));
        xml.append("        </bookmark:applications>\n"
                   "      </metadata>\n"
                   "    </info>\n"
                   "  </bookmark>\n"sv);
    }
    xml.append("</xbel>\n"sv);

    auto target = recently_used_path();
    auto tmp_path = ByteString::formatted("{}.tmp.{}", target, ::getpid());
    {
        auto out = TRY(Core::File::open(tmp_path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
        TRY(out->write_until_depleted(xml.string_view().bytes()));
    }
    if (::rename(tmp_path.characters(), target.characters()) != 0) {
        int err = errno;
        ::unlink(tmp_path.characters());
        return Error::from_errno(err);
    }
    return {};
}

}
