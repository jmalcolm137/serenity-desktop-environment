#pragma once

#include <AK/Error.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <sys/types.h>

// Serenity Desktop Environment: host implementations of SerenityOS *system*
// interfaces whose behaviour differs on Linux. This is deliberately separate
// from the libgui-wayland compatibility layer (which stays a portable host
// build of the Serenity userland): these are sessions/system services that the
// SDE owns.
namespace SerenitySystem {

// Best-effort equivalent of SerenityOS's utmp update. Linux maintains a
// different utmp database and updating it requires privileges, so for now this
// is a no-fail bookkeeping hook. A full session service will replace it.
ErrorOr<void> update_utmp(StringView tty, pid_t pid, bool create);

// Direct children of `parent`, equivalent to the contents of SerenityOS's
// /proc/<parent>/children, implemented over the Linux /proc filesystem.
ErrorOr<Vector<pid_t>> child_processes(pid_t parent);

}
