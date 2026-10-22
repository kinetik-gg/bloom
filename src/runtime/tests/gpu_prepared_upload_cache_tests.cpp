#include "generated_jpeg.hpp"
#include "packed_host_retirement_private.hpp"
#include <bloom/media/image.hpp>
#include <filesystem>
#include <fstream>

#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/render/image.hpp>
#include <bloom/runtime/gpu_prepared_upload_cache.hpp>
#include <bloom/runtime/gpu_scene_coverage_cache.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <source_location>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using bloom::render::ImageWindow;
using bloom::render::Rgba32f;
using bloom::render::Rgba32fImage;
using bloom::render::Rgba32fImageBuilder;
using bloom::render::Rgba32fImageDescriptor;
using bloom::runtime::GpuPreparedUploadCache;
using bloom::runtime::GpuSceneCoverageCache;

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message,
                const std::source_location location = std::source_location::current()) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }
    [[nodiscard]] bool ok() const noexcept { return failures_ == 0; }

  private:
    std::size_t failures_ = 0;
};

[[nodiscard]] std::shared_ptr<const Rgba32fImage> makeImage(const std::uint32_t width,
                                                            const std::uint32_t height) {
    const auto window = ImageWindow::create(0, 0, width, height);
    if (!window) {
        throw std::logic_error("cache test image window");
    }
    const auto descriptor = Rgba32fImageDescriptor::create(*window.value(), *window.value(),
                                                           bloom::core::PixelAspectRatio::square());
    if (!descriptor) {
        throw std::logic_error("cache test image descriptor");
    }
    auto builder = Rgba32fImageBuilder::create(*descriptor.value(), 1U << 30U);
    if (!builder) {
        throw std::logic_error("cache test image builder");
    }
    auto frozen = std::move(*builder.value()).freeze();
    if (!frozen) {
        throw std::logic_error("cache test image freeze");
    }
    return std::make_shared<const Rgba32fImage>(std::move(*frozen.value()));
}

[[nodiscard]] std::uint64_t byteCount(const std::uint32_t width, const std::uint32_t height) {
    return static_cast<std::uint64_t>(width) * height * sizeof(Rgba32f);
}

void testHitMissAndZeroDisabled(Expectations& expectations) {
    GpuPreparedUploadCache cache(1U << 20U, 8);
    const auto small = makeImage(4, 4);
    const auto smallBytes = byteCount(4, 4);
    expectations.expect(cache.find("a") == nullptr && cache.misses() == 1,
                        "an absent key is a miss");
    cache.store("a", small);
    expectations.expect(cache.entryCount() == 1 && cache.retainedBytes() == smallBytes,
                        "a stored image is charged its actual pixel bytes");
    expectations.expect(cache.find("a").get() == small.get() && cache.hits() == 1,
                        "a stored key serves the same immutable allocation");
    cache.store("b", small);
    expectations.expect(cache.entryCount() == 2 && cache.retainedBytes() == 2 * smallBytes,
                        "two entries are charged twice");

    GpuPreparedUploadCache zeroBytes(0, 8);
    zeroBytes.store("a", small);
    expectations.expect(zeroBytes.entryCount() == 0 && zeroBytes.retainedBytes() == 0 &&
                            zeroBytes.find("a") == nullptr,
                        "a zero byte budget disables retention entirely");

    GpuPreparedUploadCache zeroEntries(1U << 20U, 0);
    zeroEntries.store("a", small);
    expectations.expect(zeroEntries.entryCount() == 0 && zeroEntries.retainedBytes() == 0,
                        "a zero entry ceiling disables retention entirely");
}

void testByteAndEntryCapsEvict(Expectations& expectations) {
    const auto image = makeImage(4, 4);
    const auto bytes = byteCount(4, 4);

    GpuPreparedUploadCache entryCap(1U << 20U, 2);
    entryCap.store("a", image);
    entryCap.store("b", image);
    entryCap.store("c", image);
    expectations.expect(entryCap.entryCount() == 2 && entryCap.retainedBytes() == 2 * bytes,
                        "the entry ceiling never overflows and bytes stay consistent");
    expectations.expect(entryCap.find("a") == nullptr && entryCap.find("b") != nullptr &&
                            entryCap.find("c") != nullptr,
                        "the least recently used entry is the one evicted");

    GpuPreparedUploadCache byteCap(2 * bytes, 8);
    byteCap.store("a", image);
    byteCap.store("b", image);
    byteCap.store("c", image);
    expectations.expect(byteCap.retainedBytes() <= byteCap.maxBytes() && byteCap.entryCount() <= 2,
                        "the byte ceiling is never exceeded");
    expectations.expect(byteCap.find("c") != nullptr, "the newest entry survives a byte eviction");
}

