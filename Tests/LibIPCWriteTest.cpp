/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Regression: MessageBuffer::transfer_message() retried a would-block write by
// pumping the current Core::EventLoop, up to 100 times, and logged a warning
// when a large message needed more than one write.
//
// Pumping re-enters arbitrary application callbacks from inside a send, which
// can destroy the state the sender is using. A large message legitimately needs
// several writes, so the warning was not actionable either.
//
// This test fills a small socket send buffer with a 1 MiB message while the
// receiving side is not reading yet. It records whether the event loop ran
// during the send: the fixed implementation waits for the socket to become
// writable without pumping, so the timer below must not fire until the send has
// returned. The receiver then drains the message from another thread and checks
// that every byte (including the size header) arrived.
//
// The driver is scripts/run-libipc-write-test.sh.
#include <AK/Vector.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Socket.h>
#include <LibCore/Timer.h>
#include <LibIPC/Message.h>
#include <LibMain/Main.h>
#include <LibThreading/Thread.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

constexpr size_t payload_size = 1024 * 1024;
constexpr size_t receive_chunk_size = 16 * 1024;

int g_failures = 0;

void fail(char const* what)
{
    if (g_failures < 20)
        warnln("FAIL: {}", what);
    ++g_failures;
}

}

ErrorOr<int> serenity_main(Main::Arguments)
{
    Core::EventLoop event_loop;

    int fds[2] = { -1, -1 };
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0) {
        warnln("FAIL: socketpair: {}", strerror(errno));
        return 1;
    }

    auto sender = TRY(Core::LocalSocket::adopt_fd(fds[0]));
    auto receiver = TRY(Core::LocalSocket::adopt_fd(fds[1]));

    TRY(sender->set_blocking(false));
    TRY(receiver->set_blocking(true));

    // A small send buffer makes the first write return a short count and the
    // next one return EAGAIN, which is the path under test.
    int send_buffer_size = 4096;
    (void)::setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &send_buffer_size, sizeof(send_buffer_size));

    Vector<u8> payload;
    payload.ensure_capacity(payload_size);
    for (size_t i = 0; i < payload_size; ++i)
        payload.append(static_cast<u8>((i * 31) + 7));

    IPC::MessageBuffer buffer;
    TRY(buffer.append_data(payload.data(), payload.size()));
    auto expected_size = sizeof(u32) + payload.size();

    Vector<u8> received;
    received.ensure_capacity(expected_size);
    ByteString reader_error;
    auto receiver_thread = Threading::Thread::construct([&]() -> intptr_t {
        // Let the sender fill the socket buffer and block first.
        usleep(100 * 1000);

        while (received.size() < expected_size) {
            u8 chunk[receive_chunk_size];
            auto maybe_nread = receiver->read_some(Bytes { chunk, sizeof(chunk) });
            if (maybe_nread.is_error()) {
                reader_error = maybe_nread.error().string_literal();
                return 1;
            }
            if (maybe_nread.value().is_empty())
                break;
            received.append(maybe_nread.value().data(), maybe_nread.value().size());
        }
        return 0;
    });
    receiver_thread->start();

    // If transfer_message() pumps the event loop, this timer runs before the
    // message has been sent.
    bool event_loop_ran_during_transfer = false;
    auto reentry_timer = Core::Timer::create_repeating(1, [&] {
        event_loop_ran_during_transfer = true;
    });
    reentry_timer->start();

    auto transfer_result = buffer.transfer_message(*sender);
    reentry_timer->stop();

    if (transfer_result.is_error()) {
        warnln("FAIL: transfer_message: {}", transfer_result.error());
        ++g_failures;
        // Unblock the receiver thread's read.
        sender->close();
    }

    (void)receiver_thread->join();

    if (!reader_error.is_empty())
        warnln("FAIL: receiver read failed: {}", reader_error);

    if (event_loop_ran_during_transfer)
        fail("the event loop ran during transfer_message()");

    if (received.size() != expected_size) {
        warnln("FAIL: received {} bytes, expected {}", received.size(), expected_size);
        ++g_failures;
    } else {
        u32 message_size = 0;
        memcpy(&message_size, received.data(), sizeof(message_size));
        if (message_size != payload.size()) {
            warnln("FAIL: size header is {} bytes, expected {}", message_size, payload.size());
            ++g_failures;
        }
        if (memcmp(received.data() + sizeof(message_size), payload.data(), payload.size()) != 0)
            fail("the received payload does not match the message");
    }

    if (g_failures != 0) {
        warnln("{} check(s) failed", g_failures);
        return 1;
    }
    outln("ok: large IPC message transferred without re-entering the event loop");
    return 0;
}
