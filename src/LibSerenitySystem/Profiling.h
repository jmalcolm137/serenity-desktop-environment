#pragma once

#include <AK/Error.h>
#include <AK/String.h>
#include <sys/types.h>

// Serenity Desktop Environment: a host implementation of SerenityOS's kernel
// profiling interface. On SerenityOS, `profiling_enable(pid, mask)` makes the
// kernel sample the process and publish the events at /proc/<pid>/perf_events,
// which the Profiler app reads (a JSON "perfcore" object). Linux has no such
// kernel interface, so this samples with perf_event_open() and writes the same
// perfcore JSON that Profiler consumes.
namespace SerenitySystem::Profiling {

// Start sampling `pid` (a software CPU-clock counter, with ip/tid/time/callchain
// samples and mmap/comm/fork/exit records) and snapshot its current mappings.
// `event_mask` mirrors SerenityOS's PERF_EVENT_* mask; the native sampler emits
// the events it can support.
ErrorOr<void> enable(pid_t pid, u64 event_mask);

// Stop sampling and write the collected events as perfcore JSON.
ErrorOr<void> disable(pid_t pid);

// The perfcore JSON path written by disable().
ByteString perfcore_path(pid_t pid);

}
