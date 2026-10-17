/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Capture.h"
#include "Wlroots.h"
#include <LibGfx/Bitmap.h>
#include <drm_fourcc.h>
#include "serenity-window-server-protocol.h"
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>

#include <chrono>

#include <sys/mman.h>
#include <unistd.h>

namespace {

struct PendingCapture {
    wl_resource* manager;
    uint32_t serial;
    Gfx::IntRect region;
};

// One compositor per process, and a magnifier keeps one request outstanding.
Vector<PendingCapture> s_pending;

Gfx::BitmapFormat bitmap_format_for(uint32_t read_format)
{
    if (read_format == DRM_FORMAT_ARGB8888)
        return Gfx::BitmapFormat::BGRA8888;
    if (read_format == DRM_FORMAT_XRGB8888)
        return Gfx::BitmapFormat::BGRx8888;
    return Gfx::BitmapFormat::Invalid;
}

}

int bitmap_fd(RefPtr<Gfx::Bitmap> const& bitmap)
{
    if (!bitmap)
        return -1;
    auto name = ByteString::formatted("sde-bitmap-{}", getpid());
    int fd = memfd_create(name.characters(), 0);
    if (fd < 0)
        return -1;
    auto size = static_cast<size_t>(bitmap->pitch()) * bitmap->height();
    if (ftruncate(fd, static_cast<off_t>(size)) != 0) {
        ::close(fd);
        return -1;
    }
    auto* mapped = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapped == MAP_FAILED) {
        ::close(fd);
        return -1;
    }
    memcpy(mapped, bitmap->scanline(0), size);
    munmap(mapped, size);
    return fd;
}

void queue_capture_request(wl_resource* manager, uint32_t serial, Gfx::IntRect const& region)
{
    // Only the newest request per manager matters: the magnifier asks again every
    // frame, and answering a stale serial would only add latency.
    s_pending.remove_all_matching([&](auto& pending) { return pending.manager == manager; });
    s_pending.append({ manager, serial, region });
}

void forget_capture_requests(wl_resource* manager)
{
    s_pending.remove_all_matching([&](auto& pending) { return pending.manager == manager; });
}

void serve_pending_captures(Server& server, wlr_buffer* committed)
{
    if (s_pending.is_empty())
        return;

    auto* texture = committed ? wlr_texture_from_buffer(server.renderer, committed) : nullptr;
    auto const read_format = texture ? wlr_texture_preferred_read_format(texture) : 0;
    auto const bitmap_format = bitmap_format_for(read_format);

    for (auto const& pending : s_pending) {
        RefPtr<Gfx::Bitmap> bitmap;
        if (texture && bitmap_format != Gfx::BitmapFormat::Invalid) {
            auto const region = pending.region.intersected({ 0, 0, committed->width, committed->height });
            if (!region.is_empty()) {
                if (auto created = Gfx::Bitmap::create(bitmap_format, region.size()); !created.is_error()) {
                    // src_box is const: the whole options object is aggregate-initialised.
                    struct wlr_texture_read_pixels_options read = {
                        .data = created.value()->scanline(0),
                        .format = read_format,
                        .stride = static_cast<uint32_t>(created.value()->pitch()),
                        .dst_x = 0,
                        .dst_y = 0,
                        .src_box = { .x = region.x(), .y = region.y(), .width = region.width(), .height = region.height() },
                    };
                    if (wlr_texture_read_pixels(texture, &read))
                        bitmap = created.release_value();
                }
            }
        }
        int fd = bitmap_fd(bitmap);
        if (fd < 0) {
            serenity_window_manager_send_captured(pending.manager, pending.serial, 0, 0, 0, 0, -1);
            continue;
        }
        serenity_window_manager_send_captured(pending.manager, pending.serial, bitmap->width(), bitmap->height(),
            bitmap->pitch(), static_cast<uint32_t>(bitmap->format()), fd);
        ::close(fd);
        // At most once a second: a magnifier asks every frame, and the log line is
        // for diagnostics, not a per-frame trace.
        static auto last_report = std::chrono::steady_clock::now() - std::chrono::seconds(1);
        auto const now = std::chrono::steady_clock::now();
        if (now - last_report >= std::chrono::seconds(1)) {
            last_report = now;
            dbgln("WindowServer: capture served {}x{} at {},{}", bitmap->width(), bitmap->height(),
                pending.region.x(), pending.region.y());
        }
    }

    if (texture)
        wlr_texture_destroy(texture);
    s_pending.clear();
}