void testReplacementGrowthEvictsOthers(Expectations& expectations) {
    const auto small = makeImage(4, 4);
    const auto large = makeImage(4, 12);
    const auto bytes = byteCount(4, 4);
    const auto largeBytes = byteCount(4, 12);
    GpuPreparedUploadCache cache(4 * bytes, 8);
    cache.store("a", small);
    cache.store("b", small);
    cache.store("c", small);
    expectations.expect(cache.retainedBytes() == 3 * bytes, "three small entries are retained");

    // Replacing "a" with a buffer large enough to require eviction exercises both the replacement
    // erase and the eviction loop: the map entry must be erased before its backing order node.
    cache.store("a", large);
    expectations.expect(cache.retainedBytes() <= cache.maxBytes(),
                        "replacement growth stays within the byte ceiling");
    expectations.expect(cache.entryCount() == 2,
                        "replacement growth evicts until the new size fits");
    expectations.expect(cache.find("a").get() == large.get(),
                        "the replaced key serves the new image");
    expectations.expect(cache.find("b") == nullptr, "the least recently used victim was evicted");
    expectations.expect(cache.retainedBytes() == largeBytes + bytes,
                        "retained bytes equal the two remaining entries");
}

void testOversizedStoreRefusedKeepsExisting(Expectations& expectations) {
    const auto small = makeImage(4, 4);
    const auto huge = makeImage(64, 64);
    GpuPreparedUploadCache cache(byteCount(4, 4), 8);
    cache.store("a", small);
    cache.store("huge", huge);
    expectations.expect(
        cache.entryCount() == 1 && cache.retainedBytes() == byteCount(4, 4),
        "an image larger than the whole budget is refused, keeping existing entries");
    expectations.expect(cache.find("a").get() == small.get() && cache.find("huge") == nullptr,
                        "the refused oversized key never enters the cache");
}

void testAliasedAllocationIsChargedPerEntry(Expectations& expectations) {
    const auto shared = makeImage(4, 4);
    const auto bytes = byteCount(4, 4);
    GpuPreparedUploadCache cache(1U << 20U, 8);
    cache.store("x", shared);
    cache.store("y", shared);
    expectations.expect(cache.entryCount() == 2 && cache.retainedBytes() == 2 * bytes,
                        "a keyed cache charges aliased allocations per entry, never deduplicated");
    expectations.expect(cache.find("x").get() == shared.get() &&
                            cache.find("y").get() == shared.get(),
                        "both keys serve the shared allocation");
}

void testRepeatedReplacementAndFindStaysConsistent(Expectations& expectations) {
    const auto small = makeImage(4, 4);
    const auto large = makeImage(4, 12);
    GpuPreparedUploadCache cache(4 * byteCount(4, 4), 4);
    for (int round = 0; round < 32; ++round) {
        cache.store("k" + std::to_string(round % 3), (round % 2 == 0) ? small : large);
        cache.store("stable", small);
        (void)cache.find("stable");
        (void)cache.find("k0");
        expectations.expect(cache.retainedBytes() <= cache.maxBytes() &&
                                cache.entryCount() <= cache.maxEntries(),
                            "repeated replacement never exceeds either bound");
    }
}

struct RetirementScope final {
    std::shared_ptr<bloom::render::PackedHostRetirementOwner> owner =
        bloom::render::PackedHostRetirementOwner::create();
    std::mutex mutex;
    std::condition_variable wake;
    bool blocked = false;
    bool entered = false;
    std::thread::id freeThread;
    RetirementScope() {
        auto state = owner->producerState();
        state->testContext = this;
        state->beforeFree = [](void* context) noexcept {
            auto& self = *static_cast<RetirementScope*>(context);
            std::unique_lock lock(self.mutex);
            self.entered = true;
            self.freeThread = std::this_thread::get_id();
            self.wake.notify_all();
            self.wake.wait(lock, [&] { return !self.blocked; });
        };
    }
    void release() {
        {
            const std::lock_guard lock(mutex);
            blocked = false;
        }
        wake.notify_all();
    }
    ~RetirementScope() {
        release();
        owner->beginShutdown();
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!owner->exitAcknowledged() && std::chrono::steady_clock::now() < until)
            std::this_thread::yield();
        if (!owner->exitAcknowledged() || !owner->collect())
            std::terminate();
    }
};

template <class Predicate> bool until(Predicate predicate) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!predicate() && std::chrono::steady_clock::now() < end)
        std::this_thread::yield();
    return predicate();
}

