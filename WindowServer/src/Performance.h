/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Compositor.h"

#include <AK/Array.h>
#include <AK/QuickSort.h>

// --- R6: performance instrumentation -----------------------------------------
//
// N3's budget is "low, predictable input latency; smooth 60 Hz; a software
// compositor fallback". Two numbers decide it, and both are cheap to measure:
//
//   * frame time — how long the compositor takes to produce a frame: its own
//     work in output_frame() (scene build, render, commit). This is the cost we
//     own; the client's own drawing happens before its commit.
//   * input -> present — from the moment the compositor handles an input event
//     (notify_activity(), the same hook that resets the idle timer) to the frame
//     that presents its effect (the next output_frame()). This is what the user
//     feels, so it includes the client's response and the buffer hand-off.
//
// Each metric keeps a ring of the last `capacity` samples — fixed memory, so a
// long session cannot grow — plus totals. The measurement is always on (two
// clock reads per frame); the *reporting* is opt-in:
//   SDE_PERF_STATS=<file>  rewrite a key=value summary once a second
//   SDE_PERF_LOG=1         also log it, so a run can be watched live
struct PerfMetric {
    static constexpr size_t capacity = 600; // ~10 s at 60 Hz
    Array<double, capacity> samples { };
    size_t written { 0 };
    size_t total { 0 };
    double maximum { 0 };
    double budget_ms { 0 };
    size_t over_budget { 0 };

    void add(double value)
    {
        samples[written % capacity] = value;
        ++written;
        ++total;
        maximum = max(maximum, value);
        if (budget_ms > 0 && value > budget_ms)
            ++over_budget;
    }

    size_t recent() const { return min(written, capacity); }

    double percentile(double fraction) const
    {
        size_t const count = recent();
        if (count == 0)
            return 0;
        Array<double, capacity> sorted { };
        for (size_t i = 0; i < count; ++i)
            sorted[i] = samples[i];
        auto view = sorted.span().slice(0, count);
        quick_sort(view);
        auto index = static_cast<size_t>(fraction * static_cast<double>(count - 1) + 0.5);
        return sorted[min(index, count - 1)];
    }
};

struct PerfStats {
    PerfMetric frame_ms { .budget_ms = 16.6 };           // 60 Hz
    PerfMetric frame_interval_ms { .budget_ms = 50.0 };
    PerfMetric input_to_present_ms { .budget_ms = 50.0 };
    size_t frames { 0 };
    size_t slow_frames { 0 };
    double started_ms { 0 };
    int refresh_mhz { 0 };
    // Resident memory (kB): current, high-water, and the change since the last
    // report — the cheap shape of a leak.
    size_t rss_kb { 0 };
    size_t pss_kb { 0 };
    size_t rss_high_kb { 0 };
    i64 rss_delta_kb { 0 };
    size_t rss_previous_kb { 0 };
};

// A process's memory, in kB: resident set size (statm — cheap) and proportional
// set size (smaps_rollup). PSS is the honest per-client number: RSS counts a
// shared library's pages once per process, so ten clients each "use" the whole
// Lagom stack — the applets look like 21 MB each when they cost ~2 MB.
struct ProcessMemory {
    size_t rss_kb { 0 };
    size_t pss_kb { 0 };
};

extern PerfStats s_perf;
extern double s_perf_last_frame_ms;
extern double s_perf_pending_input_ms;

double perf_now_ms();
void perf_note_input();
double perf_frame_budget_ms(int refresh_mhz);
ProcessMemory perf_process_memory(pid_t pid);
ByteString perf_summary(Server& server);
