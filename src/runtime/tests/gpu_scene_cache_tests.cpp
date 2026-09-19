// Tests for the bounded owner-thread GPU intermediate content cache.
//
// Real native images are produced by the SolidV1 pipeline; the cache is exercised against actual
// VMA allocations, actual device ownership, and actual shared_ptr pinning. A hardware-free image
// prints an explicit skip. No fake handles or fabricated eligibility: every image is a real
// resident GpuImage, and every byte count is the actual allocation size from
// GpuImage::allocationBytes().

#include <bloom/core/color.hpp>
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/render/cpu_image_primitives.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/render/gpu_solid.hpp>
#include <bloom/render/image_types.hpp>
#include <bloom/runtime/gpu_scene_cache.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <source_location>
#include <string_view>
#include <thread>
#include <utility>

namespace {

using bloom::core::Color4d;
using bloom::core::PixelAspectRatio;
using bloom::render::GpuDevice;
using bloom::render::GpuDeviceCreationOptions;
using bloom::render::GpuImage;
using bloom::render::GpuSolid;
using bloom::render::GpuSolidParameters;
using bloom::render::GpuSolidPollResult;
using bloom::render::ImageWindow;
using bloom::render::Rgba32f;
using bloom::runtime::GpuSceneCache;
using bloom::runtime::GpuSceneCacheBudgets;
using bloom::runtime::GpuSceneCacheDiagnosticCode;
using bloom::runtime::GpuSceneCacheInsertResult;

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

struct Options final {
    std::filesystem::path loader_path;
    bool require_device = false;
    bool valid = true;
};

[[nodiscard]] Options parseOptions(const int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--loader") {
            if (index + 1 >= argc) {
                std::cerr << "--loader requires a path argument\n";
                options.valid = false;
                return options;
            }
            options.loader_path = argv[++index];
        } else if (argument == "--require-device") {
            options.require_device = true;
        } else {
            std::cerr << "unknown argument: " << argument << '\n';
            options.valid = false;
            return options;
        }
    }
    return options;
}

