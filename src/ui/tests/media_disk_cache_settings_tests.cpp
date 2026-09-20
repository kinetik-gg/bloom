#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/media/cache/media_disk_cache.hpp>
#include <bloom/render/image.hpp>
#include <bloom/render/image_types.hpp>
#include <bloom/render/path_raster.hpp>
#include <bloom/runtime/gpu_prepared_upload_cache.hpp>
#include <bloom/runtime/gpu_scene_coverage_cache.hpp>
#include <bloom/runtime/operation_cache.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/cache_purge_controller.hpp>
#include <bloom/ui/media_disk_cache_settings.hpp>
#include <bloom/ui/task_ui_bridge.hpp>
#include <bloom/ui/window_status_bar.hpp>

// Private service surface: the deterministic timeout/withdraw test drives the owner-thread purge
// state machine directly, without a device.
#include "gpu_preview_display_service_private.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QIODevice>
#include <QSettings>
#include <QString>
#include <QTemporaryDir>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <source_location>
#include <string>
#include <string_view>
#include <thread>

namespace {

// The small "Expectations" idiom every Bloom test file already uses (see
// src/ui/tests/main_window_chrome_tests.cpp and src/ui/tests/ram_preview_tests.cpp), duplicated
// here rather than shared, matching the same precedent those two files themselves follow.
class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message,
                const std::source_location location = std::source_location::current()) {
        if (condition)
            return;
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }
    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

namespace ui = bloom::ui;
namespace cache = bloom::media::cache;
namespace runtime = bloom::runtime;

void testEnabledDefaultsTrue(Expectations& check) {
    QTemporaryDir directory;
    QSettings settings(directory.filePath(QStringLiteral("bloom.ini")), QSettings::IniFormat);
    check.expect(ui::mediaDiskCacheEnabledFromSettings(settings),
                 "an unset enabled setting defaults to true");
    settings.setValue(QStringLiteral("media/disk-cache-enabled"), false);
    check.expect(!ui::mediaDiskCacheEnabledFromSettings(settings), "an explicit false is honored");
}

void testDirectoryOverrideMustBeAbsolute(Expectations& check) {
    QTemporaryDir directory;
    QSettings settings(directory.filePath(QStringLiteral("bloom.ini")), QSettings::IniFormat);
    check.expect(ui::mediaDiskCacheDirectoryFromSettings(settings).empty(),
                 "an unset directory reads as empty (use the platform default)");
    settings.setValue(QStringLiteral("media/disk-cache-directory"),
                      QStringLiteral("relative/path"));
    check.expect(ui::mediaDiskCacheDirectoryFromSettings(settings).empty(),
                 "a relative directory override is refused, not silently resolved against cwd");
    const auto absolute = directory.filePath(QStringLiteral("override-cache"));
    settings.setValue(QStringLiteral("media/disk-cache-directory"), absolute);
    check.expect(ui::mediaDiskCacheDirectoryFromSettings(settings).string() ==
                     absolute.toStdString(),
                 "an absolute directory override is honored verbatim");
}

void testByteBudgetFallsBackOnInvalidValues(Expectations& check) {
    QTemporaryDir directory;
    QSettings settings(directory.filePath(QStringLiteral("bloom.ini")), QSettings::IniFormat);
    constexpr std::uint64_t fallback = 123'456'789ULL;
    check.expect(ui::mediaDiskCacheByteBudgetFromSettings(settings, fallback) == fallback,
                 "an unset budget uses the caller's fallback");
    for (const auto* value : {"0", "-1", "not-a-number"}) {
        settings.setValue(QStringLiteral("media/disk-cache-budget-bytes"),
                          QString::fromLatin1(value));
        check.expect(ui::mediaDiskCacheByteBudgetFromSettings(settings, fallback) == fallback,
                     "an invalid budget falls back rather than being silently accepted");
    }
    settings.setValue(QStringLiteral("media/disk-cache-budget-bytes"), QStringLiteral("4096"));
    check.expect(ui::mediaDiskCacheByteBudgetFromSettings(settings, fallback) == 4096,
                 "a valid positive budget is honored");
}

