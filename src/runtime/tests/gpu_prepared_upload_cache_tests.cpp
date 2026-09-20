#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/render/image.hpp>
#include <bloom/runtime/gpu_prepared_upload_cache.hpp>
#include <bloom/runtime/gpu_scene_coverage_cache.hpp>

#include <cstdint>
#include <iostream>
#include <memory>
#include <source_location>
#include <stdexcept>
#include <string>
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