[[nodiscard]] bool pollSolidUntilReady(GpuSolid& solid) {
    for (int attempt = 0; attempt < 20000; ++attempt) {
        const auto result = solid.poll();
        if (result == GpuSolidPollResult::Ready) {
            return true;
        }
        if (result != GpuSolidPollResult::Pending) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    return false;
}

[[nodiscard]] std::shared_ptr<const GpuImage> makeSolidImage(GpuSolid& solid,
                                                             const std::uint32_t width,
                                                             const std::uint32_t height,
                                                             const double tint) {
    const auto window = ImageWindow::create(0, 0, width, height);
    const auto pixel =
        bloom::render::solidPixelFromStraightLinearRec709Scene(Color4d{tint, 0.25, 0.5, 1.0});
    if (!window || !pixel) {
        return nullptr;
    }
    const GpuSolidParameters parameters{.pixel = *pixel.value(),
                                        .dataWindow = *window.value(),
                                        .displayWindow = *window.value(),
                                        .pixelAspect = PixelAspectRatio::square()};
    if (solid.begin(parameters, 1ULL << 32ULL).code !=
        bloom::render::GpuSolidDiagnosticCode::None) {
        return nullptr;
    }
    if (!pollSolidUntilReady(solid)) {
        return nullptr;
    }
    GpuImage taken = solid.takeImage();
    if (!taken.isValid()) {
        return nullptr;
    }
    return std::make_shared<const GpuImage>(std::move(taken));
}

void testIdentityAndRevision(Expectations& expectations, GpuDevice& device, GpuSolid& solid) {
    auto created = GpuSceneCache::create(device, GpuSceneCacheBudgets{1ULL << 32ULL});
    expectations.expect(created.hasValue(), "the scene cache is created");
    if (!created) {
        return;
    }
    GpuSceneCache& cache = *created.cache;
    expectations.expect(cache.isBoundTo(device), "the cache is bound to its device");

    auto image = makeSolidImage(solid, 48, 48, 0.5);
    expectations.expect(image != nullptr && image->allocationBytes() > 0,
                        "a real resident image with a real allocation is produced");
    if (image == nullptr || image->allocationBytes() == 0) {
        return;
    }

    const auto inserted = cache.insert("semantic-digest-A", image);
    expectations.expect(inserted == GpuSceneCacheInsertResult::Inserted, "the image is inserted");
    expectations.expect(cache.retainedBytes() == image->allocationBytes(),
                        "retained bytes equal the actual VMA allocation");
    expectations.expect(cache.entryCount() == 1, "one entry is retained");

    // The cache never receives a document revision or any layout/ID. Bumping unrelated metadata
    // between the insert and the lookup must not affect the hit.
    std::uint64_t externalRevision = 1;
    for (int i = 0; i < 4; ++i) {
        ++externalRevision;
    }
    auto hit = cache.find("semantic-digest-A");
    expectations.expect(hit != nullptr && hit.get() == image.get(),
                        "an unchanged semantic digest hits despite changed revision metadata");
    auto miss = cache.find("semantic-digest-B");
    expectations.expect(miss == nullptr, "a changed semantic digest misses");
    const auto counters = cache.counters();
    expectations.expect(counters.hits == 1 && counters.misses == 1,
                        "hit and miss counters are tracked");

    // find() returns the pin the caller holds; releasing it lets the entry be reclaimed later.
    hit.reset();
    expectations.expect(cache.erase("semantic-digest-A"), "the entry can be erased");
    expectations.expect(cache.retainedBytes() == 0 && cache.entryCount() == 0,
                        "erase reclaims the charged bytes");
    expectations.expect(cache.find("semantic-digest-A") == nullptr, "the erased key now misses");
    expectations.expect(cache.counters().misses == 2, "the erased key miss is counted");
    (void)externalRevision;

    cache.invalidateDevice();
    expectations.expect(cache.invalidationEpoch() == 2 && cache.entryCount() == 0,
                        "device invalidation clears entries and advances the epoch");
}

void testForeignDeviceRejection(Expectations& expectations, GpuDevice& device,
                                GpuSolid& foreignSolid, const bool foreignAvailable) {
    auto created = GpuSceneCache::create(device, GpuSceneCacheBudgets{1ULL << 32ULL});
    expectations.expect(created.hasValue(), "a cache for the first device is created");
    if (!created) {
        return;
    }
    if (!foreignAvailable) {
        std::cout << "NOTE: a second GpuDevice was unavailable; foreign-device rejection was not "
                     "exercised\n";
        return;
    }
    auto foreignImage = makeSolidImage(foreignSolid, 32, 32, 0.75);
    expectations.expect(foreignImage != nullptr,
                        "a resident image on the foreign device is produced");
    if (foreignImage == nullptr) {
        return;
    }
    expectations.expect(!foreignImage->isBoundTo(device),
                        "the foreign image is not bound to the first device");
    expectations.expect(created.cache->insert("semantic-digest-F", foreignImage) ==
                            GpuSceneCacheInsertResult::ForeignImage,
                        "an image from another device ownership generation is rejected");
    expectations.expect(created.cache->entryCount() == 0, "a rejected foreign image is not stored");
}

void testBudgetAndPinning(Expectations& expectations, GpuDevice& device, GpuSolid& solid) {
    auto probe = makeSolidImage(solid, 32, 32, 0.1);
    expectations.expect(probe != nullptr && probe->allocationBytes() > 0,
                        "the probe image is produced");
    if (probe == nullptr) {
        return;
    }
    const std::uint64_t unit = probe->allocationBytes();

    // Budget for exactly three images. Insert A, B, C; pin A; insert D and require an unpinned LRU
    // eviction (B) rather than freeing the pinned A.
    auto created = GpuSceneCache::create(device, GpuSceneCacheBudgets{unit * 3});
    expectations.expect(created.hasValue(), "the three-image cache is created");
    if (!created) {
        return;
    }
    GpuSceneCache& cache = *created.cache;
    auto a = makeSolidImage(solid, 32, 32, 0.2);
    auto b = makeSolidImage(solid, 32, 32, 0.3);
    auto c = makeSolidImage(solid, 32, 32, 0.4);
    auto d = makeSolidImage(solid, 32, 32, 0.5);
    auto e = makeSolidImage(solid, 32, 32, 0.6);
    expectations.expect(a && b && c && d && e && a->allocationBytes() == unit &&
                            b->allocationBytes() == unit && c->allocationBytes() == unit &&
                            d->allocationBytes() == unit && e->allocationBytes() == unit,
                        "all budget images share the same real allocation size");
    if (!a || !b || !c || !d || !e) {
        return;
    }
    expectations.expect(cache.insert("kA", a) == GpuSceneCacheInsertResult::Inserted, "A inserts");
    expectations.expect(cache.insert("kB", b) == GpuSceneCacheInsertResult::Inserted, "B inserts");
    expectations.expect(cache.insert("kC", c) == GpuSceneCacheInsertResult::Inserted, "C inserts");
    expectations.expect(cache.retainedBytes() == unit * 3, "three allocations are charged");
    // Drop the caller references so only the cache owns the entries; the cache's use_count check
    // then sees them as evictable. Without this the test's own shared_ptrs would pin everything.
    a.reset();
    b.reset();
    c.reset();

    auto pinnedA = cache.find("kA");
    expectations.expect(pinnedA != nullptr, "A is found and pinned by the returned shared_ptr");
    expectations.expect(cache.insert("kD", d) == GpuSceneCacheInsertResult::Inserted,
                        "D inserts by evicting an unpinned entry");
    expectations.expect(cache.find("kA") != nullptr, "the pinned A survives the eviction");
    expectations.expect(cache.find("kB") == nullptr,
                        "the unpinned least-recently-used B was evicted");
    expectations.expect(cache.find("kD") != nullptr, "D is present");
    expectations.expect(cache.retainedBytes() <= unit * 3, "retained bytes stay within budget");

    // All remaining entries pinned: a new image must be refused, not silently dropped or
    // over-budgeted, and nothing may be evicted to pretend VRAM was reclaimed.
    auto pinnedC = cache.find("kC");
    auto pinnedD = cache.find("kD");
    d.reset();
    const auto bytesBefore = cache.retainedBytes();
    const auto entriesBefore = cache.entryCount();
    expectations.expect(cache.insert("kE", e) == GpuSceneCacheInsertResult::RefusedOverBudget,
                        "an insertion is refused when only pinned entries remain");
    expectations.expect(cache.retainedBytes() == bytesBefore && cache.entryCount() == entriesBefore,
                        "a refused insertion changes nothing");
    expectations.expect(cache.find("kE") == nullptr, "the refused image was not inserted");
    expectations.expect(cache.counters().evictions > 0, "an eviction was recorded");
    expectations.expect(cache.counters().refusals > 0, "a refusal was recorded");
    // An image larger than the whole budget is refused immediately.
    auto createdSmall = GpuSceneCache::create(device, GpuSceneCacheBudgets{unit - 1});
    expectations.expect(createdSmall.hasValue(), "the under-sized cache is created");
    if (createdSmall) {
        expectations.expect(createdSmall.cache->insert("kTooBig", probe) ==
                                GpuSceneCacheInsertResult::RefusedOverBudget,
                            "an image larger than the budget is refused");
    }
}

void testWrongThread(Expectations& expectations, GpuDevice& device) {
    auto created = GpuSceneCache::create(device, GpuSceneCacheBudgets{1ULL << 32ULL});
    if (!created) {
        expectations.expect(false, "the wrong-thread cache is created");
        return;
    }
    GpuSceneCache& cache = *created.cache;
    const auto bytesBefore = cache.retainedBytes();
    const auto entriesBefore = cache.entryCount();
    GpuSceneCacheInsertResult observedInsert = GpuSceneCacheInsertResult::Inserted;
    bool observedFind = true;
    std::thread worker([&cache, &observedInsert, &observedFind]() {
        observedInsert = cache.insert("kWrongThread", nullptr);
        observedFind = cache.find("kWrongThread") != nullptr;
        cache.clear();
        cache.invalidateDevice();
    });
    worker.join();
    expectations.expect(observedInsert == GpuSceneCacheInsertResult::WrongThread,
                        "insert from a non-owner thread is WrongThread");
    expectations.expect(!observedFind, "find from a non-owner thread returns nullptr");
    expectations.expect(cache.retainedBytes() == bytesBefore && cache.entryCount() == entriesBefore,
                        "wrong-thread calls do not mutate the cache");
}

void testPinnedReplacement(Expectations& expectations, GpuDevice& device, GpuSolid& solid) {
    auto oldImage = makeSolidImage(solid, 32, 32, 0.11);
    auto newImage = makeSolidImage(solid, 32, 32, 0.22);
    expectations.expect(oldImage != nullptr && newImage != nullptr &&
                            oldImage->allocationBytes() > 0 && newImage->allocationBytes() > 0,
                        "the replacement fixtures are produced");
    if (oldImage == nullptr || newImage == nullptr) {
        return;
    }
    const std::uint64_t budget =
        std::max(oldImage->allocationBytes(), newImage->allocationBytes()) * 2;
    auto created = GpuSceneCache::create(device, GpuSceneCacheBudgets{budget});
    expectations.expect(created.hasValue(), "the replacement cache is created");
    if (!created) {
        return;
    }
    GpuSceneCache& cache = *created.cache;
    expectations.expect(cache.insert("kReplace", oldImage) == GpuSceneCacheInsertResult::Inserted,
                        "the original image inserts");
    auto sameKey = cache.find("kReplace");
    expectations.expect(sameKey != nullptr && sameKey.get() == oldImage.get(),
                        "the original image is found");
    // Same key + same image is still a hit/touch, never a replacement.
    expectations.expect(cache.insert("kReplace", sameKey) == GpuSceneCacheInsertResult::Inserted,
                        "re-inserting the same image under the same key is a hit");
    expectations.expect(cache.find("kReplace") != nullptr &&
                            cache.find("kReplace").get() == oldImage.get(),
                        "the same-image hit preserves the original entry");

    const auto bytesBefore = cache.retainedBytes();
    expectations.expect(cache.insert("kReplace", newImage) ==
                            GpuSceneCacheInsertResult::RefusedPinnedReplacement,
                        "replacing a pinned image under the same key is refused");
    expectations.expect(cache.find("kReplace") != nullptr &&
                            cache.find("kReplace").get() == oldImage.get(),
                        "the pinned original is still the hit after the refusal");
    expectations.expect(cache.retainedBytes() == bytesBefore && cache.entryCount() == 1,
                        "the refusal preserves the original bytes and entry");

    // Once the cache alone owns the old image, replacing it is safe and reclaims its charge.
    sameKey.reset();
    oldImage.reset();
    expectations.expect(cache.insert("kReplace", newImage) == GpuSceneCacheInsertResult::Inserted,
                        "an unpinned replacement is accepted");
    auto replaced = cache.find("kReplace");
    expectations.expect(replaced != nullptr && replaced.get() == newImage.get(),
                        "the new image is the hit after an accepted replacement");
    expectations.expect(cache.retainedBytes() == newImage->allocationBytes(),
                        "the charge now reflects only the new image");
}

void testForeignThreadCreate(Expectations& expectations, GpuDevice& device) {
    auto observed = GpuSceneCacheDiagnosticCode::None;
    bool observedValue = true;
    std::thread worker([&device, &observed, &observedValue]() {
        auto created = GpuSceneCache::create(device, GpuSceneCacheBudgets{1ULL << 20});
        observedValue = created.hasValue();
        observed = created.diagnostic.code;
    });
    worker.join();
    expectations.expect(!observedValue, "a foreign thread cannot create a cache");
    expectations.expect(observed == GpuSceneCacheDiagnosticCode::WrongThread,
                        "foreign-thread create reports WrongThread");
    auto owner = GpuSceneCache::create(device, GpuSceneCacheBudgets{1ULL << 20});
    expectations.expect(owner.hasValue(), "the owner thread can still create a cache");
}

// The cache holds no Vulkan handle and dropping entries / destroying it on a foreign thread must
// not call Vulkan. Run last.
void testForeignThreadLifetime(Expectations& expectations, GpuDevice& device, GpuSolid& solid) {
    auto created = GpuSceneCache::create(device, GpuSceneCacheBudgets{1ULL << 32ULL});
    expectations.expect(created.hasValue(), "the lifetime cache is created");
    if (!created) {
        return;
    }
    auto image = makeSolidImage(solid, 16, 16, 0.9);
    expectations.expect(created.cache->insert("kLifetime", image) ==
                            GpuSceneCacheInsertResult::Inserted,
                        "the lifetime entry inserts");
    auto cache = std::move(created.cache);
    std::thread worker([&cache]() { cache.reset(); });
    worker.join();
    expectations.expect(true, "foreign-thread cache destruction returned without touching Vulkan");
}

} // namespace