void testMakeMediaDiskCacheFromSettings(Expectations& check) {
    QTemporaryDir directory;
    QSettings settings(directory.filePath(QStringLiteral("bloom.ini")), QSettings::IniFormat);
    const auto root = directory.filePath(QStringLiteral("explicit-cache"));
    settings.setValue(QStringLiteral("media/disk-cache-directory"), root);
    settings.setValue(QStringLiteral("media/disk-cache-budget-bytes"), QStringLiteral("4194304"));
    auto built = ui::makeMediaDiskCacheFromSettings(settings);
    check.expect(built != nullptr, "an explicit directory and budget build a usable cache");
    if (built != nullptr) {
        check.expect(built->rootDirectory().string() == root.toStdString(),
                     "the built cache's root is exactly the configured directory");
        check.expect(built->byteBudget() == 4194304,
                     "the built cache's budget is what settings said");
        check.expect(built->enabled(), "a cache built from default-enabled settings is enabled");
    }

    settings.setValue(QStringLiteral("media/disk-cache-enabled"), false);
    check.expect(ui::makeMediaDiskCacheFromSettings(settings) == nullptr,
                 "disabled-in-settings builds no cache at all");
}

// mediaDiskCacheStatusText() (window_status_bar.hpp) is the free function the status bar's cell
// calls; tested directly here since it needs only a statistics value, not a live cache or widget.
void testMediaDiskCacheStatusText(Expectations& check) {
    cache::MediaDiskCacheStatistics empty;
    check.expect(ui::mediaDiskCacheStatusText(empty, /*enabled=*/false) ==
                     QStringLiteral("Disk cache off"),
                 "a disabled cache always reports 'Disk cache off', regardless of statistics");
    check.expect(ui::mediaDiskCacheStatusText(empty, /*enabled=*/true).isEmpty(),
                 "an enabled cache with no lookups yet and no resident entries reports nothing");

    cache::MediaDiskCacheStatistics warm;
    warm.hits = 3;
    warm.misses = 1;
    warm.storedBytes = 2ULL * 1024 * 1024;
    warm.entryCount = 4;
    const auto text = ui::mediaDiskCacheStatusText(warm, /*enabled=*/true);
    check.expect(text.contains(QStringLiteral("75")),
                 "3 hits of 4 lookups is reported as a 75% hit rate");
    check.expect(text.contains(QStringLiteral("MB")),
                 "resident bytes are reported with a readable unit");
}

// A tiny decodable image: enough for the disk store to write one real entry, cheap enough that the
// purge assertions are about cache effects rather than pixel work.
[[nodiscard]] std::shared_ptr<const bloom::render::Rgba32fImage> makeTestImage() {
    const auto window = bloom::render::ImageWindow::create(0, 0, 2, 2);
    const auto descriptor = bloom::render::Rgba32fImageDescriptor::create(
        *window.value(), *window.value(), bloom::core::PixelAspectRatio::square());
    auto builder = bloom::render::Rgba32fImageBuilder::create(*descriptor.value(), 4096);
    auto row = builder.value()->row(0);
    for (std::uint32_t x = 0; x < 2; ++x) {
        const auto pixel = bloom::render::Rgba32f::fromPremultiplied(0.1F, 0.2F, 0.3F, 1.0F);
        (*row.value())[x] = *pixel.value();
    }
    auto frozen = std::move(*builder.value()).freeze();
    return std::make_shared<const bloom::render::Rgba32fImage>(std::move(*frozen.value()));
}

