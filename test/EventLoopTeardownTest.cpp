/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
// A deferred callback with no event loop must not crash.
//
// `Core::EventLoop::current()` warned about an empty loop stack and then returned
// `event_loop_stack().last()` anyway — a reference into a vector with no elements.
// That is reachable during teardown, and a coredump from a session restart points
// straight at it:
//
//   EventLoop::current()                     ← SIGSEGV
//     ← deferred_invoke ← GUI::Window::schedule_relayout
//       ← GUI::Widget::child_event ← remove_child ← remove_from_parent
//         ← ~NonnullOwnPtr<GUI::Window> ← exit ← ConnectionToServer::die
//           ← EventLoop::exec
//
// A GUI application exiting because its compositor went away has already destroyed
// its EventLoop while widget trees are still being destroyed, and destroying a
// widget can defer work. Every client whose teardown defers therefore dumped core
// around a compositor restart.
//
// The fix returns an inert, leaked loop instead: work deferred during teardown is
// never run, which is what teardown wants. This test reproduces the *condition*
// exactly — a loop that exists, then stops existing, then a deferred invoke — and
// its assertion is its own exit status: dying by signal is the bug, returning 0 is
// correct. Driven by scripts/run-eventloop-teardown-test.sh.
#include <LibCore/EventLoop.h>
#include <LibCore/ThreadEventQueue.h>
#include <stdio.h>

int main()
{
    {
        // A loop that exists, as an application's does while it runs.
        Core::EventLoop loop;
        Core::EventLoop::current().deferred_invoke([] {});
    }

    // No loop now — the application has destroyed it and teardown is still running.
    // Before the fix this returned a reference into the empty stack.
    bool ran = false;
    Core::deferred_invoke([&] { ran = true; });

    // The callback must not have been run inline: teardown defers it precisely so it
    // can be dropped, not so it can execute against a half-destroyed widget tree.
    printf("eventloop-teardown-test: survived a deferred invoke with no loop (callback ran: %d)\n", ran ? 1 : 0);
    fflush(stdout);
    return ran ? 1 : 0;
}

// The same condition one step later, which is what the coredump actually showed:
// static destructors run *after* the thread-local ones, so by the time a static
// destructor tears down a widget tree, the stack the deferred work looks for has
// already been destroyed (not merely emptied). Fixing only the empty case left this
// one live, and every session restart dumped core again.
//
// Running at exit is the point: this whole object exists to be destroyed late.
struct DeferredWorkOnExit {
    ~DeferredWorkOnExit()
    {
        Core::deferred_invoke([] {});
    }
};
static DeferredWorkOnExit s_deferred_work_on_exit;
