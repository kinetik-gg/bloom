#pragma once

#include <bloom/runtime/prepared_preview_frame.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace bloom::ui {

// 2 GiB by default (docs/architecture/animation-and-time.md, "RAM preview"). A composition-resolution
// preview frame retains both its packed display buffer and the Float32 process image it was mapped
// from, so it is tens of megabytes: the budget is what decides how many frames of a range can be held
// at once, and the honest answer for a 1920x1080 composition is a few dozen.
inline constexpr std::size_t kDefaultPreviewFrameCacheByteBudget =
    std::size_t{2} * 1024U * 1024U * 1024U;

// Everything about a preview request that decides its PIXELS: the document revision, the exact
// rational time, the display identity, and the resolution -- which is where a proxy factor lives.
// Deliberately everything in runtime::PreviewRequestIdentity except requestGeneration, because the
// generation says which ASK a frame answered, not what it contains.
struct PreviewFrameCacheKey final {
    document::ProjectId projectId;
    document::CompositionId compositionId;
    document::Revision sourceRevision;
    core::RationalTime time;
    runtime::PreviewOutput output = runtime::PreviewOutput::Composition;
    runtime::EvaluationResolution resolution;
    runtime::EvaluationQuality quality = runtime::EvaluationQuality::Reference;
    runtime::EvaluationColorIntent colorIntent = runtime::EvaluationColorIntent::LinearRec709Scene;

    [[nodiscard]] static PreviewFrameCacheKey
    forIdentity(const runtime::PreviewRequestIdentity& identity) noexcept;

    friend bool operator==(const PreviewFrameCacheKey&, const PreviewFrameCacheKey&) = default;
};

using PreparedPreviewFrameHandle = std::shared_ptr<const runtime::PreparedPreviewFrame>;

// The RAM preview cache: prepared preview frames held in memory under a byte budget, so that playing
// a range a second time, or stepping back to a frame already rendered, costs a lookup instead of an
// evaluation.
//
// Single-threaded by design and by ownership: every entry is put in and taken out on the interface
// thread, by the preview controller publishing a finished frame and by the RAM preview controller
// pre-rendering a range. Nothing here is locked, because nothing here is touched by a task worker --
// a worker produces a frame and hands it back through the scheduler as it always did.
//
// Invalidation is the key rather than a notification: a document edit advances the revision, so every
// entry of an earlier revision is unreachable by construction. Those entries are dropped outright
// when a frame of a newer revision arrives, which is both the cheapest invalidation and the one that
// frees the most memory -- and it is why the cache never has to understand what an edit changed.
//
// The display identity is a CACHE-WIDE tag rather than part of the key: the qualified display
// processor publishes once per session, so an entry's display identity can change at most once, and
// when it does every earlier entry is stale. Inserting a frame whose qualification differs from the
// tag therefore clears the cache and adopts the new tag.
class PreviewFrameCache final {
  public:
    struct Statistics final {
        std::uint64_t hits = 0;
        std::uint64_t misses = 0;
        std::uint64_t insertions = 0;
        // Entries dropped to stay inside the budget, and entries dropped because a newer document
        // revision made them unreachable. Counted apart because they mean different things: the first
        // says the budget is the limit, the second says the artist edited.
        std::uint64_t evictions = 0;
        std::uint64_t staleDrops = 0;
        // Frames refused because one frame alone does not fit the budget.
        std::uint64_t rejections = 0;

        friend bool operator==(const Statistics&, const Statistics&) = default;
    };

    explicit PreviewFrameCache(std::size_t byteBudget = kDefaultPreviewFrameCacheByteBudget) noexcept;

    // The cached frame for `identity`, re-stamped with that identity's own request generation so the
    // caller can publish it as the answer to THIS request (a frame's generation says which ask it
    // answered; its pixels are what the key matched on). Null on a miss. Counts a hit or a miss, and
    // moves a hit to the front of the eviction order.
    [[nodiscard]] PreparedPreviewFrameHandle take(const runtime::PreviewRequestIdentity& identity);

    // Retains `frame` under its own identity's key. Drops every entry of an older revision first,
    // then evicts least-recently-used entries until the budget is satisfied. A frame larger than the
    // whole budget is refused rather than allowed to evict everything for itself.
    void insert(const PreparedPreviewFrameHandle& frame);

    [[nodiscard]] bool contains(const PreviewFrameCacheKey& key) const;

    // Changing the budget evicts immediately if the new one is smaller.
    void setByteBudget(std::size_t bytes);
    [[nodiscard]] std::size_t byteBudget() const noexcept { return byteBudget_; }
    [[nodiscard]] std::size_t residentBytes() const noexcept { return residentBytes_; }
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] Statistics statistics() const noexcept { return statistics_; }
    void clear();

    // What one retained frame costs: its packed display buffer plus the process image it keeps alive.
    [[nodiscard]] static std::size_t frameByteCost(const runtime::PreparedPreviewFrame& frame) noexcept;

  private:
    struct Entry final {
        PreviewFrameCacheKey key;
        PreparedPreviewFrameHandle frame;
        std::size_t bytes = 0;
    };

    void dropStaleRevisions(const PreviewFrameCacheKey& current);
    void evictToBudget();
    void removeAt(std::size_t index);

    // Most-recently-used first.
    std::vector<Entry> entries_;
    std::size_t byteBudget_ = kDefaultPreviewFrameCacheByteBudget;
    std::size_t residentBytes_ = 0;
    std::optional<bool> displayQualified_;
    Statistics statistics_;
};

using PreviewFrameCacheHandle = std::shared_ptr<PreviewFrameCache>;

} // namespace bloom::ui
