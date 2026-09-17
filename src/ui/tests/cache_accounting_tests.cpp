// Task CACHEFIX-1, deliverable 2: prove where 38 GB of resident memory could come from, and prove
// it cannot come from there any more.
//
// The incident was an import: the owner was dragging assets when a release build reached 38 GB RSS
// on a 60 GB machine with a 3 GB swap, froze the desktop, and died with SIGABRT. This exercise is
// that import at the scale that produced it -- two hundred 4K RGBA32F frames, 126.6 MiB each,
// 24.7 GiB of decoded pixels offered to every cache in the process one after another -- and it
// asserts two things afterwards: every cache's own byte account is inside the budget or hard cap it
// was given, and the process's resident set never grew past the sum of those budgets plus a small
// slack. The second assertion is what makes the first mean something: a cache can only report what
// it knows it holds, and the bug was a pool that knew nothing.
//
// This drives the cache APIs directly rather than the Assets panel. The accounting under test lives
// in the caches, the importer only feeds them, and two hundred real 4K files on disk would make the
// suite cost gigabytes of I/O to assert nothing extra.
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/media/audio/audio.hpp>
#include <bloom/media/cache/media_disk_cache.hpp>
#include <bloom/media/video/session.hpp>
#include <bloom/render/image.hpp>
#include <bloom/render/image_types.hpp>
#include <bloom/runtime/memory_budget_ledger.hpp>
#include <bloom/runtime/operation_cache.hpp>

#include <bloom/ui/asset_controller.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/project_host.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QTemporaryDir>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

// Deliberately self-contained rather than borrowing src/output/tests/png_test_support.hpp: this
// suite writes no PNGs, and reusing that helper would drag bloom_output and ZLIB into the link for
// two small utilities.
class Expectations final {
  public:
    void expect(const bool condition, const std::string& what) {
        if (condition)
            return;
        ++failures_;
        std::cerr << "FAIL: " << what << '\n';
    }
    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

// A private directory removed on destruction, so a refused-write test leaves nothing behind.
class ScratchDirectory final {
  public:
    explicit ScratchDirectory(const std::string& name)
        : path_(std::filesystem::temp_directory_path() /
                (name + "-" +
                 std::to_string(static_cast<unsigned long long>(
                     std::chrono::steady_clock::now().time_since_epoch().count())))) {
        std::error_code error;
        std::filesystem::create_directories(path_, error);
    }
    ScratchDirectory(const ScratchDirectory&) = delete;
    ScratchDirectory& operator=(const ScratchDirectory&) = delete;
    ScratchDirectory(ScratchDirectory&&) = delete;
    ScratchDirectory& operator=(ScratchDirectory&&) = delete;
    ~ScratchDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

  private:
    std::filesystem::path path_;
};

namespace cache = bloom::media::cache;
namespace core = bloom::core;
namespace render = bloom::render;
namespace runtime = bloom::runtime;

constexpr std::size_t kMebibyte = std::size_t{1024} * 1024U;
// 3840 x 2160 x 4 channels x 4 bytes. The number the audit is about.
constexpr std::uint32_t kUhdWidth = 3840;
constexpr std::uint32_t kUhdHeight = 2160;
constexpr std::size_t kUhdBytes =
    std::size_t{kUhdWidth} * kUhdHeight * 4U * sizeof(float); // 126.6 MiB
constexpr int kImportedImages = 200;

// Resident set size in bytes, or 0 where the platform does not report one. Only Linux is asserted
// against: this is the only target whose RSS is readable without a platform API, and the budgets it
// checks are platform-independent, so a missing reading skips the assertion rather than failing it.
[[nodiscard]] std::size_t residentBytes() {
#if defined(__linux__)
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        constexpr std::string_view label = "VmRSS:";
        if (!std::string_view(line).starts_with(label))
            continue;
        return static_cast<std::size_t>(std::strtoull(line.c_str() + label.size(), nullptr, 10)) *
               1024U;
    }
#endif
    return 0;
}

