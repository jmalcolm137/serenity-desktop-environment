/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// R5 ScreenCast consumer probe.
//
// Connects to the SDE ScreenCast PipeWire node and reports what the producer
// actually delivers: the SPA buffer data type (SPA_DATA_DmaBuf vs
// SPA_DATA_MemFd) and whether SPA_META_Cursor is present. This is the
// verification the portal docs ask for (§6.2/§6.3) — "the consumer receives
// SPA_DATA_DmaBuf, not MemFd" and "the metadata arrives as SPA_META_Cursor".
//
// Usage: screencast-consumer [target] [buffer-count]
//   target       node name, or a numeric node id / serial (default: sde-screencast)
//   buffer-count how many buffers to inspect before exiting (default: 5)

#define _GNU_SOURCE

#include <pipewire/pipewire.h>
#include <spa/buffer/meta.h>
#include <spa/param/video/raw.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>

#include <drm_fourcc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct consumer {
    struct pw_main_loop* loop;
    struct pw_stream* stream;
    char const* target;
    int wanted;
    int seen;
    int dmabuf;
    int memfd;
    int cursor_meta;
    int cursor_bitmap;
    int quit;
};

static struct consumer g = { 0 };

static void on_process(void* data)
{
    struct consumer* c = data;
    struct pw_buffer* buffer = pw_stream_dequeue_buffer(c->stream);
    if (!buffer)
        return;
    struct spa_buffer* spa_buffer = buffer->buffer;
    char const* kind = "?";
    if (spa_buffer->n_datas > 0) {
        if (spa_buffer->datas[0].type == SPA_DATA_DmaBuf) {
            c->dmabuf++;
            kind = "DMA-BUF";
        } else if (spa_buffer->datas[0].type == SPA_DATA_MemFd) {
            c->memfd++;
            kind = "memfd";
        }
    }
    struct spa_meta_cursor* cursor = spa_buffer_find_meta_data(spa_buffer, SPA_META_Cursor, sizeof(struct spa_meta_cursor));
    if (cursor && cursor->id != 0) {
        c->cursor_meta++;
        if (cursor->bitmap_offset >= sizeof(struct spa_meta_cursor))
            c->cursor_bitmap++;
    }
    pw_stream_queue_buffer(c->stream, buffer);
    c->seen++;
    fprintf(stderr, "CONSUMER buffer %d: %s%s\n", c->seen, kind,
        (cursor && cursor->id != 0) ? " +cursor-meta" : "");
    if (c->seen >= c->wanted) {
        c->quit = 1;
        pw_main_loop_quit(c->loop);
    }
}

static void on_param_changed(void* data, uint32_t id, const struct spa_pod* param)
{
    (void)data;
    if (id != SPA_PARAM_Format || !param)
        return;
    struct spa_video_info_raw info;
    if (spa_format_video_raw_parse(param, &info) < 0)
        return;
    printf("CONSUMER format %dx%d spa-format=%d%s\n", info.size.width, info.size.height, info.format,
        (info.flags & SPA_VIDEO_FLAG_MODIFIER) ? " modifier" : "");

    // Tell the producer we can consume DMA-BUF and memfd buffers, so a DMA-BUF
    // producer/consumer pair can be negotiated (docs' DMA-BUF procedure).
    uint8_t buffer[1024];
    struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    int stride = (int)info.size.width * 4;
    int buffertypes = (1 << SPA_DATA_DmaBuf) | (1 << SPA_DATA_MemFd);
    struct spa_pod* params[4];
    int n = 0;
    params[n++] = spa_pod_builder_add_object(&builder, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
        SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(4, 1, 8),
        SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
        SPA_PARAM_BUFFERS_size, SPA_POD_Int(stride * (int)info.size.height),
        SPA_PARAM_BUFFERS_stride, SPA_POD_Int(stride),
        SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(buffertypes));
    params[n++] = spa_pod_builder_add_object(&builder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
        SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
        SPA_PARAM_META_size, SPA_POD_Int((int)sizeof(struct spa_meta_header)));
    params[n++] = spa_pod_builder_add_object(&builder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
        SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoDamage),
        SPA_PARAM_META_size, SPA_POD_CHOICE_RANGE_Int((int)sizeof(struct spa_meta_region) * 16,
            (int)sizeof(struct spa_meta_region), (int)sizeof(struct spa_meta_region) * 16));
    params[n++] = spa_pod_builder_add_object(&builder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
        SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Cursor),
        SPA_PARAM_META_size, SPA_POD_CHOICE_RANGE_Int((int)(sizeof(struct spa_meta_cursor) + sizeof(struct spa_meta_bitmap) + 256 * 256 * 4),
            (int)sizeof(struct spa_meta_cursor), (int)(sizeof(struct spa_meta_cursor) + sizeof(struct spa_meta_bitmap) + 256 * 256 * 4)));
    pw_stream_update_params(g.stream, (const struct spa_pod**)params, n);
}