// CACHE-PURGE: the media purge's real effect -- the on-disk decoded store AND the evaluator's
// in-memory decoded-media entries are dropped, derived operation results are left alone, and a
// source file is never opened for writing. This is the owning API the purge worker calls; the
// asynchronous, off-UI orchestration is exercised by the controller test below.
void testPurgeMediaCachesClearsDiskAndDecodedMemory(Expectations& check) {
    QTemporaryDir directory;
    check.expect(directory.isValid(), "purge media: temp directory is available");
    if (!directory.isValid()) {
        return;
    }

    cache::MediaDiskCacheConfig config;
    config.rootDirectory = std::filesystem::path(directory.filePath("media-cache").toStdString());
    config.byteBudget = 4ULL * 1024ULL * 1024ULL;
    cache::MediaDiskCache diskCache(config);

    const auto image = makeTestImage();
    const std::string key(64, 'a');
    diskCache.store(key, image);
    check.expect(diskCache.statistics().entryCount == 1 && diskCache.statistics().storedBytes > 0,
                 "purge media: the disk cache holds one real entry before the purge");

    runtime::OperationCache operationCache(std::size_t{1} << 20U);
    operationCache.store("decoded-key", bloom::document::Revision::fromRaw(1),
                         {.image = image, .values = {}, .bounds = {}},
                         runtime::OperationCacheEntryKind::DecodedMedia);
    operationCache.store("derived-key", bloom::document::Revision::fromRaw(1),
                         {.image = {}, .values = {}, .bounds = {}});
    check.expect(operationCache.retainedBytes(runtime::OperationCacheEntryKind::DecodedMedia) > 0 &&
                     operationCache.retainedBytes(runtime::OperationCacheEntryKind::Operation) > 0,
                 "purge media: memory holds decoded media and one derived result before the purge");

    const auto sourcePath = directory.filePath(QStringLiteral("source.png"));
    const QByteArray sourceBytes("source-bytes-that-must-survive");
    {
        QFile source(sourcePath);
        check.expect(source.open(QIODevice::WriteOnly), "purge media: the source file is writable");
        source.write(sourceBytes);
    }

    check.expect(ui::purgeMediaCaches(&diskCache, &operationCache),
                 "purge media: an available cache reports a real purge");
    const auto stats = diskCache.statistics();
    check.expect(stats.entryCount == 0 && stats.storedBytes == 0,
                 "purge media: the disk store is empty and its counters were reset");
    check.expect(operationCache.retainedBytes(runtime::OperationCacheEntryKind::DecodedMedia) == 0,
                 "purge media: the decoded-media memory entries are gone");
    check.expect(operationCache.retainedBytes(runtime::OperationCacheEntryKind::Operation) > 0,
                 "purge media: derived operation results are left for Purge preview cache");

    QFile source(sourcePath);
    check.expect(source.open(QIODevice::ReadOnly) && source.readAll() == sourceBytes,
                 "purge media: the source file is byte-for-byte untouched");
    QFile sourceAfter(sourcePath);
    check.expect(sourceAfter.exists(), "purge media: the source file still exists after the purge");

    check.expect(!ui::purgeMediaCaches(nullptr, nullptr),
                 "purge media: no store available reports nothing to purge");
}