// A 4K frame whose pixels are cheap to produce: the builder's own zeroed storage with one written
// row, so the test spends its time on the caches rather than on 24.7 GiB of arithmetic. The
// ALLOCATION is what matters here, and it is full size.
[[nodiscard]] std::shared_ptr<const render::Rgba32fImage> makeUhdImage(const float seed) {
    const auto window = render::ImageWindow::create(0, 0, kUhdWidth, kUhdHeight);
    const auto descriptor = render::Rgba32fImageDescriptor::create(
        *window.value(), *window.value(), core::PixelAspectRatio::square());
    auto builder = render::Rgba32fImageBuilder::create(*descriptor.value(), kUhdBytes * 2U);
    auto row = builder.value()->row(0);
    for (std::uint32_t x = 0; x < kUhdWidth; ++x) {
        const auto pixel = render::Rgba32f::fromPremultiplied(seed, seed * 0.5F, 0.25F, 1.0F);
        (*row.value())[x] = *pixel.value();
    }
    auto frozen = std::move(*builder.value()).freeze();
    return std::make_shared<const render::Rgba32fImage>(std::move(*frozen.value()));
}

[[nodiscard]] std::string keyFor(const int index) {
    // 64 hex characters, the shape MediaDiskCache sharding expects, without depending on a hasher.
    std::string key(64, '0');
    auto value = static_cast<unsigned>(index) + 1U;
    for (std::size_t digit = 0; digit < 8 && value != 0; ++digit, value /= 16U)
        key[digit] = "0123456789abcdef"[value % 16U];
    return key;
}

// The AssetController's proxy cache, in the shape asset_controller.cpp builds it: at most 512
// entries AND at most 8 MiB, evicting to satisfy both.
struct ProxyCache final {
    static constexpr std::size_t kEntryLimit = 512;
    static constexpr std::size_t kByteCapacity = std::size_t{8} * kMebibyte;
    std::map<std::string, QImage> entries;
    std::size_t bytes = 0;

    void insert(const std::string& key, const QImage& image) {
        const auto cost = static_cast<std::size_t>(image.sizeInBytes());
        while (!entries.empty() && (entries.size() >= kEntryLimit ||
                                    cost > kByteCapacity - std::min(bytes, kByteCapacity))) {
            bytes -=
                std::min(bytes, static_cast<std::size_t>(entries.begin()->second.sizeInBytes()));
            entries.erase(entries.begin());
        }
        if (cost <= kByteCapacity) {
            entries.emplace(key, image);
            bytes += cost;
        }
    }
};

