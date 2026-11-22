/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibXDG/BaseDirs.h>
#include <LibXDG/Trash.h>

#include <AK/LexicalPath.h>
#include <AK/QuickSort.h>
#include <AK/StringBuilder.h>
#include <AK/StringUtils.h>
#include <LibCore/Directory.h>
#include <LibCore/File.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

namespace XDG {

static ByteString percent_encode_path(StringView path)
{
    StringBuilder out;
    for (u8 c : path.bytes()) {
        bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
            || (c >= '0' && c <= '9') || c == '/' || c == '-' || c == '_'
            || c == '.' || c == '~';
        if (safe)
            out.append(static_cast<char>(c));
        else
            out.appendff("%{:02X}", c);
    }
    return out.to_byte_string();
}

static ByteString percent_decode_path(StringView encoded)
{
    StringBuilder out;
    for (size_t i = 0; i < encoded.length(); ++i) {
        if (encoded[i] == '%' && i + 2 < encoded.length()) {
            auto hex = AK::StringUtils::convert_to_uint_from_hex(encoded.substring_view(i + 1, 2));
            if (hex.has_value()) {
                out.append(static_cast<char>(hex.value()));
                i += 2;
                continue;
            }
        }
        out.append(encoded[i]);
    }
    return out.to_byte_string();
}

static ByteString current_iso8601_local()
{
    time_t now = ::time(nullptr);
    struct tm tm_now {};
    ::localtime_r(&now, &tm_now);
    char buf[32];
    ::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm_now);
    return ByteString(buf);
}

static ErrorOr<void> remove_recursive(ByteString const& path)
{
    struct stat st {};
    if (::lstat(path.characters(), &st) != 0)
        return Error::from_errno(errno);

    if (S_ISDIR(st.st_mode)) {
        DIR* dir = ::opendir(path.characters());
        if (!dir)
            return Error::from_errno(errno);
        while (auto* ent = ::readdir(dir)) {
            StringView name { ent->d_name, __builtin_strlen(ent->d_name) };
            if (name == "."sv || name == ".."sv)
                continue;
            auto child = ByteString::formatted("{}/{}", path, name);
            auto res = remove_recursive(child);
            if (res.is_error()) {
                ::closedir(dir);
                return res;
            }
        }
        ::closedir(dir);
        if (::rmdir(path.characters()) != 0)
            return Error::from_errno(errno);
        return {};
    }

    if (::unlink(path.characters()) != 0)
        return Error::from_errno(errno);
    return {};
}

static ErrorOr<void> copy_file_contents(ByteString const& src, ByteString const& dst, mode_t mode)
{
    int in_fd = ::open(src.characters(), O_RDONLY);
    if (in_fd < 0)
        return Error::from_errno(errno);
    int out_fd = ::open(dst.characters(), O_CREAT | O_WRONLY | O_TRUNC, mode & 0777);
    if (out_fd < 0) {
        int err = errno;
        ::close(in_fd);
        return Error::from_errno(err);
    }

    char buf[65536];
    for (;;) {
        ssize_t n = ::read(in_fd, buf, sizeof(buf));
        if (n == 0)
            break;
        if (n < 0) {
            if (errno == EINTR)
                continue;
            int err = errno;
            ::close(in_fd);
            ::close(out_fd);
            return Error::from_errno(err);
        }
        ssize_t written = 0;
        while (written < n) {
            ssize_t w = ::write(out_fd, buf + written, static_cast<size_t>(n - written));
            if (w < 0) {
                if (errno == EINTR)
                    continue;
                int err = errno;
                ::close(in_fd);
                ::close(out_fd);
                return Error::from_errno(err);
            }
            written += w;
        }
    }
    ::close(in_fd);
    ::close(out_fd);
    return {};
}

static ErrorOr<void> move_across_devices(ByteString const& src, ByteString const& dst)
{
    if (::rename(src.characters(), dst.characters()) == 0)
        return {};
    if (errno != EXDEV)
        return Error::from_errno(errno);

    struct stat st {};
    if (::lstat(src.characters(), &st) != 0)
        return Error::from_errno(errno);
    if (S_ISREG(st.st_mode)) {
        TRY(copy_file_contents(src, dst, st.st_mode));
        if (::unlink(src.characters()) != 0)
            return Error::from_errno(errno);
        return {};
    }
    return Error::from_errno(EXDEV);
}

ByteString trash_directory()
{
    return ByteString::formatted("{}/Trash", data_home());
}

