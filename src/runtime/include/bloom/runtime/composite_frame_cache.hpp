#pragma once

#include <bloom/core/sha256.hpp>
#include <bloom/render/image.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/evaluation.hpp>

#include <cstdint>
#include <filesystem>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

// Scene-referred composite frames that export, RAM preview and playback share, as After Effects
// reuses its disk cache: a frame rendered once at full resolution and Reference quality is not
// rendered again while its render identity holds.
//
// A frame is the composition output in the working space, before any display/view transform,
// Viewer adjustment or ROI, stored as premultiplied RGBA half floats (8 bytes a pixel, about 14 MB
// for 2048x858). A hit returns exactly those half values; consumers that need float32 receive the
// exact widening of each half.
//
// The key is the full render identity: the complete compiled plan (reachable graph, every
// parameter and every media asset with its recorded content digest), the time and output, the
// colour intent (OCIO config revision and working space), and the verified source revision of every
// reachable media file: its resolved path and the content digest that the identity-verified source
// registry (ADR 0025, platform::observeSource/hashSourceFile) vouches for, which must equal the
// digest the document recorded. A lookup revalidates through the same registry the evaluator uses
// for MediaSourceRevisionPolicy::Revalidate, so a changed, replaced, relinked or missing file
// misses. Proxy resolution, an ROI or bypassed looks are not cacheable. A hit is trusted without
// re-hashing pixels.
//
// Retention fills usable memory and is evictable: the owner registers evict() as a retention
// reclaimer on the processing root, and insert() evicts least-recently-used frames first when the
// root reports less free host capacity than the new frame needs.
namespace bloom::runtime {

class GpuProcessingPhasePolicy;

struct MediaSourceStamp final {
    std::string path;
    core::Sha256Digest digest{};

    friend bool operator==(const MediaSourceStamp&, const MediaSourceStamp&) = default;
};

struct CompositeFrameKey final {
    std::shared_ptr<const CompiledCompositionPlan> plan;
    core::RationalTime time;
    OperationIndex output;
    EvaluationColorIntent colorIntent = EvaluationColorIntent::LinearRec709Scene;
    std::vector<MediaSourceStamp> media;

    // Deep plan equality: two revisions with the same reachable graph share frames.
    friend bool operator==(const CompositeFrameKey&, const CompositeFrameKey&);
};

// The key for a cacheable request, or nullopt (proxy, ROI, bypassed looks, or media whose verified
// revision does not match the recorded digest). Resolves each reachable media file below
// `assetBaseDirectory` and verifies it through the source registry (a full hash only the first time
// a file is seen in this session); call on a worker thread.
[[nodiscard]] std::optional<CompositeFrameKey>
compositeFrameKeyFor(const std::shared_ptr<const CompiledCompositionPlan>& plan,
                     const EvaluationRequest& request,
                     const std::filesystem::path& assetBaseDirectory);

// Revalidates the key's media through the source registry's cheap identity observation. False when
// any file changed, vanished, was replaced or cannot be observed.
[[nodiscard]] bool compositeFrameMediaCurrent(const CompositeFrameKey& key);

class CompositeFrame final {
  public:
    // Rounds each float32 component to the nearest half (ties to even). Null on allocation failure.
    [[nodiscard]] static std::shared_ptr<const CompositeFrame>
    fromImage(const render::Rgba32fImage& image);
    // The exact float32 widening of every stored half. Fails only on allocation failure.
    [[nodiscard]] render::ImageResult<render::Rgba32fImage> toImage() const;
    [[nodiscard]] const render::Rgba32fImageDescriptor& descriptor() const noexcept {
        return descriptor_;
    }
    [[nodiscard]] std::span<const std::uint16_t> halfComponents() const noexcept { return halves_; }
    [[nodiscard]] std::uint64_t bytes() const noexcept {
        return halves_.size() * sizeof(std::uint16_t);
    }

  private:
    CompositeFrame(render::Rgba32fImageDescriptor descriptor, std::vector<std::uint16_t> halves)
        : descriptor_(descriptor), halves_(std::move(halves)) {}
    render::Rgba32fImageDescriptor descriptor_;
    std::vector<std::uint16_t> halves_;
};

[[nodiscard]] std::uint16_t floatToHalf(float value) noexcept;
[[nodiscard]] float halfToFloat(std::uint16_t value) noexcept;

struct CompositeFrameCacheStats final {
    std::size_t entries = 0;
    std::uint64_t bytes = 0;
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t staleMediaMisses = 0;
    std::uint64_t evictions = 0;
};

class CompositeFrameCache final {
  public:
    // With a policy, insert() keeps the root's free usable host capacity covering the new frame.
    explicit CompositeFrameCache(std::shared_ptr<GpuProcessingPhasePolicy> policy = {});
    CompositeFrameCache(const CompositeFrameCache&) = delete;
    CompositeFrameCache& operator=(const CompositeFrameCache&) = delete;

    // Thread-safe. A hit refreshes recency; media that changed on disk drops the entry and misses.
    [[nodiscard]] std::shared_ptr<const CompositeFrame> lookup(const CompositeFrameKey& key);
    // Thread-safe. Replaces an equal key. False when the frame cannot fit usable memory at all.
    bool insert(CompositeFrameKey key, std::shared_ptr<const CompositeFrame> frame);
    // Evicts least-recently-used frames until `bytes` were released or the cache is empty.
    std::uint64_t evict(std::uint64_t bytes);
    void clear();
    [[nodiscard]] CompositeFrameCacheStats stats() const;

  private:
    struct Entry final {
        CompositeFrameKey key;
        std::shared_ptr<const CompositeFrame> frame;
    };
    [[nodiscard]] static std::size_t bucketOf(const CompositeFrameKey& key) noexcept;
    std::uint64_t evictLocked(std::uint64_t bytes);

    std::shared_ptr<GpuProcessingPhasePolicy> policy_;
    mutable std::mutex mutex_;
    // Most recently used first.
    std::list<Entry> entries_;
    std::unordered_multimap<std::size_t, std::list<Entry>::iterator> index_;
    CompositeFrameCacheStats stats_;
    std::uint64_t sampleSerial_ = 0;
    std::uint64_t bytesAtSample_ = 0;
};

} // namespace bloom::runtime