void testTwoHundredUhdImportsStayInsideEveryBudget(Expectations& check) {
    ScratchDirectory scratch("cachefix1-import-accounting");
    // Budgets small enough that 24.7 GiB of offered pixels must be refused or evicted, and large
    // enough that each one holds more than a single 4K frame: a budget that cannot hold one frame
    // would pass this test by never caching anything.
    constexpr std::size_t kOperationBudget = 384 * kMebibyte;
    constexpr std::size_t kAsyncQueueCapacity = 8 * kMebibyte;
    constexpr std::size_t kSlack = 256 * kMebibyte;

    cache::MediaDiskCacheConfig config;
    config.rootDirectory = scratch.path();
    config.byteBudget = 64 * kMebibyte;
    config.asyncQueueByteCapacity = kAsyncQueueCapacity;
    cache::MediaDiskCache diskCache(config);
    runtime::OperationCache operationCache(kOperationBudget);
    ProxyCache proxies;

    const auto baseline = residentBytes();
    std::size_t peakResident = baseline;
    for (int index = 0; index < kImportedImages; ++index) {
        auto image = makeUhdImage(static_cast<float>(index % 97) / 97.0F);
        // What an import does with a decoded frame: hand it to the disk cache to persist off the
        // calling thread, keep it in the evaluator's decoded-media cache, and scale a proxy for the
        // Assets panel.
        diskCache.storeAsync(keyFor(index), image);
        operationCache.store("decoded:" + keyFor(index), bloom::document::Revision{},
                             {.image = image, .values = {}, .bounds = {}},
                             runtime::OperationCacheEntryKind::DecodedMedia);
        QImage proxy(64, 64, QImage::Format_RGBA8888);
        proxy.fill(0);
        proxies.insert(keyFor(index), proxy);
        image.reset();
        peakResident = std::max(peakResident, residentBytes());

        // The invariants hold at EVERY step, not only at the end: a pool that spiked to 8 GiB and
        // drained before the final assertion would still have frozen the machine.
        if (operationCache.retainedBytes() > kOperationBudget) {
            check.expect(false, "the operation cache stayed inside its budget at every step");
            break;
        }
        if (diskCache.statistics().asyncQueueBytes > kAsyncQueueCapacity) {
            check.expect(false, "the pending-write queue stayed inside its capacity at every step");
            break;
        }
    }

    const auto stats = diskCache.statistics();
    check.expect(operationCache.retainedBytes() <= kOperationBudget,
                 "200 4K imports leave the operation cache inside its budget");
    check.expect(operationCache.retainedBytes(runtime::OperationCacheEntryKind::DecodedMedia) ==
                     operationCache.retainedBytes(),
                 "every byte the operation cache holds is accounted to decoded media");
    check.expect(operationCache.retainedBytes() >= kUhdBytes,
                 "the operation cache did hold real 4K frames rather than refusing everything");
    check.expect(stats.peakAsyncQueueBytes <= kAsyncQueueCapacity,
                 "the disk cache's pending-write queue never passed its byte capacity");
    check.expect(stats.droppedAsyncWrites == static_cast<std::uint64_t>(kImportedImages),
                 "a 4K write larger than the queue capacity is refused rather than staged");
    check.expect(proxies.bytes <= ProxyCache::kByteCapacity &&
                     proxies.entries.size() <= ProxyCache::kEntryLimit,
                 "the proxy cache honors both its entry limit and its byte capacity");

    const auto tracked = kOperationBudget + kAsyncQueueCapacity + ProxyCache::kByteCapacity;
    if (baseline != 0 && peakResident != 0) {
        const auto growth = peakResident > baseline ? peakResident - baseline : 0;
        check.expect(growth <= tracked + kSlack,
                     "resident memory grew no further than the budgets plus a small slack");
        std::cout << "cache accounting: offered "
                  << static_cast<double>(kUhdBytes) * kImportedImages / (1024.0 * 1024 * 1024)
                  << " GiB across " << kImportedImages << " 4K frames; RSS grew "
                  << static_cast<double>(growth) / (1024.0 * 1024) << " MiB against a tracked "
                  << static_cast<double>(tracked) / (1024.0 * 1024) << " MiB of budgets\n";
        std::cout.flush();
    }
}

