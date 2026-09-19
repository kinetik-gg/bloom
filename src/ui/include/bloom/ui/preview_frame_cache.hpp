#pragma once

#include <bloom/runtime/memory_budget_ledger.hpp>
#include <bloom/runtime/prepared_preview_frame.hpp>

#include <QObject>
#include <QTimer>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class QSettings;

namespace bloom::ui {

// The floor of the default budget (docs/architecture/animation-and-time.md, "RAM preview"). A
// retained frame is its packed RGBA8 display buffer and nothing else -- about 8 MB at 1920x1080, a
// quarter of what the Float32 process image it was mapped from would have cost -- so even the floor
// holds roughly 250 frames of a composition-resolution range.
inline constexpr std::size_t kMinimumPreviewFrameCacheByteBudget =
    runtime::kMinimumPreviewFrameCacheByteBudget;

// The configured preview ceiling is 40% of the ledger default total, with a 2 GiB preview
// minimum at the low-memory floor. Live admission can be smaller under contention or pressure.
[[nodiscard]] std::size_t defaultPreviewFrameCacheByteBudget() noexcept;

// Total physical memory in bytes, or 0 when the platform does not report it.
[[nodiscard]] std::size_t physicalMemoryBytes() noexcept;

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

    runtime::PreviewResolutionPolicy resolutionPolicy = runtime::PreviewResolutionPolicy::Auto;

    [[nodiscard]] static PreviewFrameCacheKey
    forIdentity(const runtime::PreviewRequestIdentity& identity) noexcept;

    std::optional<render::ImageWindow> roi = std::nullopt;
    runtime::ViewAdjust viewAdjust{};
    std::string displayName;
    std::string viewName;
    bool showLook = true;

    friend bool operator==(const PreviewFrameCacheKey&, const PreviewFrameCacheKey&) = default;
};

using PreparedPreviewFrameHandle = std::shared_ptr<const runtime::PreparedPreviewFrame>;

// The RAM preview cache: prepared preview frames held in memory under a byte budget, so that
// playing a range a second time, or stepping back to a frame already rendered, costs a lookup
// instead of an evaluation.
//
// Single-threaded by design and by ownership: every entry is put in and taken out on the interface
// thread, by the preview controller publishing a finished frame and by the RAM preview controller
// pre-rendering a range. Nothing here is locked, because nothing here is touched by a task worker
// -- a worker produces a frame and hands it back through the scheduler as it always did.
//
// Invalidation is the key rather than a notification: a document edit advances the revision, so
// every entry of an earlier revision is unreachable by construction. Those entries are dropped
// outright when a frame of a newer revision arrives, which is both the cheapest invalidation and
// the one that frees the most memory -- and it is why the cache never has to understand what an
// edit changed.
//
// The display identity is a CACHE-WIDE tag rather than part of the key: the qualified display
// processor publishes once per session, so an entry's display identity can change at most once, and
// when it does every earlier entry is stale. Inserting a frame whose qualification differs from the
// tag therefore clears the cache and adopts the new tag.
class PreviewFrameCache final : public QObject {
    Q_OBJECT

  public:
    struct Statistics final {
        std::uint64_t hits = 0;
        std::uint64_t misses = 0;
        std::uint64_t insertions = 0;
        // Entries dropped to stay inside the budget, and entries dropped because a newer document
        // revision made them unreachable. Counted apart because they mean different things: the
        // first says the budget is the limit, the second says the artist edited.
        std::uint64_t evictions = 0;
        std::uint64_t staleDrops = 0;
        // Frames refused because one frame alone does not fit the budget.
        std::uint64_t rejections = 0;
        // CACHEFIX-1. Frames dropped because retaining them threw std::bad_alloc: the insert is
        // abandoned, the published frame is untouched, and the viewer still shows it.
        std::uint64_t allocationFailures = 0;
        // Frames dropped by a memory-pressure trim, as opposed to ordinary budget eviction.
        std::uint64_t pressureDrops = 0;
        // Frames dropped (or refused on insertion) because a work-area edit put them outside the
        // retained range. Counted apart from evictions and pressure so an explicit range trim never
        // reads as, or behaves as, memory pressure.
        std::uint64_t rangeDrops = 0;

        friend bool operator==(const Statistics&, const Statistics&) = default;
    };

    // The half-open time range [start, end) this cache is scoped to for one composition, taken from
    // the LIVE work area. A frame whose project/composition matches and whose time falls outside is
    // not retained (it may still be displayed), and setting a narrower range prunes what is already
    // held. std::nullopt means "retain anything", which is the default and what a standalone cache
    // uses; the shared cache of `CompositionPreviewController` always sets it from the session.
    struct RetentionRange final {
        document::ProjectId projectId;
        document::CompositionId compositionId;
        core::RationalTime start;
        core::RationalTime end;

        friend bool operator==(const RetentionRange&, const RetentionRange&) = default;
    };

