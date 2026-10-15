/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Compositor.h"
#include "Performance.h"

#include <AK/QuickSort.h>
#include <AK/StringBuilder.h>
#include <LibCore/Directory.h>
#include <LibCore/File.h>

#include <ctime>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "Wlroots.h"

PerfStats s_perf;

double s_perf_last_frame_ms { 0 };

double s_perf_pending_input_ms { -1 };

double perf_now_ms()
{
    timespec now { };
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<double>(now.tv_sec) * 1000.0 + static_cast<double>(now.tv_nsec) / 1000000.0;
}

// Note that input arrived: the next frame is the one that presents it. The most
// recent event wins, because that is what the frame ends up showing.
void perf_note_input()
{
    s_perf_pending_input_ms = perf_now_ms();
}

// The frame budget is one refresh interval of the output being rendered on: a
// 60 Hz panel is held to 16.667 ms, a 144 Hz one to 6.944 ms, a 240 Hz one to
// 4.167 ms. SDE_PERF_BUDGET_HZ overrides the reference rate, for a harness (a
// headless output reports no refresh) or to hold a session to a display it is
// not on.
double perf_frame_budget_ms(int refresh_mhz)
{
    if (auto const* forced = getenv("SDE_PERF_BUDGET_HZ"); forced && *forced) {
        double const hz = atof(forced);
        if (hz > 0)
            return 1000.0 / hz;
    }
    if (refresh_mhz > 0)
        return 1000000.0 / static_cast<double>(refresh_mhz);
    return 16.667; // 60 Hz, when the output does not report a refresh
}

ProcessMemory perf_process_memory(pid_t pid)
{
    ProcessMemory memory;

    auto statm = Core::File::open(ByteString::formatted("/proc/{}/statm", pid), Core::File::OpenMode::Read);
    if (!statm.is_error()) {
        if (auto contents = statm.value()->read_until_eof(); !contents.is_error()) {
            auto bytes = contents.release_value();
            StringView text { reinterpret_cast<char const*>(bytes.data()), bytes.size() };
            auto fields = text.split_view(' ');
            if (fields.size() >= 2) {
                auto const page_kb = static_cast<size_t>(sysconf(_SC_PAGESIZE)) / 1024;
                memory.rss_kb = fields[1].to_number<size_t>().value_or(0) * page_kb;
            }
        }
    }

    auto rollup = Core::File::open(ByteString::formatted("/proc/{}/smaps_rollup", pid), Core::File::OpenMode::Read);
    if (!rollup.is_error()) {
        if (auto contents = rollup.value()->read_until_eof(); !contents.is_error()) {
            auto bytes = contents.release_value();
            StringView text { reinterpret_cast<char const*>(bytes.data()), bytes.size() };
            for (auto line : text.split_view('\n')) {
                if (!line.starts_with("Pss:"sv))
                    continue;
                auto value = line.substring_view(4).trim_whitespace();
                if (auto space = value.find(' '); space.has_value())
                    value = value.substring_view(0, space.value());
                memory.pss_kb = value.to_number<size_t>().value_or(0);
                break;
            }
        }
    }

    // A kernel without smaps_rollup (or a process we may not read): fall back.
    if (memory.pss_kb == 0)
        memory.pss_kb = memory.rss_kb;
    return memory;
}

// One entry per session process, biggest first: the "who is using memory" list.
struct ClientMemory {
    ByteString label;
    pid_t pid { 0 };
    size_t rss_kb { 0 };
    size_t pss_kb { 0 };
};