static void on_state_changed(void* data, enum pw_stream_state old, enum pw_stream_state state, char const* error)
{
    (void)data;
    (void)old;
    fprintf(stderr, "CONSUMER state %s%s%s\n", pw_stream_state_as_string(state), error ? " " : "", error ? error : "");
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = on_state_changed,
    .param_changed = on_param_changed,
    .process = on_process,
};

int main(int argc, char** argv)
{
    pw_init(&argc, &argv);
    g.target = argc > 1 ? argv[1] : "sde-screencast";
    g.wanted = argc > 2 ? atoi(argv[2]) : 5;

    g.loop = pw_main_loop_new(NULL);
    if (!g.loop) {
        fprintf(stderr, "screencast-consumer: pw_main_loop_new failed\n");
        return 2;
    }

    struct pw_properties* props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Video",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "Screen",
        NULL);
    // Prefer the serial (v6) when given a number, else the node name.
    pw_properties_set(props, PW_KEY_TARGET_OBJECT, g.target);

    g.stream = pw_stream_new_simple(pw_main_loop_get_loop(g.loop), "sde-screencast-consumer", props,
        &stream_events, &g);
    if (!g.stream) {
        fprintf(stderr, "screencast-consumer: pw_stream_new_simple failed\n");
        return 2;
    }

    uint8_t buffer[4096];
    struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    struct spa_rectangle min_size = SPA_RECTANGLE(1, 1);
    struct spa_rectangle max_size = SPA_RECTANGLE(16384, 16384);
    struct spa_fraction min_rate = SPA_FRACTION(0, 1);
    struct spa_fraction max_rate = SPA_FRACTION(1000, 1);
    struct spa_pod* params[5];
    int n_params = 0;
    // With CONSUMER_DMABUF set, first advertise a DMA-BUF format (modifier,
    // DONT_FIXATE) so a DMA-BUF producer can be negotiated; else SHM only.
    if (getenv("CONSUMER_DMABUF")) {
        struct spa_pod_frame frame;
        spa_pod_builder_push_object(&builder, &frame, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
        spa_pod_builder_add(&builder, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), 0);
        spa_pod_builder_add(&builder, SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), 0);
        spa_pod_builder_add(&builder, SPA_FORMAT_VIDEO_format,
            SPA_POD_CHOICE_ENUM_Id(7, SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_BGRx,
                SPA_VIDEO_FORMAT_RGBA, SPA_VIDEO_FORMAT_RGBx, SPA_VIDEO_FORMAT_ARGB, SPA_VIDEO_FORMAT_xRGB), 0);
        spa_pod_builder_add(&builder, SPA_FORMAT_VIDEO_size,
            SPA_POD_CHOICE_RANGE_Rectangle(SPA_POD_Rectangle(&max_size), SPA_POD_Rectangle(&min_size), SPA_POD_Rectangle(&max_size)), 0);
        spa_pod_builder_add(&builder, SPA_FORMAT_VIDEO_framerate,
            SPA_POD_CHOICE_RANGE_Fraction(SPA_POD_Fraction(&max_rate), SPA_POD_Fraction(&min_rate), SPA_POD_Fraction(&max_rate)), 0);
        spa_pod_builder_prop(&builder, SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY);
        spa_pod_builder_long(&builder, DRM_FORMAT_MOD_LINEAR);
        params[n_params++] = spa_pod_builder_pop(&builder, &frame);
    }
    {
        struct spa_pod_frame frame;
        spa_pod_builder_push_object(&builder, &frame, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
        spa_pod_builder_add(&builder, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), 0);
        spa_pod_builder_add(&builder, SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), 0);
        spa_pod_builder_add(&builder, SPA_FORMAT_VIDEO_format,
            SPA_POD_CHOICE_ENUM_Id(7, SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_BGRx,
                SPA_VIDEO_FORMAT_RGBA, SPA_VIDEO_FORMAT_RGBx, SPA_VIDEO_FORMAT_ARGB, SPA_VIDEO_FORMAT_xRGB), 0);
        spa_pod_builder_add(&builder, SPA_FORMAT_VIDEO_size,
            SPA_POD_CHOICE_RANGE_Rectangle(SPA_POD_Rectangle(&max_size), SPA_POD_Rectangle(&min_size), SPA_POD_Rectangle(&max_size)), 0);
        spa_pod_builder_add(&builder, SPA_FORMAT_VIDEO_framerate,
            SPA_POD_CHOICE_RANGE_Fraction(SPA_POD_Fraction(&max_rate), SPA_POD_Fraction(&min_rate), SPA_POD_Fraction(&max_rate)), 0);
        params[n_params++] = spa_pod_builder_pop(&builder, &frame);
    }
    // Accept DMA-BUF or memfd buffers, and the metadata a producer may attach.
    params[n_params++] = spa_pod_builder_add_object(&builder, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
        SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(4, 1, 8),
        SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
        SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_DmaBuf) | (1 << SPA_DATA_MemFd)));
    params[n_params++] = spa_pod_builder_add_object(&builder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
        SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
        SPA_PARAM_META_size, SPA_POD_Int((int)sizeof(struct spa_meta_header)));
    params[n_params++] = spa_pod_builder_add_object(&builder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
        SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Cursor),
        SPA_PARAM_META_size, SPA_POD_CHOICE_RANGE_Int((int)(sizeof(struct spa_meta_cursor) + sizeof(struct spa_meta_bitmap) + 256 * 256 * 4),
            (int)sizeof(struct spa_meta_cursor), (int)(sizeof(struct spa_meta_cursor) + sizeof(struct spa_meta_bitmap) + 256 * 256 * 4)));

    if (pw_stream_connect(g.stream, PW_DIRECTION_INPUT, PW_ID_ANY,
            PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS, (const struct spa_pod**)params, n_params) < 0) {
        fprintf(stderr, "screencast-consumer: pw_stream_connect failed\n");
        return 2;
    }

    pw_main_loop_run(g.loop);

    printf("CONSUMER buffers=%d dmabuf=%d memfd=%d cursor_meta=%d cursor_bitmap=%d\n",
        g.seen, g.dmabuf, g.memfd, g.cursor_meta, g.cursor_bitmap);
    if (g.dmabuf > 0)
        printf("CONSUMER verdict: zero-copy DMA-BUF buffers\n");
    else if (g.memfd > 0)
        printf("CONSUMER verdict: memfd buffers (no DMA-BUF negotiated)\n");
    else
        printf("CONSUMER verdict: no buffers seen\n");
    if (g.cursor_meta > 0)
        printf("CONSUMER verdict: SPA_META_Cursor present (%d with bitmap)\n", g.cursor_bitmap);

    pw_stream_destroy(g.stream);
    pw_main_loop_destroy(g.loop);
    pw_deinit();
    return g.seen > 0 ? 0 : 1;
}