// CACHE-PURGE: the asynchronous controller runs the clears only after the scheduler reports
// quiescence and does them on a worker lane. Preview purge drops derived operation results and the
// shared GPU-scene/prepared-upload stores while leaving decoded media and the disk store for the
// media purge; media purge clears the disk store, decoded still-image memory, the decoded-video
// memory and the prepared uploads. A caller-held image is never invalidated.
void testCachePurgeControllerClearsApplicableStores(Expectations& check) {
    QTemporaryDir directory;
    check.expect(directory.isValid(), "cache purge: temp directory is available");
    if (!directory.isValid()) {
        return;
    }

    runtime::TaskScheduler scheduler;
    ui::TaskUiBridge bridge(scheduler, nullptr, std::chrono::milliseconds{1});

    cache::MediaDiskCacheConfig config;
    config.rootDirectory = std::filesystem::path(directory.filePath("media").toStdString());
    config.byteBudget = 4ULL * 1024ULL * 1024ULL;
    cache::MediaDiskCache disk(config);
    runtime::OperationCache operationCache(std::size_t{1} << 20U);
    runtime::GpuPreparedUploadCache uploads(std::size_t{1} << 20U);
    runtime::GpuSceneCoverageCache coverage;

    const auto image = makeTestImage();
    disk.store(std::string(64, 'b'), image);
    operationCache.store("op", bloom::document::Revision::fromRaw(1),
                         {.image = {}, .values = {}, .bounds = {}});
    operationCache.store("decoded", bloom::document::Revision::fromRaw(1),
                         {.image = image, .values = {}, .bounds = {}},
                         runtime::OperationCacheEntryKind::DecodedMedia);
    uploads.store("upload", image);
    auto geometry = std::make_shared<bloom::render::PathRasterCoverageGeometry>();
    geometry->width = 1;
    geometry->height = 1;
    geometry->rows.push_back({0, 1});
    geometry->spans.push_back({0, 0});
    coverage.store("geometry", geometry);
    check.expect(disk.statistics().entryCount == 1 && operationCache.retainedBytes() > 0 &&
                     uploads.entryCount() == 1 && coverage.entryCount() == 1,
                 "cache purge: every store holds an entry before the purges");

    std::atomic<bool> videoCleared{false};
    std::atomic<int> gateOnCalls{0};
    std::atomic<int> gateOffCalls{0};
    std::atomic<int> gpuPurgeCalls{0};
    ui::CachePurgeController controller(
        scheduler, bridge, &disk, &operationCache, &uploads, &coverage,
        [&videoCleared] { videoCleared.store(true); },
        [&gateOnCalls, &gateOffCalls](const bool gated) {
            (gated ? gateOnCalls : gateOffCalls).fetch_add(1);
        },
        [&gpuPurgeCalls](const std::chrono::milliseconds /*timeout*/) {
            gpuPurgeCalls.fetch_add(1);
            return true;
        });
    const auto drive = [&controller] {
        for (int turn = 0; turn < 4000 && controller.isPurging(); ++turn) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            controller.poll();
        }
        return !controller.isPurging();
    };

    check.expect(controller.requestPreviewPurge(), "cache purge: a preview purge starts");
    check.expect(gateOnCalls.load() == 1,
                 "cache purge: preview producers are gated before the clear");
    check.expect(drive(), "cache purge: the preview purge completes");
    check.expect(gateOffCalls.load() == 1,
                 "cache purge: the producer gate is released when the purge finishes");
    check.expect(gpuPurgeCalls.load() == 1,
                 "cache purge: preview purge reaches the owning GPU cache purge API");
    check.expect(operationCache.retainedBytes(runtime::OperationCacheEntryKind::Operation) == 0,
                 "cache purge: preview purge drops derived operation results");
    check.expect(operationCache.retainedBytes(runtime::OperationCacheEntryKind::DecodedMedia) > 0,
                 "cache purge: preview purge leaves decoded media for the media purge");
    check.expect(uploads.entryCount() == 0 && coverage.entryCount() == 0,
                 "cache purge: preview purge clears the shared GPU-scene and upload stores");
    check.expect(disk.statistics().entryCount == 1,
                 "cache purge: preview purge leaves the disk store for the media purge");

    check.expect(controller.requestMediaPurge(), "cache purge: a media purge starts");
    check.expect(drive(), "cache purge: the media purge completes");
    check.expect(disk.statistics().entryCount == 0 && disk.statistics().storedBytes == 0,
                 "cache purge: media purge clears the disk store");
    check.expect(operationCache.retainedBytes(runtime::OperationCacheEntryKind::DecodedMedia) == 0,
                 "cache purge: media purge clears decoded still-image memory");
    check.expect(videoCleared.load(),
                 "cache purge: media purge reaches the decoded-video memory store");
    check.expect(gpuPurgeCalls.load() == 2,
                 "cache purge: media purge also clears the resident GPU scene cache");
    check.expect(image != nullptr && image->pixels().size_bytes() > 0,
                 "cache purge: a caller-held decoded image is not invalidated");
}

// A rendezvous so a producer task can be held mid-flight while the purge starts.
class ProducerGate final {
  public:
    void enterAndWait() {
        std::unique_lock lock(mutex_);
        entered_ = true;
        condition_.notify_all();
        condition_.wait(lock, [this] { return released_; });
    }
    [[nodiscard]] bool entered() const {
        std::lock_guard lock(mutex_);
        return entered_;
    }
    void release() {
        std::lock_guard lock(mutex_);
        released_ = true;
        condition_.notify_all();
    }

  private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    bool entered_ = false;
    bool released_ = false;
};