static Vector<ClientMemory> perf_client_memory(Server& server)
{
    Vector<ClientMemory> clients;
    if (!server.display)
        return clients;

    wl_client* client = nullptr;
    wl_client_for_each(client, wl_display_get_client_list(server.display)) {
        pid_t pid = 0;
        wl_client_get_credentials(client, &pid, nullptr, nullptr);
        if (pid <= 0)
            continue;

        ClientMemory entry;
        entry.pid = pid;
        entry.label = ByteString::formatted("pid{}", pid);
        // The executable name is the label the user would use ("Taskbar",
        // "Audio"): the shell's clients are panels and applets — layer surfaces
        // with no toplevel to name them by. Take it from argv[0] rather than
        // /proc/<pid>/comm, which the kernel truncates to 15 characters.
        if (auto cmdline = Core::File::open(ByteString::formatted("/proc/{}/cmdline", pid), Core::File::OpenMode::Read); !cmdline.is_error()) {
            if (auto contents = cmdline.value()->read_until_eof(); !contents.is_error()) {
                auto bytes = contents.release_value();
                size_t length = 0;
                while (length < bytes.size() && bytes[length] != '\0')
                    ++length;
                StringView argv0 { reinterpret_cast<char const*>(bytes.data()), length };
                if (auto slash = argv0.find_last('/'); slash.has_value())
                    argv0 = argv0.substring_view(slash.value() + 1);
                if (!argv0.is_empty())
                    entry.label = argv0.to_byte_string();
            }
        }
        // A windowed client is better named by its app id (or title).
        for (auto& toplevel : server.toplevels) {
            if (!toplevel->xdg_toplevel || !toplevel->xdg_toplevel->base || !toplevel->xdg_toplevel->base->surface)
                continue;
            if (wl_resource_get_client(toplevel->xdg_toplevel->base->surface->resource) != client)
                continue;
            if (auto const* app_id = toplevel->xdg_toplevel->app_id; app_id && *app_id != '\0')
                entry.label = app_id;
            else if (auto const* title = toplevel->xdg_toplevel->title; title && *title != '\0')
                entry.label = title;
            break;
        }

        auto const memory = perf_process_memory(pid);
        entry.rss_kb = memory.rss_kb;
        entry.pss_kb = memory.pss_kb;
        clients.append(move(entry));
    }

    // Second: the rest of the session's processes — the services LibWM spawns on
    // demand (the AudioServer, the NotificationServer) and the shell's launcher.
    // They share the compositor's process group (the session supervisor runs it
    // with setsid) but never connect as clients, so the client list above misses
    // them. A service's own children are their own business; this is one level.
    auto const group = getpgid(0);
    if (group > 0) {
        (void)Core::Directory::for_each_entry("/proc"sv, Core::DirIterator::SkipDots, [&](Core::DirectoryEntry const& entry, Core::Directory const&) -> ErrorOr<AK::IterationDecision> {
            auto pid = entry.name.to_number<pid_t>().value_or(0);
            if (pid <= 0 || pid == getpid())
                return AK::IterationDecision::Continue;
            bool already_listed = false;
            for (auto const& client : clients) {
                if (client.pid == pid) {
                    already_listed = true;
                    break;
                }
            }
            if (already_listed)
                return AK::IterationDecision::Continue;
            auto stat = Core::File::open(ByteString::formatted("/proc/{}/stat", pid), Core::File::OpenMode::Read);
            if (stat.is_error())
                return AK::IterationDecision::Continue;
            auto contents = stat.value()->read_until_eof();
            if (contents.is_error())
                return AK::IterationDecision::Continue;
            auto bytes = contents.release_value();
            StringView text { reinterpret_cast<char const*>(bytes.data()), bytes.size() };
            // "<pid> (<comm>) <state> <ppid> <pgrp> ...": comm may contain spaces,
            // so skip past the last ')'.
            auto close_paren = text.find_last(')');
            if (!close_paren.has_value())
                return AK::IterationDecision::Continue;
            auto fields = text.substring_view(close_paren.value() + 1).trim_whitespace().split_view(' ');
            if (fields.size() < 3 || fields[2].to_number<pid_t>().value_or(0) != group)
                return AK::IterationDecision::Continue;

            ClientMemory service;
            service.pid = pid;
            service.label = ByteString::formatted("pid{}", pid);
            if (auto cmdline = Core::File::open(ByteString::formatted("/proc/{}/cmdline", pid), Core::File::OpenMode::Read); !cmdline.is_error()) {
                if (auto cmd = cmdline.value()->read_until_eof(); !cmd.is_error()) {
                    auto cmd_bytes = cmd.release_value();
                    size_t length = 0;
                    while (length < cmd_bytes.size() && cmd_bytes[length] != '\0')
                        ++length;
                    StringView argv0 { reinterpret_cast<char const*>(cmd_bytes.data()), length };
                    if (auto slash = argv0.find_last('/'); slash.has_value())
                        argv0 = argv0.substring_view(slash.value() + 1);
                    if (!argv0.is_empty())
                        service.label = argv0.to_byte_string();
                }
            }
            auto const memory = perf_process_memory(pid);
            service.rss_kb = memory.rss_kb;
            service.pss_kb = memory.pss_kb;
            clients.append(move(service));
            return AK::IterationDecision::Continue;
        });
    }

    quick_sort(clients, [](ClientMemory const& a, ClientMemory const& b) { return a.pss_kb > b.pss_kb; });
    return clients;
}

