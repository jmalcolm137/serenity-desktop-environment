/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Server.h"
#include <AK/RefPtr.h>
#include <AK/Vector.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Rect.h>

struct wl_resource;
struct wlr_buffer;

// R5 accessibility (docs/29-accessibility.md): the Magnifier's screen capture.
//
// The Magnifier asks through get_screen_bitmap_around_cursor/_around_location; the
// shim turns that into request_capture on serenity-window and expects `captured`
// back. A request is answered from the next frame the output actually commits:
// the compositor renders the scene every frame, and that frame's buffer is what the
// user sees. Capturing from it means the magnifier shows exactly what is on screen,
// needs no swapchain of our own (wlroots does not export one), leaves the visible
// frame untouched, and costs one refresh interval of latency -- invisible in a
// magnifier, and the same push shape as the shim's other answers.
//
// A bitmap as a memfd the client can map (fd = -1 when there is none).
int bitmap_fd(RefPtr<Gfx::Bitmap> const&);

// Queue a request; the answer is pushed on the next commit.
void queue_capture_request(wl_resource* manager, uint32_t serial, Gfx::IntRect const& region);

// A manager resource is going away: drop its pending requests, so the pointer is
// never used after the resource is freed.
void forget_capture_requests(wl_resource* manager);

// Answer every pending request from this frame's buffer.
void serve_pending_captures(Server&, wlr_buffer* committed);
