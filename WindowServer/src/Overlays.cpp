/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Overlays.h"

#include <AK/ScopeGuard.h>
#include <LibGfx/Painter.h>
#include <drm_fourcc.h>
#include <string.h>

// wlroots is a C library and does not wrap its headers in extern "C", so we do.
extern "C" {
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_scene.h>
}

// --- bitmap buffers ---------------------------------------------------------

struct BitmapBuffer {
    wlr_buffer base { };
    NonnullRefPtr<Gfx::Bitmap> bitmap;

    explicit BitmapBuffer(NonnullRefPtr<Gfx::Bitmap> bitmap_)
        : bitmap(move(bitmap_))
    {
    }
};

static void bitmap_buffer_destroy(wlr_buffer* buffer)
{
    auto* self = reinterpret_cast<BitmapBuffer*>(buffer);
    wlr_buffer_finish(buffer);
    delete self;
}

static bool bitmap_buffer_get_dmabuf(wlr_buffer*, wlr_dmabuf_attributes*) { return false; }
static bool bitmap_buffer_get_shm(wlr_buffer*, wlr_shm_attributes*) { return false; }

static bool bitmap_buffer_begin_data_ptr_access(wlr_buffer* buffer, uint32_t, void** data, uint32_t* format, size_t* stride)
{
    auto* self = reinterpret_cast<BitmapBuffer*>(buffer);
    *data = self->bitmap->scanline_u8(0);
    *format = DRM_FORMAT_ARGB8888;
    *stride = self->bitmap->pitch();
    return true;
}

static void bitmap_buffer_end_data_ptr_access(wlr_buffer*) { }

static constexpr struct wlr_buffer_impl BITMAP_BUFFER_IMPL = {
    .destroy = bitmap_buffer_destroy,
    .get_dmabuf = bitmap_buffer_get_dmabuf,
    .get_shm = bitmap_buffer_get_shm,
    .begin_data_ptr_access = bitmap_buffer_begin_data_ptr_access,
    .end_data_ptr_access = bitmap_buffer_end_data_ptr_access,
};

wlr_buffer* bitmap_buffer_create(NonnullRefPtr<Gfx::Bitmap> bitmap)
{
    auto* buffer = new BitmapBuffer(move(bitmap));
    wlr_buffer_init(&buffer->base, &BITMAP_BUFFER_IMPL, buffer->bitmap->width(), buffer->bitmap->height());
    return &buffer->base;
}

// --- chrome nodes -----------------------------------------------------------

ChromeNode::ChromeNode(wlr_scene_tree& parent)
{
    m_buffer = wlr_scene_buffer_create(&parent, nullptr);
}

ChromeNode::~ChromeNode()
{
    if (m_buffer)
        wlr_scene_node_destroy(&m_buffer->node);
}

void ChromeNode::set_rect(Gfx::IntRect rect)
{
    m_rect = rect;
    wlr_scene_node_set_position(&m_buffer->node, rect.x(), rect.y());
}

void ChromeNode::set_visible(bool visible)
{
    wlr_scene_node_set_enabled(&m_buffer->node, visible);
}

bool ChromeNode::is_visible() const
{
    return m_buffer->node.enabled;
}

void ChromeNode::raise_to_top()
{
    wlr_scene_node_raise_to_top(&m_buffer->node);
}

void ChromeNode::render(Function<void(Gfx::Painter&)> const& callback)
{
    if (m_rect.is_empty())
        return;

    auto bitmap = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, m_rect.size()).release_value_but_fixme_should_propagate_errors();
    Gfx::Painter painter(*bitmap);
    callback(painter);

    auto* buffer = bitmap_buffer_create(move(bitmap));
    wlr_scene_buffer_set_buffer(m_buffer, buffer);
    wlr_buffer_drop(buffer);
}

// --- snapshots --------------------------------------------------------------

RefPtr<Gfx::Bitmap> snapshot_toplevel(wlr_scene_node& toplevel_node)
{
    // The client's content is the first scene surface under the toplevel node.
    wlr_buffer* client_buffer = nullptr;
    wlr_scene_node_for_each_buffer(&toplevel_node, [](wlr_scene_buffer* scene_buffer, int, int, void* data) {
        auto** out = static_cast<wlr_buffer**>(data);
        if (*out || !scene_buffer->buffer)
            return;
        if (wlr_scene_surface_try_from_buffer(scene_buffer))
            *out = scene_buffer->buffer;
    }, &client_buffer);

    if (!client_buffer)
        return nullptr;

    // Take the buffer so the client cannot reuse it while we read.
    auto* buffer = wlr_buffer_lock(client_buffer);
    if (!buffer)
        return nullptr;
    ScopeGuard unlock = [buffer] { wlr_buffer_unlock(buffer); };

    void* pixels = nullptr;
    uint32_t drm_format = 0;
    size_t stride = 0;
    if (!wlr_buffer_begin_data_ptr_access(buffer, WLR_BUFFER_DATA_PTR_ACCESS_READ, &pixels, &drm_format, &stride)) {
        // A DMA-BUF (or other non-mappable) buffer. A general scene-subtree render
        // or ext-image-copy-capture would cover this; callers fall back to the app
        // icon. See docs/09-r3-implementation-plan.md §3.
        return nullptr;
    }
    ScopeGuard end_access = [buffer] { wlr_buffer_end_data_ptr_access(buffer); };

    // SHM buffers arrive as ARGB8888 (BGRA in little-endian memory), which matches
    // Gfx::BitmapFormat::BGRA8888.
    auto bitmap = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, { buffer->width, buffer->height }).release_value_but_fixme_should_propagate_errors();
    for (int y = 0; y < buffer->height; ++y)
        memcpy(bitmap->scanline(y), static_cast<u8 const*>(pixels) + static_cast<size_t>(y) * stride, min(stride, static_cast<size_t>(buffer->width) * 4));
    return bitmap;
}
