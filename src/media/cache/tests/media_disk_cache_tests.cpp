#include "png_test_support.hpp"

#include <bloom/media/cache/media_disk_cache.hpp>
#include <bloom/media/cache/media_disk_cache_decode.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// Coverage matches the CACHE-2 task package's test list: round trip across a simulated restart,
// eviction under budget, corruption detection, concurrent readers/writers, and a sequence-scrub
// benchmark pinned as "second pass never decodes" -- measured here as decoder invocations,
// equivalently the disk cache's own miss count (decodeThroughDiskCache() calls media::decodeImage()
// exactly once per miss and never on a hit; see media_disk_cache_decode.cpp).
namespace {

using bloom_output_png_test_support::Expectations;
using bloom_output_png_test_support::PngChunkBytes;
using bloom_output_png_test_support::ScratchDirectory;
namespace cache = bloom::media::cache;
namespace core = bloom::core;
namespace media = bloom::media;
namespace render = bloom::render;

[[nodiscard]] render::Rgba32fImage makeImage(const std::uint32_t width, const std::uint32_t height,
                                             const float seed) {
    const auto window = render::ImageWindow::create(0, 0, width, height);
    const auto descriptor = render::Rgba32fImageDescriptor::create(
        *window.value(), *window.value(), core::PixelAspectRatio::square());
    auto builder = render::Rgba32fImageBuilder::create(*descriptor.value(), 64ULL * 1024 * 1024);
    for (std::uint32_t y = 0; y < height; ++y) {
        auto row = builder.value()->row(y);
        for (std::uint32_t x = 0; x < width; ++x) {
            const float value =
                std::min(1.0F, seed + (static_cast<float>(y) * static_cast<float>(width) +
                                       static_cast<float>(x)) *
                                          0.01F);
            const auto pixel = render::Rgba32f::fromPremultiplied(value, value * 0.5F, 0.25F, 1.0F);
            (*row.value())[x] = *pixel.value();
        }
    }
    auto frozen = std::move(*builder.value()).freeze();
    return std::move(*frozen.value());
}

[[nodiscard]] bool imagesEqual(const render::Rgba32fImage& a, const render::Rgba32fImage& b) {
    if (!(*a.descriptor() == *b.descriptor()))
        return false;
    if (a.pixels().size() != b.pixels().size())
        return false;
    for (std::size_t i = 0; i < a.pixels().size(); ++i)
        if (!(a.pixels()[i] == b.pixels()[i]))
            return false;
    return true;
}

// A stable 64-character hex key, distinct per `n`, without depending on any production key
// builder -- these tests exercise MediaDiskCache's own storage mechanics directly.
[[nodiscard]] std::string keyFor(const int n) {
    const auto bytes = std::as_bytes(std::span(&n, 1));
    const auto digest = core::Sha256Hasher::hash(bytes);
    if (!digest.has_value())
        std::abort();
    const auto hex = digest->toLowercaseHex();
    return {hex.begin(), hex.end()};
}

void testRoundTripAcrossRestart(Expectations& check) {
    ScratchDirectory scratch("media-disk-cache-roundtrip");
    cache::MediaDiskCacheConfig config;
    config.rootDirectory = scratch.path() / "cache";
    config.byteBudget = 1ULL * 1024 * 1024 * 1024;
    const auto original = std::make_shared<const render::Rgba32fImage>(makeImage(6, 5, 0.2F));
    const auto key = keyFor(1);
    {
        cache::MediaDiskCache diskCache(config);
        diskCache.store(key, original);
        const auto stats = diskCache.statistics();
        check.expect(stats.entryCount == 1 && stats.storedBytes == original->pixels().size_bytes(),
                     "store records one entry with the exact payload size");
    }
    {
        // A brand-new instance over the same root simulates a process restart: nothing but the
        // directory on disk carries the entry across.
        cache::MediaDiskCache restarted(config);
        const auto found = restarted.find(key);
        check.expect(found != nullptr, "entry survives a simulated restart");
        if (found != nullptr)
            check.expect(imagesEqual(*found, *original),
                         "round-tripped pixels and descriptor are bit-for-bit identical");
        const auto stats = restarted.statistics();
        check.expect(stats.hits == 1 && stats.misses == 0 && stats.corruptDropped == 0,
                     "post-restart lookup is a clean hit");
    }
}

void testEvictionUnderByteBudget(Expectations& check) {
    ScratchDirectory scratch("media-disk-cache-eviction-bytes");
    cache::MediaDiskCacheConfig config;
    config.rootDirectory = scratch.path();
    const auto oneImageBytes = static_cast<std::uint64_t>(8 * 8) * sizeof(render::Rgba32f);
    config.byteBudget = oneImageBytes * 2 + oneImageBytes / 2; // room for ~2 of 5 entries
    cache::MediaDiskCache diskCache(config);
    constexpr int kEntries = 5;
    for (int i = 0; i < kEntries; ++i)
        diskCache.store(keyFor(i), std::make_shared<const render::Rgba32fImage>(
                                       makeImage(8, 8, static_cast<float>(i) * 0.1F)));
    const auto stats = diskCache.statistics();
    check.expect(stats.entryCount <= 2, "byte budget bounds the resident entry count");
    check.expect(stats.evictions >= static_cast<std::uint64_t>(kEntries) - 2,
                 "older entries were evicted to stay under budget");
    check.expect(diskCache.find(keyFor(kEntries - 1)) != nullptr,
                 "the most recently stored entry is retained (LRU)");
    check.expect(diskCache.find(keyFor(0)) == nullptr,
                 "the oldest entry was evicted (LRU) and its file removed");
    check.expect(!std::filesystem::exists(diskCache.entryPathForTest(keyFor(0))),
                 "an evicted entry's file is deleted, not just forgotten");
}

void testEvictionUnderEntryCountBudget(Expectations& check) {
    ScratchDirectory scratch("media-disk-cache-eviction-count");
    cache::MediaDiskCacheConfig config;
    config.rootDirectory = scratch.path();
    config.byteBudget = 1ULL * 1024 * 1024 * 1024; // bytes are not the limiting factor here
    config.maxEntryCount = 3;
    cache::MediaDiskCache diskCache(config);
    for (int i = 0; i < 7; ++i)
        diskCache.store(keyFor(100 + i),
                        std::make_shared<const render::Rgba32fImage>(makeImage(2, 2, 0.0F)));
    const auto stats = diskCache.statistics();
    check.expect(stats.entryCount <= 3, "bounded entry count independent of byte budget");
}

void testCorruptionDetection(Expectations& check) {
    ScratchDirectory scratch("media-disk-cache-corruption");
    cache::MediaDiskCacheConfig config;
    config.rootDirectory = scratch.path();
    cache::MediaDiskCache diskCache(config);
    const auto key = keyFor(7);
    diskCache.store(key, std::make_shared<const render::Rgba32fImage>(makeImage(4, 4, 0.5F)));
    check.expect(diskCache.find(key) != nullptr, "entry is readable before corruption");

    const auto path = diskCache.entryPathForTest(key);
    {
        std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
        file.seekg(0, std::ios::end);
        const auto end = file.tellg();
        check.expect(end > 0, "fixture entry file is non-empty");
        file.seekp(end - std::streamoff(1));
        file.put(static_cast<char>(0xFF));
    }
    const auto afterCorruption = diskCache.find(key);
    check.expect(afterCorruption == nullptr, "a digest-mismatched entry is treated as a miss");
    const auto stats = diskCache.statistics();
    check.expect(stats.corruptDropped == 1,
                 "corruption is counted separately from an ordinary miss");
    check.expect(!std::filesystem::exists(path), "a corrupt entry's file is removed on detection");
    check.expect(diskCache.find(key) == nullptr,
                 "the removed corrupt entry stays a miss (not resurrected from a stale index)");
}

void testConcurrentReadersAndWriters(Expectations& check) {
    ScratchDirectory scratch("media-disk-cache-concurrency");
    cache::MediaDiskCacheConfig config;
    config.rootDirectory = scratch.path();
    cache::MediaDiskCache diskCache(config);
    constexpr int kKeys = 6;
    constexpr int kRoundsPerThread = 40;
    std::vector<std::shared_ptr<const render::Rgba32fImage>> images;
    images.reserve(kKeys);
    for (int i = 0; i < kKeys; ++i)
        images.push_back(std::make_shared<const render::Rgba32fImage>(
            makeImage(5, 5, static_cast<float>(i) * 0.05F)));
    std::atomic<int> correctReads{0};
    std::atomic<int> attemptedReads{0};
    std::vector<std::thread> threads;
    threads.reserve(4);
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&, t] {
            for (int round = 0; round < kRoundsPerThread; ++round) {
                const int i = (t + round) % kKeys;
                diskCache.store(keyFor(i), images[static_cast<std::size_t>(i)]);
                if (const auto found = diskCache.find(keyFor(i))) {
                    attemptedReads.fetch_add(1, std::memory_order_relaxed);
                    if (imagesEqual(*found, *images[static_cast<std::size_t>(i)]))
                        correctReads.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& thread : threads)
        thread.join();
    check.expect(attemptedReads.load() == correctReads.load() && attemptedReads.load() > 0,
                 "every concurrent read that lands after a concurrent write of the same key on one "
                 "shared instance returns that key's own correct pixels, never another key's or "
                 "torn bytes");
}

void testTwoInstancesShareOneDirectory(Expectations& check) {
    ScratchDirectory scratch("media-disk-cache-two-instances");
    cache::MediaDiskCacheConfig config;
    config.rootDirectory = scratch.path();
    // Two independent MediaDiskCache objects -- as two separate CpuCompositionEvaluator instances
    // would each own -- pointed at the same directory, with no shared in-memory state.
    cache::MediaDiskCache first(config);
    cache::MediaDiskCache second(config);
    const auto image = std::make_shared<const render::Rgba32fImage>(makeImage(3, 3, 0.75F));
    first.store(keyFor(42), image);
    const auto found = second.find(keyFor(42));
    check.expect(found != nullptr && imagesEqual(*found, *image),
                 "a second independent instance reads what the first instance wrote to the same "
                 "directory");
}

void testSecondPassNeverDecodes(Expectations& check) {
    ScratchDirectory scratch("media-disk-cache-scrub-benchmark");
    cache::MediaDiskCacheConfig config;
    config.rootDirectory = scratch.path();
    config.byteBudget = 1ULL * 1024 * 1024 * 1024;
    constexpr int kFrameCount = 24;
    std::vector<std::shared_ptr<const render::Rgba32fImage>> frames;
    frames.reserve(kFrameCount);
    for (int i = 0; i < kFrameCount; ++i)
        frames.push_back(std::make_shared<const render::Rgba32fImage>(
            makeImage(4, 4, static_cast<float>(i) * 0.02F)));
    int decodeCalls = 0;
    const auto frameKey = [](const int i) { return keyFor(1000 + i); };
    // Mirrors evaluateImageSource()'s memory -> disk -> decode order: a disk hit never "decodes";
    // a miss decodes (counted) and schedules an off-thread write via storeAsync(), exactly the
    // contract media-io.md's "Disk cache" section requires so evaluation never waits on the write.
    const auto scrubFrame = [&](cache::MediaDiskCache& diskCache, const int i) {
        if (auto hit = diskCache.find(frameKey(i)))
            return hit;
        ++decodeCalls;
        diskCache.storeAsync(frameKey(i), frames[static_cast<std::size_t>(i)]);
        return frames[static_cast<std::size_t>(i)];
    };
    {
        cache::MediaDiskCache diskCache(config);
        for (int i = 0; i < kFrameCount; ++i) {
            const auto frame = scrubFrame(diskCache, i);
            check.expect(imagesEqual(*frame, *frames[static_cast<std::size_t>(i)]),
                         "first pass returns each frame's correct pixels");
        }
        check.expect(decodeCalls == kFrameCount,
                     "first pass decodes every frame in the range exactly once");
        // Ensures the async writer has actually landed every entry before the "restart" below --
        // production code never needs this; only the test needs the write to be observably done.
        diskCache.flush();
    }
    decodeCalls = 0;
    {
        // Restart-simulate: a brand-new cache instance (as a fresh evaluator session would build)
        // pointed at the same directory, scrubbing the identical frame range a second time.
        cache::MediaDiskCache diskCache(config);
        for (int i = 0; i < kFrameCount; ++i) {
            const auto frame = scrubFrame(diskCache, i);
            check.expect(imagesEqual(*frame, *frames[static_cast<std::size_t>(i)]),
                         "second pass returns each frame's correct pixels");
        }
        check.expect(decodeCalls == 0,
                     "second pass over the same range never decodes a single frame");
        const auto stats = diskCache.statistics();
        check.expect(stats.hits == kFrameCount && stats.misses == 0,
                     "statistics corroborate: every second-pass lookup was a disk-cache hit");
    }
}

[[nodiscard]] core::Sha256Digest digestOf(const std::string_view text) {
    const auto digest = core::Sha256Hasher::hash(std::as_bytes(std::span(text)));
    if (!digest.has_value())
        std::abort();
    return *digest;
}

void testBuildImageCacheKey(Expectations& check) {
    cache::ImageCacheKeyInputs inputs;
    inputs.contentDigest = digestOf("asset-a");
    inputs.memberFrame = 3;
    inputs.colorSpace = media::ImageColorSpace::Srgb;
    inputs.alphaAssociation = media::ImageAlphaAssociation::Straight;
    inputs.configDigest = digestOf("config");
    const auto keyA = cache::buildImageCacheKey(inputs, "decoder-v1");
    check.expect(keyA.size() == core::kSha256HexCharacters,
                 "the disk cache key is a fixed-length SHA-256 hex digest");
    check.expect(keyA == cache::buildImageCacheKey(inputs, "decoder-v1"),
                 "the same inputs and decoder identity always build the same key");
    auto changedFrame = inputs;
    changedFrame.memberFrame = 4;
    check.expect(cache::buildImageCacheKey(changedFrame, "decoder-v1") != keyA,
                 "a different member frame changes the key");
    check.expect(cache::buildImageCacheKey(inputs, "decoder-v2") != keyA,
                 "a decoder identity/version change invalidates the key, exactly as a decoder "
                 "upgrade must invalidate old disk entries");
}

// A minimal, independently constructed 1x1 RGBA PNG -- the same encoder helper
// src/media/image/tests/image_tests.cpp uses, reused here rather than duplicated as a second
// implementation.
void writeTinyPng(const std::filesystem::path& path) {
    {
        std::ofstream file(path, std::ios::binary);
        const std::array<unsigned char, 8> signature{137, 80, 78, 71, 13, 10, 26, 10};
        file.write(reinterpret_cast<const char*>(signature.data()), 8);
    }
    PngChunkBytes chunks(path);
    const std::vector<unsigned char> header{0, 0, 0, 1, 0, 0, 0, 1, 8, 6, 0, 0, 0};
    chunks.insertChunk(chunks.endOffset(), "IHDR", header);
    const std::vector<unsigned char> raw{0, 128, 64, 32, 128};
    auto size = compressBound(static_cast<uLong>(raw.size()));
    std::vector<unsigned char> compressed(size);
    if (compress2(compressed.data(), &size, raw.data(), static_cast<uLong>(raw.size()), 6) != Z_OK)
        std::abort();
    compressed.resize(size);
    chunks.insertChunk(chunks.endOffset(), "IDAT", compressed);
    chunks.insertChunk(chunks.endOffset(), "IEND", {});
    chunks.save();
}

void testDecodeThroughDiskCacheRealDecoder(Expectations& check) {
    ScratchDirectory scratch("media-disk-cache-decode-real");
    const auto path = scratch.file("frame.png");
    writeTinyPng(path);
    cache::MediaDiskCacheConfig config;
    config.rootDirectory = scratch.path() / "cache";
    cache::MediaDiskCache diskCache(config);
    cache::ImageCacheKeyInputs inputs;
    inputs.memberFrame = 0;
    const auto key = cache::buildImageCacheKey(inputs, "test-decoder-v1");

    const auto first =
        cache::decodeThroughDiskCache(path, {}, std::nullopt, key, &diskCache, false);
    check.expect(first.value.has_value(),
                 "first call decodes the real PNG through media::decodeImage");
    const auto afterFirst = diskCache.statistics();
    check.expect(afterFirst.misses == 1 && afterFirst.hits == 0,
                 "the first call is recorded as a disk-cache miss");

    // Deleting the source file makes a second real decodeImage() call fail. A second cache hit
    // succeeding anyway proves decodeThroughDiskCache() never called the decoder again.
    std::filesystem::remove(path);
    const auto second =
        cache::decodeThroughDiskCache(path, {}, std::nullopt, key, &diskCache, false);
    check.expect(second.value.has_value(),
                 "second call still succeeds from the disk cache alone -- the decoder was never "
                 "invoked (its source file no longer exists)");
    const auto afterSecond = diskCache.statistics();
    check.expect(afterSecond.hits == 1, "the second call is recorded as a disk-cache hit");

    // Corrupt the cached entry, then confirm the wrapper falls back to a real decode again.
    writeTinyPng(path);
    {
        std::fstream file(diskCache.entryPathForTest(key),
                          std::ios::binary | std::ios::in | std::ios::out);
        file.seekg(0, std::ios::end);
        file.seekp(file.tellg() - std::streamoff(1));
        file.put(static_cast<char>(0xAA));
    }
    const auto third =
        cache::decodeThroughDiskCache(path, {}, std::nullopt, key, &diskCache, false);
    check.expect(third.value.has_value(),
                 "a corrupted disk entry falls back to a real decode rather than failing");
    const auto afterThird = diskCache.statistics();
    check.expect(afterThird.corruptDropped == 1 && afterThird.misses == 2,
                 "the corrupted entry was dropped and counted, and the fallback decode is a miss");
}

void testDisabledCacheNeverTouchesDisk(Expectations& check) {
    ScratchDirectory scratch("media-disk-cache-disabled");
    cache::MediaDiskCacheConfig config;
    config.rootDirectory = scratch.path();
    config.enabled = false;
    cache::MediaDiskCache diskCache(config);
    diskCache.store(keyFor(9), std::make_shared<const render::Rgba32fImage>(makeImage(2, 2, 0.1F)));
    check.expect(diskCache.find(keyFor(9)) == nullptr, "a disabled cache never serves a hit");
    check.expect(!std::filesystem::exists(scratch.path() / "entries"),
                 "a disabled cache never creates its on-disk entries directory");
    diskCache.setEnabled(true);
    diskCache.store(keyFor(9), std::make_shared<const render::Rgba32fImage>(makeImage(2, 2, 0.1F)));
    check.expect(diskCache.find(keyFor(9)) != nullptr,
                 "re-enabling the cache resumes normal service");
}

void testClearRemovesEverything(Expectations& check) {
    ScratchDirectory scratch("media-disk-cache-clear");
    cache::MediaDiskCacheConfig config;
    config.rootDirectory = scratch.path();
    cache::MediaDiskCache diskCache(config);
    for (int i = 0; i < 3; ++i)
        diskCache.store(keyFor(200 + i),
                        std::make_shared<const render::Rgba32fImage>(makeImage(3, 3, 0.0F)));
    const auto path0 = diskCache.entryPathForTest(keyFor(200));
    check.expect(std::filesystem::exists(path0), "fixture entry exists before clear()");
    diskCache.clear();
    const auto stats = diskCache.statistics();
    check.expect(stats.entryCount == 0 && stats.storedBytes == 0,
                 "clear() resets the in-memory index and statistics");
    check.expect(!std::filesystem::exists(path0), "clear() deletes every entry file");
    check.expect(diskCache.find(keyFor(200)) == nullptr, "cleared entries are misses afterward");
}

} // namespace

// CACHEFIX-1. The pending-write queue was bounded at 64 ENTRIES and never at bytes, while each
// entry owns the last reference to a decoded Float32 image: 64 pending 4K RGBA32F frames are
// 7.9 GiB and 64 pending 8K frames are 31.6 GiB of process memory that no budget ever saw. This
// pins the byte bound, and pins that the bound is applied at storeAsync() -- before the memory is
// staged -- rather than by the writer thread after the fact.
void testAsyncWriteQueueIsBoundedByBytes(Expectations& check) {
    ScratchDirectory scratch("media-disk-cache-async-queue-bytes");
    cache::MediaDiskCacheConfig config;
    config.rootDirectory = scratch.path();
    config.byteBudget = 64ULL * 1024 * 1024;
    // One 32x32 RGBA32F image is 16 KiB of pixels. A capacity of 4 KiB is below a single image, so
    // EVERY offer is refused outright: the assertion does not depend on when the writer drained.
    config.asyncQueueByteCapacity = 4ULL * 1024;
    constexpr int kOffered = 32;
    {
        cache::MediaDiskCache diskCache(config);
        check.expect(diskCache.asyncQueueByteCapacity() == 4ULL * 1024,
                     "the configured queue byte capacity is what the cache uses");
        for (int i = 0; i < kOffered; ++i)
            diskCache.storeAsync(keyFor(2000 + i),
                                 std::make_shared<const render::Rgba32fImage>(
                                     makeImage(32, 32, static_cast<float>(i) * 0.01F)));
        diskCache.flush();
        const auto stats = diskCache.statistics();
        check.expect(stats.peakAsyncQueueBytes == 0,
                     "a write larger than the queue capacity is never staged at all");
        check.expect(stats.asyncQueueBytes == 0, "the queue account returns to zero when drained");
        check.expect(stats.droppedAsyncWrites == kOffered,
                     "every oversized async write is dropped and counted");
        check.expect(stats.storedBytes == 0 && stats.entryCount == 0,
                     "a refused async write never reaches the disk");
    }

    // With room for a few images the queue accepts them, and the high-water mark -- which is true
    // whenever the writer happened to run -- never passes the capacity.
    ScratchDirectory roomy("media-disk-cache-async-queue-room");
    config.rootDirectory = roomy.path();
    config.asyncQueueByteCapacity = 64ULL * 1024;
    {
        cache::MediaDiskCache diskCache(config);
        for (int i = 0; i < kOffered; ++i)
            diskCache.storeAsync(keyFor(3000 + i),
                                 std::make_shared<const render::Rgba32fImage>(
                                     makeImage(32, 32, static_cast<float>(i) * 0.01F)));
        diskCache.flush();
        const auto stats = diskCache.statistics();
        check.expect(stats.peakAsyncQueueBytes <= diskCache.asyncQueueByteCapacity(),
                     "the queue never holds more than its byte capacity");
        check.expect(stats.asyncQueueBytes == 0,
                     "the queue account is empty once every write has landed or been dropped");
        check.expect(stats.entryCount > 0, "writes that fit the queue still reach the disk");
        check.expect(stats.entryCount + stats.droppedAsyncWrites ==
                         static_cast<std::uint64_t>(kOffered),
                     "every offered write either landed or was counted as dropped");
    }
}

int main() {
    Expectations check;
    testRoundTripAcrossRestart(check);
    testEvictionUnderByteBudget(check);
    testEvictionUnderEntryCountBudget(check);
    testCorruptionDetection(check);
    testConcurrentReadersAndWriters(check);
    testTwoInstancesShareOneDirectory(check);
    testSecondPassNeverDecodes(check);
    testBuildImageCacheKey(check);
    testDecodeThroughDiskCacheRealDecoder(check);
    testDisabledCacheNeverTouchesDisk(check);
    testClearRemovesEverything(check);
    testAsyncWriteQueueIsBoundedByBytes(check);
    return check.failures() == 0 ? 0 : 1;
}