ErrorOr<ByteString> trash_file(StringView path)
{
    if (path.is_empty())
        return Error::from_errno(EINVAL);

    ByteString raw_path = path.to_byte_string();
    char resolved_buf[PATH_MAX];
    ByteString abs_path = ::realpath(raw_path.characters(), resolved_buf)
        ? ByteString(resolved_buf)
        : raw_path;

    struct stat st {};
    if (::lstat(abs_path.characters(), &st) != 0)
        return Error::from_errno(errno);

    auto base_dir = trash_directory();
    auto files_dir = ByteString::formatted("{}/files", base_dir);
    auto info_dir = ByteString::formatted("{}/info", base_dir);
    TRY(Core::Directory::create(files_dir, Core::Directory::CreateDirectories::Yes));
    TRY(Core::Directory::create(info_dir, Core::Directory::CreateDirectories::Yes));
    (void)::chmod(base_dir.characters(), 0700);

    auto base_name = LexicalPath::basename(abs_path);
    if (base_name.is_empty() || base_name == "/"sv)
        return Error::from_errno(EINVAL);

    // Atomically reserve a unique <id>.trashinfo via O_CREAT | O_EXCL per spec.
    ByteString id;
    ByteString info_path;
    int info_fd = -1;
    for (unsigned suffix = 1; suffix < 10000; ++suffix) {
        id = suffix == 1 ? base_name : ByteString::formatted("{}.{}", base_name, suffix);
        info_path = ByteString::formatted("{}/{}.trashinfo", info_dir, id);
        info_fd = ::open(info_path.characters(), O_CREAT | O_EXCL | O_WRONLY, 0600);
        if (info_fd >= 0)
            break;
        if (errno != EEXIST)
            return Error::from_errno(errno);
    }
    if (info_fd < 0)
        return Error::from_errno(EEXIST);

    auto metadata = ByteString::formatted(
        "[Trash Info]\nPath={}\nDeletionDate={}\n",
        percent_encode_path(abs_path),
        current_iso8601_local());

    ssize_t written = ::write(info_fd, metadata.characters(), metadata.length());
    ::close(info_fd);
    if (written < 0 || static_cast<size_t>(written) != metadata.length()) {
        ::unlink(info_path.characters());
        return Error::from_errno(EIO);
    }

    auto dst_path = ByteString::formatted("{}/{}", files_dir, id);
    if (auto move_res = move_across_devices(abs_path, dst_path); move_res.is_error()) {
        ::unlink(info_path.characters());
        return move_res.release_error();
    }

    return id;
}

Vector<TrashEntry> list_trash()
{
    Vector<TrashEntry> entries;
    auto base_dir = trash_directory();
    auto files_dir = ByteString::formatted("{}/files", base_dir);
    auto info_dir = ByteString::formatted("{}/info", base_dir);

    DIR* dir = ::opendir(info_dir.characters());
    if (!dir)
        return entries;

    Vector<ByteString> info_files;
    while (auto* ent = ::readdir(dir)) {
        StringView name { ent->d_name, __builtin_strlen(ent->d_name) };
        if (name.ends_with(".trashinfo"sv))
            info_files.append(name.to_byte_string());
    }
    ::closedir(dir);
    quick_sort(info_files);

    for (auto const& fname : info_files) {
        auto id = fname.substring(0, fname.length() - 10);
        auto info_path = ByteString::formatted("{}/{}", info_dir, fname);
        auto file = Core::File::open(info_path, Core::File::OpenMode::Read);
        if (file.is_error())
            continue;
        auto contents = file.value()->read_until_eof();
        if (contents.is_error())
            continue;

        ByteString orig_path;
        ByteString deletion_date;
        StringView text { contents.value() };
        for (auto raw_line : text.lines()) {
            auto line = raw_line.trim_whitespace();
            if (line.starts_with("Path="sv))
                orig_path = percent_decode_path(line.substring_view(5));
            else if (line.starts_with("DeletionDate="sv))
                deletion_date = line.substring_view(13).to_byte_string();
        }
        if (orig_path.is_empty())
            continue;

        entries.append({
            .id = id,
            .original_path = move(orig_path),
            .trashed_file_path = ByteString::formatted("{}/{}", files_dir, id),
            .deletion_date = move(deletion_date),
        });
    }

    return entries;
}

ErrorOr<void> restore_trashed_file(StringView id)
{
    auto entries = list_trash();
    for (auto const& entry : entries) {
        if (entry.id != id)
            continue;
        auto parent = LexicalPath::dirname(entry.original_path);
        if (!parent.is_empty())
            TRY(Core::Directory::create(parent, Core::Directory::CreateDirectories::Yes));
        TRY(move_across_devices(entry.trashed_file_path, entry.original_path));
        auto info_path = ByteString::formatted("{}/info/{}.trashinfo", trash_directory(), id);
        (void)::unlink(info_path.characters());
        return {};
    }
    return Error::from_errno(ENOENT);
}

ErrorOr<void> empty_trash()
{
    auto base_dir = trash_directory();
    for (auto sub : { "files"sv, "info"sv }) {
        auto sub_path = ByteString::formatted("{}/{}", base_dir, sub);
        DIR* dir = ::opendir(sub_path.characters());
        if (!dir)
            continue;
        Vector<ByteString> names;
        while (auto* ent = ::readdir(dir)) {
            StringView name { ent->d_name, __builtin_strlen(ent->d_name) };
            if (name == "."sv || name == ".."sv)
                continue;
            names.append(name.to_byte_string());
        }
        ::closedir(dir);
        for (auto const& name : names)
            TRY(remove_recursive(ByteString::formatted("{}/{}", sub_path, name)));
    }
    return {};
}

}