    explicit PreviewFrameCache(
        std::size_t byteBudget = defaultPreviewFrameCacheByteBudget(),
        runtime::MemoryBudgetLedger& ledger = runtime::processMemoryBudgetLedger());
    ~PreviewFrameCache() override;

    // The cached frame for `identity`, re-stamped with that identity's own request generation so
    // the caller can publish it as the answer to THIS request (a frame's generation says which ask
    // it answered; its pixels are what the key matched on). Null on a miss. Counts a hit or a miss,
    // and moves a hit to the front of the eviction order.
    [[nodiscard]] PreparedPreviewFrameHandle take(const runtime::PreviewRequestIdentity& identity);

    // Retains `frame`'s DISPLAY BUFFER under its own identity's key -- the process image is dropped
    // here, so a hit can never hand back scene-linear pixels. Drops every entry of an older
    // revision first, then evicts least-recently-used entries until the budget is satisfied. A
    // frame larger than the whole budget is refused rather than allowed to evict everything for
    // itself.
    void insert(const PreparedPreviewFrameHandle& frame);

    [[nodiscard]] bool contains(const PreviewFrameCacheKey& key) const;
    // Cached times for exactly this project/composition/revision/resolution/display policy.
    // The probe time is ignored. UI consumers do not scan the composition's entire duration.
    [[nodiscard]] std::vector<core::RationalTime> timesFor(const PreviewFrameCacheKey& probe) const;

    // Changing the budget evicts immediately if the new one is smaller.
    void setByteBudget(std::size_t bytes);
    // Explicit one-shot trim without changing admission. The ledger callback also reduces the
    // admission budget, so ongoing work cannot undo a pressure trim before the next poll.
    void trimToBytes(std::size_t bytes);
    [[nodiscard]] std::size_t byteBudget() const noexcept { return byteBudget_; }
    [[nodiscard]] std::size_t residentBytes() const noexcept { return residentBytes_; }
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] Statistics statistics() const noexcept { return statistics_; }
    void clear();

    // WORKAREA-1: scope retention to one composition's half-open [start, end) time range, pruning
    // any entry it excludes immediately. std::nullopt restores "retain anything" without pruning.
    // This is range management, not invalidation: it neither advances the revision nor touches a
    // displayed frame, and the frames it removes are counted as rangeDrops.
    void setRetentionRange(std::optional<RetentionRange> range);
    // Drops every retained entry for the range's project/composition whose time is outside it.
    // Returns the number dropped. Counted as rangeDrops, never as eviction or pressure.
    std::size_t pruneToRange(const RetentionRange& range);

    // What RETAINING one frame costs: its packed display buffer, and nothing else. Deliberately not
    // what holding the frame costs right now -- insertion keeps the display buffer and drops the
    // Float32 process image (see runtime::PreviewDisplayOnlyFrame).
    [[nodiscard]] static std::size_t
    frameByteCost(const runtime::PreparedPreviewFrame& frame) noexcept;

  signals:
    // Cache mutations coalesce for 50ms; a burst produces one ruler repaint notification.
    void contentsChanged();
    void byteBudgetChanged();

  private:
    void scheduleNotification();
    QTimer notificationTimer_;
    struct Entry final {
        PreviewFrameCacheKey key;
        // Display-only: the packed buffer plus its identity, never the process image it was mapped
        // from. A hit is wrapped back into a PreparedPreviewFrame envelope on the way out.
        std::shared_ptr<const runtime::PreviewDisplayOnlyFrame> frame;
        std::size_t bytes = 0;
    };

    void dropStaleRevisions(const PreviewFrameCacheKey& current);
    void evictToBudget();
    void removeAt(std::size_t index);
    // True when `key` belongs to the retention range's composition and its time is inside
    // [start, end); true when no range is set or the key names a different composition.
    [[nodiscard]] bool retains(const PreviewFrameCacheKey& key) const noexcept;

    // Most-recently-used first.
    std::vector<Entry> entries_;
    runtime::MemoryBudgetLedger& ledger_;
    std::size_t byteBudget_ = defaultPreviewFrameCacheByteBudget();
    std::size_t residentBytes_ = 0;
    std::optional<bool> displayQualified_;
    std::optional<RetentionRange> retentionRange_;
    Statistics statistics_;
};

using PreviewFrameCacheHandle = std::shared_ptr<PreviewFrameCache>;

// Both playback memory settings are resolved through one runtime::MemoryBudgetLedger. A missing,
// unparseable, or zero value is absent; the ledger applies the machine-derived split and clamps
// effective allocations so their sum never exceeds the usable budget.
[[nodiscard]] std::size_t ramPreviewByteBudgetFromSettings(const QSettings& settings);
[[nodiscard]] std::size_t operationCacheByteBudgetFromSettings(const QSettings& settings);
[[nodiscard]] runtime::MemoryBudgetAllocation
cacheMemoryBudgetsFromSettings(const QSettings& settings);
void setRamPreviewByteBudgetInSettings(QSettings& settings, std::size_t bytes);

} // namespace bloom::ui