ByteString perf_summary(Server& server)
{
    double const seconds = max(0.001, (perf_now_ms() - s_perf.started_ms) / 1000.0);
    StringBuilder out;
    out.appendff("frames={}\n", s_perf.frames);
    out.appendff("slow_frames={}\n", s_perf.slow_frames);
    out.appendff("fps={:.1}\n", static_cast<double>(s_perf.frames) / seconds);
    out.appendff("samples={}\n", s_perf.frame_ms.recent());
    out.appendff("output_refresh_mhz={}\n", s_perf.refresh_mhz);
    out.appendff("output_refresh_hz={:.1}\n", static_cast<double>(s_perf.refresh_mhz) / 1000.0);
    out.appendff("frame_ms_budget={:.3}\n", s_perf.frame_ms.budget_ms);
    out.appendff("frame_ms_p50={:.3}\n", s_perf.frame_ms.percentile(0.5));
    out.appendff("frame_ms_p95={:.3}\n", s_perf.frame_ms.percentile(0.95));
    out.appendff("frame_ms_p99={:.3}\n", s_perf.frame_ms.percentile(0.99));
    out.appendff("frame_ms_max={:.3}\n", s_perf.frame_ms.maximum);
    out.appendff("frame_ms_over_budget={}\n", s_perf.frame_ms.over_budget);
    out.appendff("frame_interval_ms_p50={:.3}\n", s_perf.frame_interval_ms.percentile(0.5));
    out.appendff("frame_interval_ms_p95={:.3}\n", s_perf.frame_interval_ms.percentile(0.95));
    out.appendff("frame_interval_ms_p99={:.3}\n", s_perf.frame_interval_ms.percentile(0.99));
    out.appendff("input_to_present_ms_budget={:.1}\n", s_perf.input_to_present_ms.budget_ms);
    out.appendff("input_to_present_ms_p50={:.3}\n", s_perf.input_to_present_ms.percentile(0.5));
    out.appendff("input_to_present_ms_p95={:.3}\n", s_perf.input_to_present_ms.percentile(0.95));
    out.appendff("input_to_present_ms_p99={:.3}\n", s_perf.input_to_present_ms.percentile(0.99));
    out.appendff("input_to_present_ms_max={:.3}\n", s_perf.input_to_present_ms.maximum);
    out.appendff("input_to_present_ms_over_budget={}\n", s_perf.input_to_present_ms.over_budget);
    out.appendff("input_to_present_samples={}\n", s_perf.input_to_present_ms.total);
    out.appendff("rss_kb={}\n", s_perf.rss_kb);
    out.appendff("pss_kb={}\n", s_perf.pss_kb);
    out.appendff("rss_high_kb={}\n", s_perf.rss_high_kb);
    out.appendff("rss_delta_kb={}\n", s_perf.rss_delta_kb);

    // The session: the compositor plus every client (the Taskbar, the applets,
    // the apps). PSS is what "who uses memory" needs; RSS is kept for continuity.
    auto const clients = perf_client_memory(server);
    size_t client_rss_kb = 0;
    size_t client_pss_kb = 0;
    StringBuilder breakdown;
    for (size_t i = 0; i < clients.size(); ++i) {
        client_rss_kb += clients[i].rss_kb;
        client_pss_kb += clients[i].pss_kb;
        if (i >= 12)
            continue;
        if (i > 0)
            breakdown.append(',');
        breakdown.appendff("{}:{}", clients[i].label, clients[i].pss_kb);
    }
    out.appendff("clients={}\n", clients.size());
    out.appendff("client_rss_kb={}\n", client_rss_kb);
    out.appendff("client_pss_kb={}\n", client_pss_kb);
    out.appendff("client_memory={}\n", breakdown.to_byte_string());
    out.appendff("session_pss_kb={}\n", s_perf.pss_kb + client_pss_kb);
    out.appendff("session_rss_kb={}\n", s_perf.rss_kb + client_rss_kb);
    return out.to_byte_string();
}