void testOrdinaryGraceIsPreserved(Expectations& checks) {
    bloom::runtime::MemoryBudgetLedger ledger(64ULL << 30U, 48ULL << 30U);
    bloom::runtime::OperationCache pool(1U << 20U, ledger);
    const bloom::runtime::OperationCacheValue value{
        .image = {}, .values = {bloom::runtime::CompiledValue{0.5}}, .bounds = {}};
    pool.store("decoded", {}, value, bloom::runtime::OperationCacheEntryKind::DecodedMedia);
    pool.setByteBudget(pool.retainedBytes());
    pool.store("operation", {}, value);
    checks.expect(pool.find("decoded", {}).has_value() && !pool.find("operation", {}),
                  "ordinary-only incoming operation preserves an existing decoded grace entry");
}

void testSharedNamespaceAndPolicy(Expectations& checks) {
    RetirementScope scope;
    bloom::runtime::MemoryBudgetLedger ledger(64ULL << 30U, 48ULL << 30U);
    auto pool = std::make_shared<bloom::runtime::OperationCache>(1U << 20U, ledger);
    const auto assignment = pool->byteBudget();
    GpuPreparedUploadCache prepared(pool, scope.owner, 4096, 2);
    GpuPreparedUploadCache attached(pool);
    checks.expect(!scope.owner->snapshot().started,
                  "binding on the composition root starts no worker");
    checks.expect(pool->byteBudget() == assignment && attached.maxBytes() == 4096 &&
                      attached.maxEntries() == 2,
                  "default attachment preserves subset and adds no ledger participant");
    auto image = makeImage(4, 4);
    pool->store("same", {}, {.image = image, .values = {}, .bounds = {}});
    prepared.store("same", image);
    checks.expect(pool->find("same", {}).has_value() && prepared.find("same") == image,
                  "identical content is isolated by prepared namespace");
    checks.expect(
        pool->retainedBytes() ==
                pool->retainedBytes(bloom::runtime::OperationCacheEntryKind::Operation) +
                    pool->retainedBytes(bloom::runtime::OperationCacheEntryKind::PreparedUpload) &&
            pool->retainedBytes(bloom::runtime::OperationCacheEntryKind::PreparedUpload) >
                prepared.retainedBytes(),
        "one combined pool charges prepared payload and key metadata exactly once");
    bool rejected = false;
    try {
        GpuPreparedUploadCache incompatible(pool, scope.owner, 0);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    checks.expect(rejected && attached.maxBytes() == 4096,
                  "incompatible explicit zero cannot mutate another facade");
    rejected = false;
    try {
        GpuPreparedUploadCache incompatible(pool, {}, {}, 4096);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    checks.expect(rejected, "explicit legacy-valued count remains an explicit incompatible cap");
    const auto foreign = bloom::render::PackedHostRetirementOwner::create();
    rejected = false;
    try {
        GpuPreparedUploadCache incompatible(pool, foreign);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    checks.expect(rejected, "another owner cannot rebind one neutral pool");
    pool->clearOperations();
    checks.expect(!pool->find("same", {}) && prepared.find("same") == image,
                  "operation purge preserves prepared media");
    pool->clearDecodedMedia();
    checks.expect(prepared.find("same") == nullptr &&
                      until([&] { return prepared.retainedBytes() == 0; }),
                  "media purge includes prepared entries and actual worker release");
    GpuPreparedUploadCache zero(std::make_shared<bloom::runtime::OperationCache>(1U << 20U, ledger),
                                scope.owner, 0);
    zero.store("none", image);
    checks.expect(zero.entryCount() == 0 && zero.retainedBytes() == 0,
                  "explicit zero disables optional retention");
}

void testSourceRevisionWorkerRetirement(Expectations& checks) {
    RetirementScope scope;
    scope.blocked = true;
    bloom::runtime::MemoryBudgetLedger ledger(64ULL << 30U, 48ULL << 30U);
    auto pool = std::make_shared<bloom::runtime::OperationCache>(1U << 20U, ledger);
    GpuPreparedUploadCache prepared(pool, scope.owner);
    const auto path =
        std::filesystem::temp_directory_path() /
        ("bloom-source-ticket-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".jpg");
    {
        std::ofstream file(path, std::ios::binary);
        file.write(reinterpret_cast<const char*>(kGeneratedJpeg.data()), kGeneratedJpeg.size());
    }
    auto captured = bloom::media::captureImageSourceRevision(path, 1U << 20U);
    std::filesystem::remove(path);
    if (!captured.value.has_value())
        throw std::runtime_error("source revision retirement fixture capture");
    const std::weak_ptr<const bloom::media::ImageSourceRevision> backing = *captured.value;
    bool wrongKindRejected = false;
    try {
        pool->store("default-operation-source", {},
                    {.image = {}, .values = {}, .bounds = {}, .sourceRevision = *captured.value});
    } catch (const std::invalid_argument&) {
        wrongKindRejected = true;
    }
    pool->clearOperations();
    checks.expect(wrongKindRejected && pool->retainedBytes() == 0 &&
                      pool->retainedBytes(bloom::runtime::OperationCacheEntryKind::DecodedMedia) ==
                          0 &&
                      scope.owner->snapshot().allocations == 0,
                  "source revisions reject default Operation kind before registration/accounting");

    pool->store(
        "source", {}, {.image = {}, .values = {}, .bounds = {}, .sourceRevision = *captured.value},
        bloom::runtime::OperationCacheEntryKind::DecodedMedia, pool->decodedMediaGeneration());
    captured.value.reset();
    const auto charged = pool->retainedBytes();
    checks.expect(charged > kGeneratedJpeg.size() && scope.owner->snapshot().allocations == 1,
                  "rooted source ticket charges encoded backing/metadata and host registration");
    pool->setByteBudget(charged);
    const auto releasingThread = std::this_thread::get_id();
    pool->invalidateSourceRevisions();
    checks.expect(until([&] {
                      const std::lock_guard lock(scope.mutex);
                      return scope.entered;
                  }),
                  "source ticket reaches existing worker free gate");
    {
        const std::lock_guard lock(scope.mutex);
        checks.expect(scope.freeThread != releasingThread,
                      "source backing release runs off the purge/sampling caller thread");
    }
    checks.expect(
        !pool->find("source", {}) && !backing.expired() && pool->retainedBytes() == charged &&
            pool->retainedBytes(bloom::runtime::OperationCacheEntryKind::DecodedMedia) == charged,
        "detached source remains charged and physically alive until worker free");
    pool->store("new-operation", {}, {.image = makeImage(4, 4), .values = {}, .bounds = {}});
    checks.expect(!pool->find("new-operation", {}) && pool->retainedBytes() == charged,
                  "pending source bytes cannot donate reusable admission capacity");
    pool->closePreparedUploads();
    scope.owner->beginShutdown();
    checks.expect(!scope.owner->exitAcknowledged(),
                  "source registration prevents shutdown exit while free is held");
    scope.release();
    checks.expect(until([&] { return backing.expired() && pool->retainedBytes() == 0; }),
                  "actual encoded backing free precedes source charge foldback");
}

void testHeldRetirementConservation(Expectations& checks) {
    RetirementScope scope;
    scope.blocked = true;
    bloom::runtime::MemoryBudgetLedger ledger(64ULL << 30U, 48ULL << 30U);
    auto pool = std::make_shared<bloom::runtime::OperationCache>(1U << 20U, ledger);
    GpuPreparedUploadCache prepared(pool, scope.owner, 256, 1);
    prepared.store("old", makeImage(4, 4));
    const auto charged = pool->retainedBytes();
    const auto external = prepared.find("old");
    prepared.clear();
    checks.expect(until([&] {
                      const std::lock_guard lock(scope.mutex);
                      return scope.entered;
                  }),
                  "existing host worker reaches actual prepared-reference release gate");
    checks.expect(prepared.entryCount() == 0 && prepared.retainedBytes() == 256 &&
                      pool->retainedBytes() == charged && scope.owner->snapshot().allocations == 1,
                  "detached pending payload metadata and registration remain charged");
    for (int i = 0; i < 32; ++i)
        prepared.store("new", makeImage(4, 4));
    checks.expect(prepared.entryCount() == 0 && prepared.find("new") == nullptr &&
                      pool->retainedBytes() == 2 * charged && prepared.retainedBytes() == 512 &&
                      scope.owner->snapshot().allocations == 2,
                  "one candidate remains fully charged; repeated stores cannot queue more");
    scope.owner->beginShutdown();
    checks.expect(!scope.owner->exitAcknowledged(),
                  "common owner cannot exit while charged reference release is held");
    scope.release();
    checks.expect(until([&] { return prepared.find("new") != nullptr; }) &&
                      prepared.entryCount() == 1 && prepared.retainedBytes() == 256 &&
                      pool->retainedBytes() == charged && external->pixels().size() == 16,
                  "actual old-reference release promotes the captured image without another store");
    prepared.store("closed", makeImage(4, 4));
    checks.expect(prepared.find("closed") == nullptr && prepared.entryCount() == 1,
                  "closed owner refuses optional stores without reviving a lane");
    prepared.clear();
    checks.expect(until([&] { return pool->retainedBytes() == 0; }),
                  "late registered candidate retires through the closing owner");
}

void testCandidatePressureAndClose(Expectations& checks) {
    RetirementScope scope;
    scope.blocked = true;
    bloom::runtime::MemoryBudgetLedger ledger(64ULL << 30U, 48ULL << 30U);
    auto pool = std::make_shared<bloom::runtime::OperationCache>(1U << 20U, ledger);
    GpuPreparedUploadCache prepared(pool, scope.owner, 256, 1);
    const auto oldImage = makeImage(4, 4);
    const auto newImage = makeImage(4, 4);
    prepared.store("old", oldImage);
    const auto charge = pool->retainedBytes();
    prepared.store("new", newImage);
    checks.expect(until([&] {
                      const std::lock_guard lock(scope.mutex);
                      return scope.entered;
                  }),
                  "replacement reaches held host retirement");
    checks.expect(prepared.find("new") == nullptr && prepared.entryCount() == 0 &&
                      prepared.retainedBytes() == 512 && pool->retainedBytes() == 2 * charge,
                  "held old and candidate references both remain charged before retirement");
    pool->setByteBudget(0);
    checks.expect(prepared.find("new") == nullptr && prepared.entryCount() == 0 &&
                      pool->retainedBytes() == 2 * charge &&
                      scope.owner->snapshot().allocations == 2,
                  "pressure cancels candidate without borrowing pending retirement credit");
    pool->setByteBudget(charge);
    for (int i = 0; i < 16; ++i)
        prepared.store("later", makeImage(4, 4));
    checks.expect(prepared.find("later") == nullptr && pool->retainedBytes() == 2 * charge &&
                      scope.owner->snapshot().allocations == 2,
                  "retiring candidate keeps its one-slot bound until actual worker release");
    pool->closePreparedUploads();
    scope.owner->beginShutdown();
    checks.expect(!scope.owner->exitAcknowledged(),
                  "close cannot acknowledge while held old and cancelled candidate remain live");
    scope.release();
    checks.expect(until([&] { return pool->retainedBytes() == 0; }) &&
                      prepared.retainedBytes() == 0 && prepared.entryCount() == 0,
                  "both exact charges return only after worker-side release");
}

void testOrdinaryPayloadWorkerRetirement(Expectations& checks) {
    RetirementScope scope;
    scope.blocked = true;
    bloom::runtime::MemoryBudgetLedger ledger(64ULL << 30U, 48ULL << 30U);
    auto pool = std::make_shared<bloom::runtime::OperationCache>(1U << 20U, ledger);
    GpuPreparedUploadCache prepared(pool, scope.owner);
    auto image = makeImage(64, 64);
    const std::weak_ptr<const Rgba32fImage> backing = image;
    pool->store("ordinary", {}, {.image = image, .values = {}, .bounds = {}});
    image.reset();
    const auto ordinaryCharge = pool->retainedBytes();
    auto decoded = makeImage(64, 64);
    const std::weak_ptr<const Rgba32fImage> decodedBacking = decoded;
    pool->store("decoded", {}, {.image = decoded, .values = {}, .bounds = {}},
                bloom::runtime::OperationCacheEntryKind::DecodedMedia);
    decoded.reset();
    const auto totalCharge = pool->retainedBytes();
    const auto decodedCharge = totalCharge - ordinaryCharge;
    const auto caller = std::this_thread::get_id();
    pool->clearOperations();
    checks.expect(until([&] {
                      const std::lock_guard lock(scope.mutex);
                      return scope.entered;
                  }),
                  "ordinary CPU image reaches existing host worker free gate");
    {
        const std::lock_guard lock(scope.mutex);
        checks.expect(scope.freeThread != caller,
                      "ordinary image cache reference is released off the purge caller");
    }
    pool->clearDecodedMedia();
    checks.expect(!pool->find("ordinary", {}) && !pool->find("decoded", {}) && !backing.expired() &&
                      !decodedBacking.expired() && pool->retainedBytes() == totalCharge &&
                      pool->retainedBytes(bloom::runtime::OperationCacheEntryKind::DecodedMedia) ==
                          decodedCharge &&
                      scope.owner->snapshot().allocations == 2,
                  "ordinary and decoded misses leave both physical refs and exact charges pending");
    pool->closePreparedUploads();
    scope.owner->beginShutdown();
    checks.expect(!scope.owner->exitAcknowledged(),
                  "ordinary registration prevents early host-owner exit");
    scope.release();
    checks.expect(until([&] {
                      return backing.expired() && decodedBacking.expired() &&
                             pool->retainedBytes() == 0;
                  }),
                  "ordinary and decoded image frees precede worker charge foldback");
}

void testProjectedVictimsPreserveWarmEntries(Expectations& checks) {
    RetirementScope scope;
    scope.blocked = true;
    bloom::runtime::MemoryBudgetLedger ledger(64ULL << 30U, 48ULL << 30U);
    auto pool = std::make_shared<bloom::runtime::OperationCache>(1U << 20U, ledger);
    GpuPreparedUploadCache prepared(pool, scope.owner, 3U * 256U, 3);
    const auto image = makeImage(4, 4);
    prepared.store("a", image);
    prepared.store("b", image);
    prepared.store("c", image);
    pool->store("operation", {}, {.image = image, .values = {}, .bounds = {}});
    const auto total = pool->retainedBytes();
    const auto preparedCharge =
        pool->retainedBytes(bloom::runtime::OperationCacheEntryKind::PreparedUpload);
    prepared.store("d", image);
    checks.expect(until([&] {
                      const std::lock_guard lock(scope.mutex);
                      return scope.entered;
                  }),
                  "subset replacement queues its one LRU victim on actual worker");
    for (int i = 0; i < 16; ++i)
        prepared.store("d", image);
    checks.expect(prepared.find("a") == nullptr && prepared.find("b") == image &&
                      prepared.find("c") == image && prepared.find("d") == nullptr &&
                      pool->find("operation", {}).has_value() &&
                      pool->retainedBytes() == total + preparedCharge / 3,
                  "one charged candidate selects no collateral prepared or ordinary victims");
    pool->trimToBytes(total - preparedCharge / 3);
    checks.expect(prepared.find("b") == image && prepared.find("c") == image &&
                      pool->find("operation", {}).has_value(),
                  "projected global trim recognizes sufficient already-pending victim");
    scope.release();
    checks.expect(until([&] { return prepared.retainedBytes() == 512; }),
                  "only actual worker ref release returns the one victim payload charge");
    prepared.store("d", image);
    checks.expect(prepared.find("d") == image && prepared.find("b") == image &&
                      prepared.find("c") == image,
                  "released subset occupancy admits newest without further eviction");
    prepared.clear();
}

void testSharedDefaultIsMemoryLimited(Expectations& checks) {
    RetirementScope scope;
    bloom::runtime::MemoryBudgetLedger ledger(64ULL << 30U, 48ULL << 30U);
    auto pool = std::make_shared<bloom::runtime::OperationCache>(8U << 20U, ledger);
    GpuPreparedUploadCache prepared(pool, scope.owner);
    const auto image = makeImage(1, 1);
    for (std::size_t i = 0; i < 4100; ++i)
        prepared.store("source-" + std::to_string(i), image);
    checks.expect(
        prepared.entryCount() == 4100 && prepared.retainedBytes() == 4100ULL * 16ULL &&
            pool->retainedBytes() > prepared.retainedBytes() &&
            pool->retainedBytes() <= pool->byteBudget() &&
            prepared.maxEntries() == std::numeric_limits<std::size_t>::max(),
        "omitted shared count admits over4096 keys within payload-plus-metadata allowance");
    const auto charge = pool->retainedBytes();
    pool->setByteBudget(charge);
    prepared.store("extra", image);
    checks.expect(pool->retainedBytes() <= charge + charge / 4100 && prepared.entryCount() <= 4100,
                  "one pending candidate is charged in addition to the bounded live set");
    prepared.clear();
    checks.expect(until([&] { return pool->retainedBytes() == 0; }) &&
                      prepared.retainedBytes() == 0 &&
                      until([&] { return scope.owner->snapshot().allocations == 0; }),
                  "all over4096 per-key charges and registrations actually retire");
}

void testAccountingOutlivesPool(Expectations& checks) {
    RetirementScope scope;
    scope.blocked = true;
    bloom::runtime::MemoryBudgetLedger ledger(64ULL << 30U, 48ULL << 30U);
    std::weak_ptr<bloom::runtime::OperationCache> weakPool;
    std::weak_ptr<const Rgba32fImage> weakImage;
    {
        auto pool = std::make_shared<bloom::runtime::OperationCache>(1U << 20U, ledger);
        weakPool = pool;
        GpuPreparedUploadCache prepared(pool, scope.owner);
        auto image = makeImage(4, 4);
        weakImage = image;
        prepared.store("sole", image);
        image.reset();
    } // Last pool destruction itself enqueues the still-active entry without waiting.
    checks.expect(until([&] {
                      const std::lock_guard lock(scope.mutex);
                      return scope.entered;
                  }),
                  "last pool destruction enqueues reference release on the existing worker");
    checks.expect(
        weakPool.expired() && !weakImage.expired() && scope.owner->snapshot().allocations == 1,
        "retiring node owns neutral accounting without pinning the cache or freeing backing");
    scope.owner->beginShutdown();
    checks.expect(!scope.owner->exitAcknowledged(),
                  "cache destruction cannot falsely acknowledge the shared owner");
    scope.release();
    checks.expect(
        until([&] { return scope.owner->exitAcknowledged(); }) && weakImage.expired() &&
            scope.owner->snapshot().allocations == 0,
        "callback safely frees refs and neutral charge after pool destruction, then exits");
}

void testIdenticalConcurrentStorePreservesWarmEntry(Expectations& checks) {
    RetirementScope scope;
    scope.blocked = true;
    bloom::runtime::MemoryBudgetLedger ledger(64ULL << 30U, 48ULL << 30U);
    auto pool = std::make_shared<bloom::runtime::OperationCache>(1U << 20U, ledger);
    GpuPreparedUploadCache prepared(pool, scope.owner, 256, 1);
    const auto image = makeImage(4, 4);
    prepared.store("stable", image);
    const auto charge = pool->retainedBytes();
    std::vector<std::thread> writers;
    writers.reserve(4);
    for (int i = 0; i < 4; ++i)
        writers.emplace_back([&] {
            for (int j = 0; j < 16; ++j)
                prepared.store("stable", image);
        });
    for (auto& writer : writers)
        writer.join();
    checks.expect(
        prepared.find("stable") == image && prepared.entryCount() == 1 &&
            prepared.retainedBytes() == 256 && pool->retainedBytes() == charge &&
            scope.owner->snapshot().queuedAllocations == 0 &&
            scope.owner->snapshot().allocations == 1,
        "identical concurrent stores preserve warm allocation without queued replacements");
    prepared.clear();
    scope.release();
}

void testClosedPoolStaysRooted(Expectations& checks) {
    RetirementScope scope;
    bloom::runtime::MemoryBudgetLedger ledger(64ULL << 30U, 48ULL << 30U);
    auto pool = std::make_shared<bloom::runtime::OperationCache>(1U << 20U, ledger);
    GpuPreparedUploadCache prepared(pool, scope.owner);
    const auto image = makeImage(4, 4);
    prepared.store("held", image);
    pool->closePreparedUploads();
    prepared.store("held", image);
    prepared.store("later", image);
    GpuPreparedUploadCache attached(pool);
    checks.expect(pool->preparedUploadsBound() && !attached.find("held") &&
                      !attached.find("later") && until([&] { return pool->retainedBytes() == 0; }),
                  "closed pool preserves root binding and rejects stores/default reopening");
}

void testDeadlineExpiresWithoutDelivery(Expectations& checks) {
    RetirementScope scope;
    bloom::runtime::MemoryBudgetLedger ledger(64ULL << 30U, 48ULL << 30U);
    ledger.beginProcessSampling();
    auto pool = std::make_shared<bloom::runtime::OperationCache>(1U << 20U, ledger);
    GpuPreparedUploadCache prepared(pool, scope.owner);
    bloom::runtime::MemoryBudgetSample sample;
    sample.machine.availableBytes = 48ULL << 30U;
    sample.physicalBytes = 64ULL << 30U;
    sample.process.source = bloom::runtime::ProcessMemorySource::OtherHostOnly;
    sample.process.status = bloom::runtime::ProcessMemoryStatus::Unsupported;
    sample.process.sampledAt = std::chrono::steady_clock::now() - std::chrono::seconds(15) +
                               std::chrono::milliseconds(100);
    static_cast<void>(ledger.applySample(sample));
    const auto image = makeImage(4, 4);
    prepared.store("held", image);
    checks.expect(prepared.find("held") == image,
                  "delivered unexpired sample admits initial store");
    std::this_thread::sleep_until(sample.process.sampledAt + std::chrono::seconds(15) +
                                  std::chrono::milliseconds(1));
    prepared.store("expired", image);
    checks.expect(prepared.find("expired") == nullptr && prepared.find("held") == image,
                  "new stores check delivered expiry even without another ledger callback");
    prepared.clear();
}

void testSameLedgerFreshnessAndShrink(Expectations& checks) {
    RetirementScope scope;
    bloom::runtime::MemoryBudgetLedger ledger(64ULL << 30U, 48ULL << 30U);
    ledger.beginProcessSampling();
    auto pool = std::make_shared<bloom::runtime::OperationCache>(1U << 20U, ledger);
    GpuPreparedUploadCache prepared(pool, scope.owner);
    const auto image = makeImage(4, 4);
    prepared.store("cold", image);
    checks.expect(prepared.entryCount() == 0 && pool->byteBudget() == 0,
                  "rooted cold delivery has no retention authority");
    bloom::runtime::MemoryBudgetSample sample;
    sample.machine.availableBytes = 48ULL << 30U;
    sample.physicalBytes = 64ULL << 30U;
    sample.process.source = bloom::runtime::ProcessMemorySource::OtherHostOnly;
    sample.process.status = bloom::runtime::ProcessMemoryStatus::Unsupported;
    sample.process.sampledAt = std::chrono::steady_clock::now();
    static_cast<void>(ledger.applySample(sample));
    const auto assigned = pool->byteBudget();
    prepared.store("fresh", image);
    checks.expect(assigned > 256 && prepared.find("fresh") == image,
                  "actual same-ledger fresh retention admits prepared payload and metadata");
    sample.process.sampledAt -= std::chrono::seconds(20);
    static_cast<void>(ledger.applySample(sample));
    prepared.store("stale", image);
    checks.expect(prepared.find("stale") == nullptr && prepared.find("fresh") == image,
                  "stale backward delivery denies new store while validated hits remain usable");
    sample.process.sampledAt = std::chrono::steady_clock::now();
    sample.process.status = bloom::runtime::ProcessMemoryStatus::Unavailable;
    static_cast<void>(ledger.applySample(sample));
    prepared.store("failed", image);
    checks.expect(prepared.find("failed") == nullptr,
                  "fresh timestamp on failed delivery does not grant new retention");
    sample.process.status = bloom::runtime::ProcessMemoryStatus::Unsupported;
    sample.process.sampledAt = std::chrono::steady_clock::now();
    static_cast<void>(ledger.applySample(sample));
    prepared.store("recovered", image);
    checks.expect(prepared.find("recovered") == image,
                  "genuine fresh same-root recovery authorizes a new optional store");
    pool->setByteBudget(0);
    prepared.store("tightened", image);
    checks.expect(pool->byteBudget() == 0 && prepared.entryCount() == 0 &&
                      until([&] { return pool->retainedBytes() == 0; }),
                  "serialized zero shrink removes entries without waiting under ledger lock");
}

void testCoverageCacheEviction(Expectations& expectations) {
    // The coverage cache now retains immutable PathRasterCoverageGeometry (row ranges + spans), not
    // an R8 mask. Its accounting is the ACTUAL ranges + spans bytes.
    auto geometry = std::make_shared<bloom::runtime::GpuSceneCoverageGeometry>();
    geometry->width = 4;
    geometry->height = 4;
    geometry->rows.resize(std::size_t{4} * 4U);
    geometry->spans.resize(8U);
    const std::uint64_t bytes = bloom::runtime::gpuSceneCoverageGeometryBytes(*geometry);
    expectations.expect(bytes == 16U * sizeof(bloom::render::PathRasterCoverageRange) +
                                     8U * sizeof(bloom::render::PathRasterCoverageSpan),
                        "the charged coverage bytes are the actual row ranges + spans");

    auto cache = std::make_shared<GpuSceneCoverageCache>(2U * bytes);
    cache->store("a", geometry);
    cache->store("b", geometry);
    expectations.expect(cache->entryCount() == 2 && cache->retainedBytes() == 2U * bytes,
                        "two equal geometries are retained at the exact byte accounting");
    cache->store("c", geometry);
    expectations.expect(cache->retainedBytes() <= 2U * bytes && cache->entryCount() == 2,
                        "the coverage cache byte ceiling is never exceeded across eviction");
    expectations.expect(cache->find("c") != nullptr && cache->find("c").get() == geometry.get(),
                        "the most recent coverage entry serves the shared geometry identity");
    expectations.expect(cache->find("a") == nullptr && cache->find("b") != nullptr,
                        "the least-recently-used coverage entry was evicted");
    // Re-storing the same identity under an existing key replaces in place with no extra bytes.
    cache->store("c", geometry);
    expectations.expect(cache->retainedBytes() == 2U * bytes && cache->entryCount() == 2,
                        "replacing an existing key keeps the byte accounting unchanged");
    expectations.expect(cache->find("c").get() == geometry.get(),
                        "the replaced entry still serves the shared geometry identity");
}

} // namespace

int main() {
    try {
        Expectations expectations;
        testHitMissAndZeroDisabled(expectations);
        testByteAndEntryCapsEvict(expectations);
        testReplacementGrowthEvictsOthers(expectations);
        testOversizedStoreRefusedKeepsExisting(expectations);
        testAliasedAllocationIsChargedPerEntry(expectations);
        testRepeatedReplacementAndFindStaysConsistent(expectations);
        testCoverageCacheEviction(expectations);
        testSharedNamespaceAndPolicy(expectations);
        testOrdinaryGraceIsPreserved(expectations);
        testHeldRetirementConservation(expectations);
        testCandidatePressureAndClose(expectations);
        testOrdinaryPayloadWorkerRetirement(expectations);
        testSourceRevisionWorkerRetirement(expectations);
        testSameLedgerFreshnessAndShrink(expectations);
        testProjectedVictimsPreserveWarmEntries(expectations);
        testIdenticalConcurrentStorePreservesWarmEntry(expectations);
        testDeadlineExpiresWithoutDelivery(expectations);
        testClosedPoolStaysRooted(expectations);
        testAccountingOutlivesPool(expectations);
        testSharedDefaultIsMemoryLimited(expectations);
        if (!expectations.ok()) {
            std::cerr << "FAIL: GPU prepared upload cache expectations failed\n";
            return 1;
        }
        std::cout << "PASS: GPU prepared upload cache\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