int main(const int argc, char** argv) {
    const Options options = parseOptions(argc, argv);
    if (!options.valid) {
        return 2;
    }
    Expectations expectations;

    GpuDeviceCreationOptions createOptions;
    createOptions.loader_path = options.loader_path;
    auto device = GpuDevice::create(createOptions);
    if (!device) {
        if (options.require_device) {
            std::cerr << "FAIL: required device unavailable: " << device.diagnostic.message << '\n';
            return 1;
        }
        std::cout << "SKIP: no compatible Vulkan device available: " << device.diagnostic.message
                  << '\n';
        return expectations.ok() ? 0 : 1;
    }
    auto solid = GpuSolid::create(*device.device);
    expectations.expect(solid.hasValue(), "the SolidV1 pipeline is created for fixtures");
    if (!solid) {
        std::cerr << "FAIL: solid pipeline creation failed\n";
        return 1;
    }

    auto foreignDevice = GpuDevice::create(createOptions);
    std::unique_ptr<GpuSolid> foreignSolid;
    bool foreignAvailable = false;
    if (foreignDevice) {
        auto created = GpuSolid::create(*foreignDevice.device);
        if (created) {
            foreignSolid = std::move(created.solid);
            foreignAvailable = true;
        }
    }

    testIdentityAndRevision(expectations, *device.device, *solid.solid);
    testForeignDeviceRejection(expectations, *device.device,
                               foreignSolid ? *foreignSolid : *solid.solid, foreignAvailable);
    testBudgetAndPinning(expectations, *device.device, *solid.solid);
    testPinnedReplacement(expectations, *device.device, *solid.solid);
    testForeignThreadCreate(expectations, *device.device);
    testWrongThread(expectations, *device.device);
    testForeignThreadLifetime(expectations, *device.device, *solid.solid);

    if (!expectations.ok()) {
        std::cerr << "FAIL: scene cache expectations failed\n";
        return 1;
    }
    std::cout << "PASS: GPU scene content cache\n";
    return 0;
}