// CACHEFIX-2: change the machine while imports continue, then keep offering frames after both
// pressure polls. This catches a trim-only implementation that immediately refills the cache.
void testContentionDuringTwoHundredImports(Expectations& check) {
    constexpr auto gib = 1024 * kMebibyte;
    constexpr auto operationBudget = 640 * kMebibyte;
    constexpr auto videoBudget = 8 * kMebibyte;
    constexpr auto queueBudget = 8 * kMebibyte;
    constexpr auto trackedBudget = operationBudget + videoBudget + queueBudget;
    constexpr auto slack = 256 * kMebibyte;
    runtime::MemoryBudgetLedger ledger(16 * gib, 16 * gib);
    runtime::OperationCache operations(operationBudget, ledger);
    bloom::media::video::DecodedVideoCache video(videoBudget, ledger);
    ScratchDirectory scratch("cachefix2-contention");
    cache::MediaDiskCacheConfig config;
    config.rootDirectory = scratch.path();
    config.asyncQueueByteCapacity = queueBudget;
    cache::MediaDiskCache disk(config, ledger);
    const auto baseline = residentBytes();
    auto peak = baseline;
    std::size_t before = 0, first = 0, second = 0;
    const auto tracked = [&] {
        return operations.retainedBytes() + video.residentBytes() +
               disk.statistics().asyncQueueBytes;
    };
    for (int index = 0; index < kImportedImages; ++index) {
        auto image = makeUhdImage(static_cast<float>(index % 97) / 97.0F);
        operations.store("contention:" + keyFor(index), {},
                         {.image = image, .values = {}, .bounds = {}},
                         index % 2 == 0 ? runtime::OperationCacheEntryKind::DecodedMedia
                                        : runtime::OperationCacheEntryKind::Operation);
        disk.storeAsync(keyFor(index), image);
        auto frame = std::make_shared<bloom::media::provider::FrameProduct>();
        frame->format = bloom::media::provider::PixelFormat::Rgba8;
        frame->colour = {1, 1, 1, 1};
        bloom::media::provider::CpuPlane plane;
        plane.width = 1024;
        plane.height = 512;
        plane.stride = 4096;
        plane.bytes.resize(static_cast<std::size_t>(plane.stride) * plane.height);
        plane.digest = bloom::media::provider::digestBytes(plane.bytes);
        frame->planes.push_back(std::move(plane));
        video.store({{}, 0, static_cast<std::uint64_t>(index), 0}, std::move(frame));
        image.reset();
        peak = std::max(peak, residentBytes());
        if (index == 99) {
            before = tracked();
            check.expect(
                before > operationBudget / 2 && video.residentBytes() > videoBudget / 2,
                "operation, decoded media and video caches hold real frames before pressure");
            const auto state = ledger.poll({.availableBytes = 2 * gib},
                                           runtime::MemoryBudgetLedger::Clock::time_point{});
            first = tracked();
            check.expect(state.retentionPercent == 25 && first <= trackedBudget / 4,
                         "first contention poll trims all tracked caches below 25 percent");
        }
        if (index == 100) {
            const auto state = ledger.poll({.availableBytes = 2 * gib},
                                           runtime::MemoryBudgetLedger::Clock::time_point{} +
                                               std::chrono::seconds(5));
            second = tracked();
            check.expect(state.retentionPercent == 10 && second < trackedBudget / 4,
                         "within two polls tracked bytes are below 25 percent despite new offers");
        }
        if (index >= 100)
            check.expect(tracked() <= trackedBudget / 10,
                         "continuing imports cannot refill beyond the persistent pressure limit");
    }
    check.expect(operations.statistics().pressureDrops > 0, "real cache entries were evicted");
    check.expect(disk.asyncQueueByteCapacity() <= queueBudget / 10,
                 "disk queue admission follows the same ledger callback");
    if (baseline != 0 && peak != 0)
        check.expect(peak - baseline <= trackedBudget + slack,
                     "contention RSS growth stays inside budgets plus transient-frame slack");
    std::cout << "contention: 200 4K frames; tracked MiB "
              << static_cast<double>(before) / kMebibyte << " -> "
              << static_cast<double>(first) / kMebibyte << " (poll 1) -> "
              << static_cast<double>(second) / kMebibyte << " (poll 2); RSS growth "
              << static_cast<double>(peak - baseline) / kMebibyte << " MiB; budget + slack "
              << static_cast<double>(trackedBudget + slack) / kMebibyte << " MiB\n";
}

// The pool the audit found. Before CACHEFIX-1 the queue's only bound was 64 ENTRIES, so this same
// loop staged 64 x 126.6 MiB = 7.9 GiB of decoded pixels the memory ledger never saw -- and 31.6
// GiB had the frames been 8K. The bound is now bytes, and it is applied before the memory is
// staged rather than by the writer thread afterwards.
void testUnboundedAsyncQueueWouldHaveHeldGigabytes(Expectations& check) {
    ScratchDirectory scratch("cachefix1-queue-bound");
    cache::MediaDiskCacheConfig config;
    config.rootDirectory = scratch.path();
    config.byteBudget = 16 * kMebibyte;
    cache::MediaDiskCache diskCache(config);
    check.expect(diskCache.asyncQueueByteCapacity() == cache::kMediaDiskCacheAsyncQueueByteCapacity,
                 "the default pending-write queue capacity is the shipped 256 MiB");
    const auto entryBoundOnly = cache::kMediaDiskCacheAsyncQueueCapacity * kUhdBytes;
    check.expect(entryBoundOnly > std::size_t{7} * 1024U * kMebibyte,
                 "64 pending 4K frames would have been more than 7 GiB under the entry bound");
    check.expect(cache::kMediaDiskCacheAsyncQueueByteCapacity < entryBoundOnly / 20,
                 "the byte bound is the binding one for frames this size");
}

// CACHEFIX-1 deliverable 3: a cache insert is an optimization. std::bad_alloc raised while
// retaining an entry unwinds the partial insert, is counted, and returns -- the value the caller
// produced is untouched, so the frame it belongs to still renders. Armed for exactly one
// allocation on THIS thread (see the operator new replacement below), so nothing else in the
// process is affected.
std::size_t proxyPixelBytes() { return std::size_t{64} * 64U * 4U; }

} // namespace

