/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Regression: FileSystemModel rendered thumbnails by creating a nested
// Core::EventLoop on the single LibThreading background thread and awaiting the
// ImageDecoder promise there. That blocked the shared background thread for the
// whole decode, so one thumbnail stalled every other background action in the
// process, and a second request for the same file could start another decode.
//
// Decoding now runs on the UI event loop and only the CPU-side scaling runs on
// the background thread. This test builds a small directory of PNGs, requests
// each icon twice (the second request must join the in-flight decode rather than
// start a new one), pumps the event loop, and checks that every node ends up
// with a 32x32 thumbnail and that progress was reported once per file.
//
// The driver is scripts/run-filemodel-thumbnail-test.sh.
#include <AK/Atomic.h>
#include <AK/Vector.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibGUI/Application.h>
#include <LibGUI/FileSystemModel.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/ImageFormats/PNGWriter.h>
#include <LibMain/Main.h>
#include <LibThreading/BackgroundAction.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

namespace {

int g_failures = 0;

void fail(char const* what)
{
    if (g_failures < 20)
        warnln("FAIL: {}", what);
    ++g_failures;
}

bool log_contains(StringView path, StringView needle)
{
    auto file = Core::File::open(path, Core::File::OpenMode::Read);
    if (file.is_error())
        return false;
    auto contents = file.value()->read_until_eof();
    if (contents.is_error())
        return false;
    return StringView { contents.value() }.contains(needle);
}

ErrorOr<ByteString> write_test_image(ByteString const& directory, int index)
{
    auto bitmap = TRY(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, { 48, 32 }));
    auto png = TRY(Gfx::PNGWriter::encode(*bitmap));

    auto path = ByteString::formatted("{}/image-{}.png", directory, index);
    auto file = TRY(Core::File::open(path, Core::File::OpenMode::Write));
    TRY(file->write_until_depleted(png));
    return path;
}

}

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    auto app = TRY(GUI::Application::create(arguments));
    (void)app;

    char directory_template[] = "/tmp/sde-filemodel-thumbnail-XXXXXX";
    auto* directory = mkdtemp(directory_template);
    if (!directory) {
        warnln("FAIL: could not create a temporary directory");
        return 1;
    }
    ByteString directory_path { directory, strlen(directory) };

    constexpr int image_count = 3;
    Vector<ByteString> paths;
    for (int i = 0; i < image_count; ++i)
        paths.append(TRY(write_test_image(directory_path, i)));

    auto model = GUI::FileSystemModel::create(directory_path, GUI::FileSystemModel::Mode::FilesAndDirectories);

    int progress_done = 0;
    int progress_total = 0;
    model->on_thumbnail_progress = [&](int done, int total) {
        progress_done = done;
        progress_total = total;
    };
    model->on_complete = [] {};

    Vector<GUI::ModelIndex> indices;
    for (auto const& path : paths) {
        auto index = model->index(path, GUI::FileSystemModel::Column::Name);
        if (!index.is_valid()) {
            warnln("FAIL: no model index for {}", path);
            ++g_failures;
            continue;
        }
        indices.append(index);
    }

    // Hold the single shared background thread. Thumbnail decoding must not need
    // it: the decode request is sent from the UI thread, and only the final
    // scaling is queued behind this blocker. The old implementation queued the
    // whole decode here, so the ImageDecoder never saw a connection while the
    // thread was busy.
    auto const* decoder_log_path = getenv("SDE_IMAGEDECODER_LOG");
    Atomic<bool> blocker_started { false };
    Atomic<bool> blocker_release { false };
    auto blocker = Threading::BackgroundAction<int>::construct(
        [&](auto&) -> ErrorOr<int> {
            blocker_started.store(true, AK::MemoryOrder::memory_order_release);
            while (!blocker_release.load(AK::MemoryOrder::memory_order_acquire))
                usleep(1000);
            return 0;
        },
        [](int) -> ErrorOr<void> { return {}; },
        [](Error) {});

    while (!blocker_started.load(AK::MemoryOrder::memory_order_acquire))
        usleep(1000);

    if (!indices.is_empty()) {
        // Two requests for the same file: the second must observe the in-flight
        // decode and must not increment the model's work count again.
        (void)model->data(indices[0], GUI::ModelRole::Icon);
        (void)model->data(indices[0], GUI::ModelRole::Icon);
    }

    if (!decoder_log_path) {
        fail("SDE_IMAGEDECODER_LOG is not set");
    } else {
        auto timer = Core::ElapsedTimer();
        timer.start();
        bool decode_started = false;
        while (timer.elapsed() < 5000) {
            if (log_contains(StringView { decoder_log_path, strlen(decoder_log_path) }, "client connected"sv)) {
                decode_started = true;
                break;
            }
            usleep(1000);
        }
        if (!decode_started)
            fail("the thumbnail decode did not start while the shared background thread was busy");
    }

    blocker_release.store(true, AK::MemoryOrder::memory_order_release);

    // Decode the rest with the background thread available again.
    for (size_t i = 1; i < indices.size(); ++i) {
        (void)model->data(indices[i], GUI::ModelRole::Icon);
        (void)model->data(indices[i], GUI::ModelRole::Icon);
    }

    if (g_failures == 0) {
        auto timer = Core::ElapsedTimer();
        timer.start();
        while (progress_done < image_count && timer.elapsed() < 10000) {
            Core::EventLoop::current().pump(Core::EventLoop::WaitMode::PollForEvents);
            usleep(1000);
        }
    }

    if (progress_done != image_count) {
        warnln("FAIL: thumbnails finished {}/{} (total reported {})", progress_done, image_count, progress_total);
        ++g_failures;
    }
    if (progress_total != image_count) {
        warnln("FAIL: {} thumbnail jobs were started for {} files (duplicate requests were not deduplicated)", progress_total, image_count);
        ++g_failures;
    }

    for (size_t i = 0; i < indices.size(); ++i) {
        // The completion path caches the bitmap and tells the view to refresh;
        // the node only picks it up on the next icon query.
        (void)model->data(indices[i], GUI::ModelRole::Icon);

        auto const& node = model->node(indices[i]);
        if (!node.thumbnail) {
            warnln("FAIL: {} has no thumbnail", paths[i]);
            ++g_failures;
            continue;
        }
        if (node.thumbnail->size() != Gfx::IntSize { 32, 32 }) {
            warnln("FAIL: {} thumbnail is {}x{}, expected 32x32", paths[i], node.thumbnail->width(), node.thumbnail->height());
            ++g_failures;
        }
    }

    for (auto const& path : paths)
        ::unlink(path.characters());
    ::rmdir(directory);

    if (g_failures != 0) {
        warnln("{} check(s) failed", g_failures);
        return 1;
    }
    outln("ok: thumbnails decode asynchronously and are deduplicated");
    return 0;
}
