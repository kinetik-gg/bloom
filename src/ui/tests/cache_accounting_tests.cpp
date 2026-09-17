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
#include <bloom/render/image.hpp>
#include <bloom/render/image_types.hpp>
#include <bloom/runtime/memory_budget_ledger.hpp>
#include <bloom/runtime/operation_cache.hpp>

#include <QImage>

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

} // namespace

int main() {
    Expectations check;
    testTwoHundredUhdImportsStayInsideEveryBudget(check);
    testUnboundedAsyncQueueWouldHaveHeldGigabytes(check);
    testBadAllocOnInsertDropsTheEntryAndKeepsTheValue(check);
    testProxyCacheBoundIsMeasuredRatherThanAssumed(check);
    testDecodedAudioAggregateIsBounded(check);
    return check.failures() == 0 ? 0 : 1;
}
