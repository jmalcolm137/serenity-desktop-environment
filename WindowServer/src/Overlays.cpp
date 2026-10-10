/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Overlays.h"

#include <LibGfx/Painter.h>
#include <drm_fourcc.h>

// wlroots is a C library and does not wrap its headers in extern "C", so we do.
extern "C" {
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_buffer.h>
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

RefPtr<Gfx::Bitmap> snapshot_toplevel(wlr_scene_node&)
{
    // TODO(WS3 spike): composite the toplevel's scene subtree into a bitmap, or
    // drive ext-image-copy-capture-v1. See docs/09-r3-implementation-plan.md §3.
    return nullptr;
}