// A failing allocator, armed one allocation at a time and only for the thread that armed it, so a
// worker thread can never trip it. Replacing the global operators is per-binary; this test does no
// Qt event loop and nothing else here allocates between arming and the call under test.
namespace {
thread_local bool gFailNextAllocation = false;
}

void* operator new(const std::size_t size) {
    if (gFailNextAllocation) {
        gFailNextAllocation = false;
        throw std::bad_alloc{};
    }
    if (void* const memory = std::malloc(size == 0 ? 1 : size))
        return memory;
    throw std::bad_alloc{};
}
void* operator new[](const std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace {

void testBadAllocOnInsertDropsTheEntryAndKeepsTheValue(Expectations& check) {
    runtime::OperationCache operationCache(4 * kMebibyte);
    const auto revision = bloom::document::Revision::fromRaw(1);
    const runtime::OperationCacheValue value{
        .image = {}, .values = {runtime::CompiledValue{0.5}}, .bounds = {}};
    operationCache.store("healthy", revision, value);
    const auto healthy = operationCache.retainedBytes();
    check.expect(healthy > 0, "a healthy insert is retained");

    // Everything the call needs is built BEFORE the allocator is armed and moved in, because
    // store() takes its content and value by value: a copy at the call site would spend the armed
    // allocation outside the code under test.
    const std::string starved = "starved-entry";
    std::string movableContent = starved;
    runtime::OperationCacheValue movableValue{
        .image = {}, .values = {runtime::CompiledValue{0.5}}, .bounds = {}};
    gFailNextAllocation = true;
    operationCache.store(std::move(movableContent), revision, std::move(movableValue));
    gFailNextAllocation = false;

    check.expect(operationCache.statistics().allocationFailures == 1,
                 "a failed allocation during an insert is caught and counted");
    check.expect(!operationCache.find(starved, revision).has_value(),
                 "the entry whose allocation failed was dropped, not half-inserted");
    check.expect(operationCache.retainedBytes() == healthy,
                 "a dropped insert leaves the cache's byte account exactly as it was");
    check.expect(operationCache.find("healthy", revision).has_value(),
                 "the entries already retained survive a failed insert");
    // The caller's own value -- what the frame is rendered from -- was never the cache's to lose.
    check.expect(value.values.size() == 1, "the value the caller produced is untouched");

    operationCache.store("after-failure", revision, value);
    check.expect(operationCache.find("after-failure", revision).has_value(),
                 "the cache keeps working after a failed insert");
}

void testProxyCacheBoundIsMeasuredRatherThanAssumed(Expectations& check) {
    ProxyCache proxies;
    for (int index = 0; index < 2000; ++index) {
        QImage proxy(64, 64, QImage::Format_RGBA8888);
        proxy.fill(0);
        proxies.insert(keyFor(index), proxy);
    }
    check.expect(proxies.entries.size() <= ProxyCache::kEntryLimit,
                 "the proxy cache holds no more than 512 entries");
    check.expect(proxies.bytes <= ProxyCache::kByteCapacity,
                 "the proxy cache holds no more than 8 MiB");
    check.expect(ProxyCache::kEntryLimit * proxyPixelBytes() <= ProxyCache::kByteCapacity,
                 "512 proxies of 64x64 RGBA8 are exactly the 8 MiB the byte capacity allows");
}

// Decoded audio was the other pool with no aggregate bound: AudioDecodeLimits caps ONE buffer at
// 48M samples (about 192 MB), and the controller kept one per audio asset with no total.
void testDecodedAudioAggregateIsBounded(Expectations& check) {
    constexpr std::size_t kCapacity = std::size_t{2} * 1024U * kMebibyte;
    const auto perAsset =
        static_cast<std::size_t>(bloom::media::audio::AudioDecodeLimits::kDefaultSampleBudget) *
        sizeof(float);
    check.expect(perAsset > 100 * kMebibyte,
                 "one decoded audio buffer is already a substantial allocation");
    std::size_t held = 0;
    int admitted = 0;
    for (int asset = 0; asset < 64; ++asset)
        if (perAsset <= kCapacity - std::min(held, kCapacity)) {
            held += perAsset;
            ++admitted;
        }
    check.expect(held <= kCapacity, "the decoded-audio aggregate never passes its capacity");
    check.expect(admitted > 0 && admitted < 64,
                 "the cap admits real projects and refuses a project that would exhaust memory");
}

void testAssetProxyAndAudioParticipation(Expectations& check) {
    QTemporaryDir directory;
    QImage image(64, 64, QImage::Format_RGBA8888);
    image.fill(0xffffffffU);
    check.expect(image.save(directory.filePath("proxy.png")), "write proxy fixture");
    const auto audioPath = directory.filePath("tone.wav");
    {
        std::ofstream wave(audioPath.toStdString(), std::ios::binary);
        const auto le = [&wave](std::uint32_t value, unsigned bytes) {
            for (unsigned index = 0; index < bytes; ++index) {
                wave.put(static_cast<char>(value & 255));
                value >>= 8;
            }
        };
        wave.write("RIFF", 4);
        le(36 + 8192, 4);
        wave.write("WAVEfmt ", 8);
        le(16, 4);
        le(1, 2);
        le(1, 2);
        le(48000, 4);
        le(96000, 4);
        le(2, 2);
        le(16, 2);
        wave.write("data", 4);
        le(8192, 4);
        for (unsigned index = 0; index < 4096; ++index)
            le(index % 32768, 2);
    }
    runtime::TaskSchedulerConfig config;
    config.cpuWorkerCount = 1;
    config.blockingIoWorkerCount = 1;
    runtime::TaskScheduler scheduler(config);
    bloom::ui::ProjectHost host(scheduler);
    bloom::ui::CompositionSession session(*host.liveDocumentAndStack().first,
                                          *host.liveDocumentAndStack().second,
                                          host.lowestCompositionId());
    bloom::ui::TaskUiBridge bridge(scheduler);
    bloom::ui::AssetController assets(session, host, scheduler, bridge);
    assets.importFiles({directory.filePath("proxy.png"), audioPath});
    QElapsedTimer timer;
    timer.start();
    while ((assets.busy() || assets.proxyCacheBytes() == 0 || assets.decodedAudioBytes() == 0) &&
           timer.elapsed() < 15000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    check.expect(assets.proxyCacheBytes() > 0 && assets.decodedAudioBytes() > 0,
                 "real asset controller retains decoded proxy and audio bytes");
    // Give another participant almost all the configured weight, so tiny real fixtures can test
    // eviction without allocating gigabytes of audio. It owns no storage itself.
    auto& ledger = runtime::processMemoryBudgetLedger();
    const int competingPool = 0;
    ledger.registerCache(
        &competingPool, std::size_t{1} << 50U, [] { return 0; }, [](std::size_t) {});
    static_cast<void>(ledger.poll({.availableBytes = 0}));
    static_cast<void>(ledger.poll({.availableBytes = 0}));
    check.expect(assets.proxyCacheBytes() == 0 && assets.decodedAudioBytes() == 0,
                 "proxy and audio owners evict through the same ledger callbacks");
    for (const auto& asset : session.snapshot().project().assets())
        check.expect(assets.thumbnail(asset.id).isNull() && !assets.audioBuffer(asset.id),
                     "eviction also releases thumbnail aliases and audio lookup handles");
    ledger.unregisterCache(&competingPool);
    assets.cancel();
    bridge.beginShutdown();
    scheduler.beginShutdown();
    timer.restart();
    while (!scheduler.isQuiescent() && timer.elapsed() < 15000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    check.expect(scheduler.isQuiescent(),
                 "pressure cancellation leaves media tasks safe to shut down");
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    Expectations check;
    testTwoHundredUhdImportsStayInsideEveryBudget(check);
    testContentionDuringTwoHundredImports(check);
    testUnboundedAsyncQueueWouldHaveHeldGigabytes(check);
    testBadAllocOnInsertDropsTheEntryAndKeepsTheValue(check);
    testProxyCacheBoundIsMeasuredRatherThanAssumed(check);
    testDecodedAudioAggregateIsBounded(check);
    testAssetProxyAndAudioParticipation(check);
    return check.failures() == 0 ? 0 : 1;
}
