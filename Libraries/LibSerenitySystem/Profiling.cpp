/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Profiling.h"
#include <AK/ByteString.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/StringUtils.h>
#include <LibCore/File.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/perf_event.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

namespace SerenitySystem::Profiling {

// One profiled process at a time (SerenityOS's profiling_enable/disable are
// process-global too). The reader thread appends perfcore events; disable()
// joins it before serialising, so the array has a single writer.
static int s_fd { -1 };
static perf_event_mmap_page* s_meta { nullptr };
static size_t s_ring_size { 0 };
static u64 s_data_tail { 0 };
static pthread_t s_reader { };
static bool s_running { false };
static pid_t s_pid { -1 };
static JsonArray s_events;

// Profiler timestamps are milliseconds: Profile::length_in_ms() is a raw
// difference (used directly as a histogram bucket count), so we must not hand
// it nanoseconds. The perf clock and CLOCK_MONOTONIC are both monotonic.
static u64 monotonic_ns()
{
    timespec ts { };
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<u64>(ts.tv_sec) * 1'000'000'000ull + static_cast<u64>(ts.tv_nsec);
}

static u64 now_ms()
{
    return monotonic_ns() / 1'000'000ull;
}

static int perf_event_open(perf_event_attr* attr, pid_t pid)
{
    return static_cast<int>(::syscall(__NR_perf_event_open, attr, pid, -1, -1, 0));
}

static ByteString read_exe(pid_t pid)
{
    char buffer[4096] = { 0 };
    auto link = ByteString::formatted("/proc/{}/exe", pid);
    auto length = ::readlink(link.characters(), buffer, sizeof(buffer) - 1);
    if (length <= 0)
        return {};
    return ByteString { buffer, static_cast<size_t>(length) };
}

static void append_event(JsonObject object)
{
    s_events.must_append(move(object));
}

static void emit_process_create(pid_t pid, pid_t tid, pid_t parent_pid)
{
    JsonObject event;
    event.set("type", ByteString { "process_create" });
    event.set("timestamp", now_ms());
    event.set("pid", pid);
    event.set("tid", tid);
    event.set("parent_pid", parent_pid);
    event.set("executable", read_exe(pid));
    append_event(move(event));
}

// /proc/<pid>/maps has one file mapping per line; report each file-backed
// mapping as an "mmap" event so Profiler can symbolicate sampled addresses.
static void snapshot_mappings(pid_t pid)
{
    auto file = Core::File::open(ByteString::formatted("/proc/{}/maps", pid), Core::File::OpenMode::Read);
    if (file.is_error())
        return;
    auto contents = file.value()->read_until_eof();
    if (contents.is_error())
        return;

    StringView text { reinterpret_cast<char const*>(contents.value().data()), contents.value().size() };
    for (auto line : text.split_view('\n')) {
        // start-end perms offset dev inode path
        auto range_end = line.find(' ');
        if (!range_end.has_value())
            continue;
        auto range = line.substring_view(0, *range_end);
        auto dash = range.find('-');
        if (!dash.has_value())
            continue;
        auto start = AK::StringUtils::convert_to_uint_from_hex<u64>(range.substring_view(0, *dash)).value_or(0);
        auto end = AK::StringUtils::convert_to_uint_from_hex<u64>(range.substring_view(*dash + 1)).value_or(0);

        // The path is the field after the inode (the 5th space-separated field).
        size_t cursor = 0;
        int field = 0;
        size_t path_offset = line.length();
        while (cursor < line.length()) {
            while (cursor < line.length() && line[cursor] == ' ')
                ++cursor;
            if (field == 5) {
                path_offset = cursor;
                break;
            }
            while (cursor < line.length() && line[cursor] != ' ')
                ++cursor;
            ++field;
        }
        if (path_offset >= line.length())
            continue;
        auto path = line.substring_view(path_offset);
        if (path.is_empty() || path[0] != '/')
            continue;

        JsonObject event;
        event.set("type", ByteString { "mmap" });
        event.set("timestamp", now_ms());
        event.set("pid", pid);
        event.set("tid", pid);
        event.set("ptr", start);
        event.set("size", end > start ? end - start : 0);
        event.set("name", path.to_byte_string());
        append_event(move(event));
    }
}

// Read `size` bytes at ring offset `offset`, handling wrap-around.
static void read_ring(void* destination, size_t size, u64 offset)
{
    auto* data = reinterpret_cast<u8*>(s_meta) + getpagesize();
    size_t const begin = offset % s_ring_size;
    if (begin + size <= s_ring_size) {
        memcpy(destination, data + begin, size);
    } else {
        size_t const first = s_ring_size - begin;
        memcpy(destination, data + begin, first);
        memcpy(static_cast<u8*>(destination) + first, data, size - first);
    }
}

static void decode_record(u8 const* record)
{
    auto* header = reinterpret_cast<perf_event_header const*>(record);
    u64 timestamp = now_ms();

    switch (header->type) {
    case PERF_RECORD_SAMPLE: {
        // sample_type = IP | TID | TIME | CALLCHAIN, in that order.
        size_t cursor = sizeof(perf_event_header);
        u64 ip = 0;
        u32 pid = 0, tid = 0;
        memcpy(&ip, record + cursor, sizeof(ip));
        cursor += sizeof(ip);
        memcpy(&pid, record + cursor, sizeof(pid));
        cursor += sizeof(pid);
        memcpy(&tid, record + cursor, sizeof(tid));
        cursor += sizeof(tid);
        memcpy(&timestamp, record + cursor, sizeof(timestamp));
        cursor += sizeof(timestamp);
        // PERF_SAMPLE_TIME is nanoseconds; Profiler works in milliseconds.
        timestamp /= 1'000'000ull;

        u64 nr = 0;
        memcpy(&nr, record + cursor, sizeof(nr));
        cursor += sizeof(nr);

        JsonArray stack;
        for (u64 i = 0; i < nr && cursor + sizeof(u64) <= header->size; ++i) {
            u64 frame = 0;
            memcpy(&frame, record + cursor, sizeof(frame));
            cursor += sizeof(frame);
            if (frame != 0)
                stack.must_append(frame);
        }
        // Profiler needs at least the sampled ip plus one caller frame.
        if (stack.size() == 0)
            break;

        JsonObject event;
        event.set("type", ByteString { "sample" });
        event.set("timestamp", timestamp);
        event.set("pid", pid);
        event.set("tid", tid);
        event.set("stack", move(stack));
        append_event(move(event));
        break;
    }
    case PERF_RECORD_MMAP:
    case PERF_RECORD_MMAP2: {
        u32 pid = 0, tid = 0;
        u64 address = 0, length = 0;
        memcpy(&pid, record + 8, sizeof(pid));
        memcpy(&tid, record + 12, sizeof(tid));
        memcpy(&address, record + 16, sizeof(address));
        memcpy(&length, record + 24, sizeof(length));
        size_t const filename_offset = header->type == PERF_RECORD_MMAP2 ? 72 : 40;
        if (header->size <= filename_offset)
            break;
        ByteString name { reinterpret_cast<char const*>(record + filename_offset), header->size - filename_offset - 1 };
        if (name.is_empty() || name[0] != '/')
            break;

        JsonObject event;
        event.set("type", ByteString { "mmap" });
        event.set("timestamp", timestamp);
        event.set("pid", pid);
        event.set("tid", tid);
        event.set("ptr", address);
        event.set("size", length);
        event.set("name", name);
        append_event(move(event));
        break;
    }
    case PERF_RECORD_EXIT: {
        u32 pid = 0, ppid = 0, tid = 0, ptid = 0;
        memcpy(&pid, record + 8, sizeof(pid));
        memcpy(&ppid, record + 12, sizeof(ppid));
        memcpy(&tid, record + 16, sizeof(tid));
        memcpy(&ptid, record + 20, sizeof(ptid));
        JsonObject event;
        event.set("type", ByteString { tid == pid ? "process_exit" : "thread_exit" });
        event.set("timestamp", timestamp);
        event.set("pid", pid);
        event.set("tid", tid);
        append_event(move(event));
        break;
    }
    case PERF_RECORD_FORK: {
        u32 pid = 0, ppid = 0, tid = 0, ptid = 0;
        memcpy(&pid, record + 8, sizeof(pid));
        memcpy(&ppid, record + 12, sizeof(ppid));
        memcpy(&tid, record + 16, sizeof(tid));
        memcpy(&ptid, record + 20, sizeof(ptid));
        if (tid == pid) {
            emit_process_create(pid, tid, ppid);
        } else {
            JsonObject event;
            event.set("type", ByteString { "thread_create" });
            event.set("timestamp", timestamp);
            event.set("pid", pid);
            event.set("tid", tid);
            event.set("parent_tid", ptid);
            append_event(move(event));
        }
        break;
    }
    default:
        break;
    }
}

static void* reader_thread(void*)
{
    auto* data = reinterpret_cast<u8*>(s_meta) + getpagesize();
    (void)data;
    u8 buffer[4096];
    while (s_running) {
        pollfd pfd { s_fd, POLLIN, 0 };
        poll(&pfd, 1, 100);

        u64 head = __atomic_load_n(&s_meta->data_head, __ATOMIC_ACQUIRE);
        while (s_data_tail < head) {
            perf_event_header header { };
            read_ring(&header, sizeof(header), s_data_tail);
            if (header.size < sizeof(perf_event_header) || header.size > sizeof(buffer))
                break;
            read_ring(buffer, header.size, s_data_tail);
            decode_record(buffer);
            s_data_tail += header.size;
        }
        __atomic_store_n(&s_meta->data_tail, s_data_tail, __ATOMIC_RELEASE);
    }
    return nullptr;
}

ErrorOr<void> enable(pid_t pid, u64)
{
    if (s_fd >= 0)
        return Error::from_string_literal("profiling is already enabled");

    perf_event_attr attr { };
    attr.size = sizeof(attr);
    attr.type = PERF_TYPE_SOFTWARE;
    attr.config = PERF_COUNT_SW_CPU_CLOCK;
    attr.sample_period = 1'000'000; // 1 ms
    attr.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_CALLCHAIN;
    attr.disabled = 1;
    attr.exclude_kernel = 1;
    attr.exclude_hv = 1;
    attr.exclude_callchain_kernel = 1;
    attr.mmap = 1;
    attr.mmap2 = 1;
    attr.comm = 1;
    attr.task = 1;
    attr.wakeup_events = 1;

    s_fd = perf_event_open(&attr, pid);
    if (s_fd < 0)
        return Error::from_errno(errno);

    s_pid = pid;
    s_events.clear();

    auto page_size = static_cast<size_t>(getpagesize());
    s_ring_size = page_size * 64;
    auto* meta = static_cast<perf_event_mmap_page*>(mmap(nullptr, s_ring_size + page_size, PROT_READ | PROT_WRITE, MAP_SHARED, s_fd, 0));
    if (meta == MAP_FAILED) {
        auto saved = errno;
        ::close(s_fd);
        s_fd = -1;
        return Error::from_errno(saved);
    }
    s_meta = meta;
    s_data_tail = 0;

    // The current mappings are not reported as mmap records (they predate
    // enable), so seed them, then report the process itself.
    snapshot_mappings(pid);
    emit_process_create(pid, pid, 0);

    ioctl(s_fd, PERF_EVENT_IOC_RESET, 0);
    ioctl(s_fd, PERF_EVENT_IOC_ENABLE, 0);

    s_running = true;
    if (pthread_create(&s_reader, nullptr, reader_thread, nullptr) != 0)
        return Error::from_errno(errno);
    return {};
}

ErrorOr<void> disable(pid_t pid)
{
    if (s_fd < 0)
        return Error::from_string_literal("profiling is not enabled");

    ioctl(s_fd, PERF_EVENT_IOC_DISABLE, 0);
    s_running = false;
    pthread_join(s_reader, nullptr);

    munmap(s_meta, s_ring_size + getpagesize());
    ::close(s_fd);
    s_fd = -1;
    s_meta = nullptr;

    JsonObject root;
    JsonArray strings;
    root.set("strings", move(strings));
    root.set("events", move(s_events));
    s_events = JsonArray {};

    auto path = perfcore_path(pid);
    auto file = TRY(Core::File::open(path, Core::File::OpenMode::Write));
    TRY(file->write_until_depleted(root.to_byte_string().to_byte_buffer()));
    return {};
}

ByteString perfcore_path(pid_t pid)
{
    if (auto const* runtime = getenv("XDG_RUNTIME_DIR"); runtime && *runtime)
        return ByteString::formatted("{}/sde-perfcore-{}.json", runtime, pid);
    return ByteString::formatted("/tmp/sde-perfcore-{}.json", pid);
}

}