// CACHE-PURGE: with a producer genuinely in flight, the purge waits rather than clearing early; the
// producer's later store is therefore cleared too, so a cancelled operation cannot repopulate the
// cache after the purge. This is the race the generation-safety claim rests on.
void testCachePurgeWaitsForInFlightProducerThenClears(Expectations& check) {
    runtime::TaskScheduler scheduler;
    ui::TaskUiBridge bridge(scheduler, nullptr, std::chrono::milliseconds{1});
    runtime::OperationCache operationCache(std::size_t{1} << 20U);
    ProducerGate producerGate;
    auto submission = scheduler.submit<std::uint64_t>(
        runtime::TaskRequest(
            "Blocked preview producer",
            {.kind = runtime::TaskOwnerKind::Composition, .id = runtime::TaskOwnerId::fromRaw(1)},
            runtime::TaskPriority::Foreground),
        [&producerGate, &operationCache](runtime::TaskContext&) {
            producerGate.enterAndWait();
            operationCache.store("late", bloom::document::Revision::fromRaw(1),
                                 {.image = {}, .values = {}, .bounds = {}});
            return runtime::TaskResult<std::uint64_t>::succeeded(std::uint64_t{0});
        });
    check.expect(submission.accepted(), "cache purge wait: the producer is admitted");
    for (int turn = 0; turn < 2000 && !producerGate.entered(); ++turn) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }
    check.expect(producerGate.entered(), "cache purge wait: the producer is in flight");

    ui::CachePurgeController controller(scheduler, bridge, nullptr, &operationCache, nullptr,
                                        nullptr);
    check.expect(controller.requestPreviewPurge(), "cache purge wait: the purge starts");
    for (int turn = 0; turn < 20; ++turn) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        controller.poll();
    }
    check.expect(controller.isPurging(),
                 "cache purge wait: the purge waits while the producer is still in flight");
    check.expect(operationCache.retainedBytes() == 0,
                 "cache purge wait: nothing is cleared before the producer retires");

    producerGate.release();
    for (int turn = 0; turn < 4000 && controller.isPurging(); ++turn) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        controller.poll();
    }
    check.expect(!controller.isPurging(), "cache purge wait: the purge completes after retirement");
    check.expect(operationCache.retainedBytes() == 0,
                 "cache purge wait: the producer's late store cannot repopulate the cache");
}

// CACHE-PURGE: the service owner-thread purge state machine. A timeout withdraws a still-pending
// request under the same mutex the owner uses, so a withdrawn request can never clear late, and the
// serial API still works for the next purge. No device is needed: the resident route is selected by
// the immutable stage function, and the absent scene cache makes the clear itself a no-op.
void testServiceCachePurgeTimeoutWithdrawsAndSerializes(Expectations& check) {
    using bloom::runtime::detail::PreviewDisplayServiceCore;
    auto core = std::make_shared<PreviewDisplayServiceCore>();
    core->gpuStageFunction = [](const bloom::document::Snapshot&,
                                const bloom::runtime::PreviewRequestIdentity&, std::size_t,
                                const std::vector<bloom::runtime::SnapshotParameterOverride>&,
                                bloom::runtime::TaskContext&) {
        return bloom::runtime::TaskResult<
            bloom::runtime::PreviewGpuSceneStageOutcomeHandle>::cancelled();
    };

    check.expect(!bloom::runtime::detail::purgeServiceCaches(core, std::chrono::milliseconds{1}),
                 "service purge: a timeout with no owner reports not cleared");
    {
        std::lock_guard lock(core->cachePurgeMutex);
        check.expect(!core->cachePurgePending && core->cachePurgeCompleted == 0,
                     "service purge: a timed-out request is withdrawn, never left pending");
    }
    bloom::runtime::detail::processServiceCachePurge(core);
    {
        std::lock_guard lock(core->cachePurgeMutex);
        check.expect(core->cachePurgeCompleted == 0,
                     "service purge: a withdrawn request never clears late");
    }

    std::atomic<bool> secondResult{false};
    std::thread owner([&core] {
        for (;;) {
            {
                std::lock_guard lock(core->cachePurgeMutex);
                if (core->cachePurgePending) {
                    break;
                }
            }
            std::this_thread::yield();
        }
        bloom::runtime::detail::processServiceCachePurge(core);
    });
    std::thread caller([&core, &secondResult] {
        secondResult.store(
            bloom::runtime::detail::purgeServiceCaches(core, std::chrono::milliseconds{2000}));
    });
    caller.join();
    owner.join();
    check.expect(secondResult.load(), "service purge: a later purge works after a cancelled one");
    {
        std::lock_guard lock(core->cachePurgeMutex);
        check.expect(core->cachePurgeCompleted == 1 && !core->cachePurgePending,
                     "service purge: completion is recorded and no pending state is left");
    }
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    Expectations check;
    testEnabledDefaultsTrue(check);
    testDirectoryOverrideMustBeAbsolute(check);
    testByteBudgetFallsBackOnInvalidValues(check);
    testMakeMediaDiskCacheFromSettings(check);
    testMediaDiskCacheStatusText(check);
    testPurgeMediaCachesClearsDiskAndDecodedMemory(check);
    testCachePurgeControllerClearsApplicableStores(check);
    testCachePurgeWaitsForInFlightProducerThenClears(check);
    testServiceCachePurgeTimeoutWithdrawsAndSerializes(check);
    return check.failures() == 0 ? 0 : 1;
}
