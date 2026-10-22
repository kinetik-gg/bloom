#pragma once

#include <bloom/ui/preview_frame_trace.hpp>

#include <bloom/runtime/memory_budget_ledger.hpp>
#include <bloom/runtime/prepared_preview_frame.hpp>

#include <QObject>
#include <QTimer>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
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
    forIdentity(const runtime::PreviewRequestIdentity& identity);

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
    enum class InsertOutcome : std::uint8_t {
        Retained,
        AlreadyPresent,
        RejectedBudget,
        RejectedStale,
        AllocationFailure,
        Invalid
    };
    struct InsertResult final {
        InsertOutcome outcome = InsertOutcome::Invalid;
        std::uint64_t evictions = 0;
        [[nodiscard]] bool retained() const noexcept {
            return outcome == InsertOutcome::Retained || outcome == InsertOutcome::AlreadyPresent;
        }
    };
    // Deterministic, one-shot allocation fault seam. All boundaries precede semantic mutation.
    enum class InsertAllocation : std::uint8_t { None, Key, Product, Capacity };
    void failNextInsertAllocationForTesting(InsertAllocation boundary) noexcept {
        insertAllocationFault_ = boundary;
    }
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
        // GPU-resident entries evicted to stay inside the additive GPU-resident sublimits below.
        // Counted apart from the overall-budget evictions because the two mean different things:
        // this one says the bounded resident set is the limit, the other says the whole cache is.
        std::uint64_t gpuResidentEvictions = 0;
        // Legacy direct CPU insertions that copied packed pixels on the owner thread. Production
        // worker-prepared results must leave this at zero, even at Full/Half resolution.
        std::uint64_t uiPixelCopies = 0;

        friend bool operator==(const Statistics&, const Statistics&) = default;
    };
    struct MetadataWork final {
        std::uint64_t exactProbes = 0;
        std::uint64_t rangeEntriesVisited = 0;
        std::uint64_t revisionEntriesVisited = 0;
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

    // TEMPORAL-2B. One accepted provenance span: while a composition's time is inside [start, end),
    // the ONLY genuine snapshot revision a retained frame for that time may carry is `revision`.
    // This is the session's time-indexed provenance, so multiple revisions coexist across a
    // composition's timeline after finite edits. An empty list means "no provenance policy" and
    // retains anything (standalone caches and tests); the shared controller cache always sets the
    // session's full-cover list.
    struct RetentionSnapshot final {
        document::ProjectId projectId;
        document::CompositionId compositionId;
        document::Revision revision;
        core::RationalTime start;
        core::RationalTime end;

        friend bool operator==(const RetentionSnapshot&, const RetentionSnapshot&) = default;
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

    // Retains display-only CPU pixels or a live opaque resident lease. All allocating work
    // precedes qualification/revision pruning; allocation failure preserves prior entries and
    // accounting. Pre-admission refusal also preserves them. A lease that expires after commit
    // may report RejectedStale with actual evictions. Return final retention and the number
    // of actual budget evictions, including eviction of earlier samples (never a promised prefix).
    InsertResult insert(const PreparedPreviewFrameHandle& frame);
    void setFrameTrace(std::shared_ptr<PreviewFrameTrace> trace) { trace_ = std::move(trace); }

    [[nodiscard]] bool contains(const PreviewFrameCacheKey& key) const;
    // Cached times for exactly this project/composition/revision/resolution/display policy.
    // The probe time is ignored. UI consumers do not scan the composition's entire duration.
    [[nodiscard]] std::vector<core::RationalTime> timesFor(const PreviewFrameCacheKey& probe) const;
    // Enumerates only retained entries in this composition's half-open time interval. This is a
    // requalification boundary, not a per-frame progress query.
    [[nodiscard]] std::vector<core::RationalTime> timesForRange(const PreviewFrameCacheKey& probe,
                                                                core::RationalTime start,
                                                                core::RationalTime end) const;
    // Non-blocking owner-thread validity refresh; no native ownership crosses the boundary.
    // Also runs every 250ms only while resident entries exist, with coalesced 50ms notifications.
    void refreshResidentEntries();

    // Changing the budget evicts immediately if the new one is smaller.
    void setByteBudget(std::size_t bytes);
    // Explicit one-shot trim without changing admission. The ledger callback also reduces the
    // admission budget, so ongoing work cannot undo a pressure trim before the next poll.
    void trimToBytes(std::size_t bytes);
    [[nodiscard]] std::size_t byteBudget() const noexcept { return byteBudget_; }
    [[nodiscard]] std::size_t residentBytes() const noexcept { return residentBytes_; }
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] Statistics statistics() const noexcept { return statistics_; }
    [[nodiscard]] MetadataWork metadataWork() const noexcept { return metadataWork_; }
    void resetMetadataWorkForTesting() noexcept { metadataWork_ = {}; }
    void clear();
    // Terminal pin release after producers/native retirement; emits no cache notification.
    void releaseForShutdown() noexcept;

    // Additive GPU-resident sublimits INSIDE the overall byte budget. The default is unlimited, so
    // a standalone cache -- and every pre-existing test -- is unchanged, and the overall CPU cache
    // budget is never rewritten. The shared application cache sets these from the aligned resident
    // budget plan so its retained resident set cannot outgrow the service's lease registry: when
    // either limit is exceeded, least-recently-used RESIDENT entries are evicted. Eviction drops
    // only the cache's reference; a live lease stays valid for the viewer and for the native pins
    // that still hold it, so no live lease is invalidated. CPU entries are never touched by these
    // limits.
    void setGpuResidentLimits(std::size_t maxBytes, std::size_t maxEntries);
    // Actual resident bytes and resident entry count retained right now. Deliberately distinct from
    // residentBytes()/size(), which count EVERY entry (CPU and resident).
    [[nodiscard]] std::size_t gpuResidentBytes() const noexcept { return gpuResidentBytes_; }
    [[nodiscard]] std::size_t gpuResidentEntryCount() const noexcept { return gpuResidentEntries_; }
    [[nodiscard]] std::size_t gpuResidentByteLimit() const noexcept {
        return gpuResidentByteLimit_;
    }
    [[nodiscard]] std::size_t gpuResidentEntryLimit() const noexcept {
        return gpuResidentEntryLimit_;
    }

    // WORKAREA-1: scope retention to one composition's half-open [start, end) time range, pruning
    // any entry it excludes immediately. std::nullopt restores "retain anything" without pruning.
    // This is range management, not invalidation: it neither advances the revision nor touches a
    // displayed frame, and the frames it removes are counted as rangeDrops.
    void setRetentionRange(std::optional<RetentionRange> range);
    // Drops every retained entry for the range's project/composition whose time is outside it.
    // Returns the number dropped. Counted as rangeDrops, never as eviction or pressure.
    std::size_t pruneToRange(const RetentionRange& range);

    // TEMPORAL-2B. Installs the accepted provenance spans and prunes every entry whose time is no
    // longer represented by its own snapshot revision. Entries in other retained segments survive.
    // Empty clears the policy (retain anything) without pruning.
    void setRetentionSnapshots(std::vector<RetentionSnapshot> snapshots);

    // What retaining one frame costs: packed CPU display pixels/geometry or actual native lease
    // bytes. Deliberately not what holding the frame costs right now -- insertion keeps the display
    // buffer and drops the Float32 process image (see runtime::PreviewDisplayOnlyFrame).
    [[nodiscard]] static std::size_t
    frameByteCost(const runtime::PreparedPreviewFrame& frame) noexcept;

  signals:
    // Cache mutations coalesce for 50ms; a burst produces one ruler repaint notification.
    void contentsChanged();
    void byteBudgetChanged();
    // Exact retention deltas are delivered synchronously to the RAM coverage index. The existing
    // coalesced contentsChanged signal remains the paint notification.
    void entryRetained(const PreviewFrameCacheKey& key);
    void entryRemoved(const PreviewFrameCacheKey& key);
    void entriesCleared();

  private:
    std::shared_ptr<PreviewFrameTrace> trace_;
    struct Entry;
    struct KeyHash final {
        [[nodiscard]] std::size_t operator()(const PreviewFrameCacheKey& key) const noexcept;
    };
    struct RangeKey final {
        document::ProjectId projectId;
        document::CompositionId compositionId;
        core::RationalTime time;
    };
    struct RangeLess final {
        [[nodiscard]] bool operator()(const RangeKey& left, const RangeKey& right) const noexcept;
    };
    using Entries = std::list<Entry>;
    using EntryIterator = Entries::iterator;
    using RangeIndex = std::multimap<RangeKey, EntryIterator, RangeLess>;
    void scheduleNotification();
    // A resident entry is live exactly while its lease is; a CPU entry is always live. An
    // invalidated resident lease is never a hit and is never listed by contains()/timesFor().
    [[nodiscard]] static bool entryIsLive(const Entry& entry) noexcept;
    QTimer notificationTimer_;
    QTimer residentRefreshTimer_;
    InsertAllocation insertAllocationFault_ = InsertAllocation::None;
    void allocationCheckpoint(InsertAllocation boundary);
    struct Entry final {
        PreviewFrameCacheKey key;
        // Display-only CPU product: the packed buffer plus its identity, never the process image it
        // was mapped from. A hit is wrapped back into a PreparedPreviewFrame envelope on the way
        // out. Exactly one of `frame`/`resident` is set.
        std::shared_ptr<const runtime::PreviewDisplayOnlyFrame> frame;
        // Fourth arm: the opaque owner-bound lease plus its identity, never a CPU pixel. Retained
        // by pointer only -- no copy -- and restamped cheaply on the way out. A resident entry is
        // live only while its lease is; an invalidated lease is never a hit and never listed.
        std::shared_ptr<const runtime::PreviewResidentDisplayFrame> resident;
        std::size_t bytes = 0;
        EntryIterator self{};
        RangeIndex::iterator rangePosition{};
        std::optional<std::list<EntryIterator>::iterator> residentPosition{};
    };

    void dropStaleRevisions(const PreviewFrameCacheKey& current);
    void evictToBudget();
    // Evicts the least-recently-used RESIDENT entry (entries_ is MRU-first) while either additive
    // GPU-resident limit is exceeded. It only drops the cache's reference; it never invalidates a
    // lease and never removes a CPU entry.
    void evictGpuResidentToLimits();
    void removeAt(EntryIterator position);
    // Drops every entry whose time is no longer accepted under the current work-area range or
    // provenance spans, counting rangeDrops for a work-area exclusion and staleDrops for a
    // provenance mismatch. Retained segments are never touched wholesale.
    void pruneUnaccepted();
    // True when `key` belongs to the retention range's composition and its time is inside
    // [start, end); true when no range is set or the key names a different composition.
    [[nodiscard]] bool retains(const PreviewFrameCacheKey& key) const noexcept;
    [[nodiscard]] bool workAreaAllows(const PreviewFrameCacheKey& key) const noexcept;
    [[nodiscard]] bool provenanceAllows(const PreviewFrameCacheKey& key) const noexcept;
    // The accepted snapshot revision for `key`'s time, when a provenance policy is installed and
    // the time is covered. std::nullopt when no policy is installed or the time is not covered.
    [[nodiscard]] std::optional<document::Revision>
    acceptedRevision(const PreviewFrameCacheKey& key) const noexcept;
    [[nodiscard]] bool hasProvenancePolicy() const noexcept { return !retentionSnapshots_.empty(); }

    // Stable MRU ownership plus exact and composition/time indexes. All index nodes for an insert
    // are staged before any qualification, revision, or budget mutation.
    Entries entries_;
    std::unordered_map<PreviewFrameCacheKey, EntryIterator, KeyHash> exactIndex_;
    RangeIndex rangeIndex_;
    std::list<EntryIterator> residentOrder_;
    // With no time-indexed provenance, a project has one retained revision. Remember the last
    // revision reconciled for each project so ordinary same-revision fill never rescans the LRU.
    std::unordered_map<std::uint64_t, std::uint64_t> uniformProjectRevision_;
    runtime::MemoryBudgetLedger& ledger_;
    std::size_t byteBudget_ = defaultPreviewFrameCacheByteBudget();
    std::size_t residentBytes_ = 0;
    // GPU-resident accounting, distinct from the all-entry totals above.
    std::size_t gpuResidentBytes_ = 0;
    std::size_t gpuResidentEntries_ = 0;
    std::size_t gpuResidentByteLimit_ = std::numeric_limits<std::size_t>::max();
    std::size_t gpuResidentEntryLimit_ = std::numeric_limits<std::size_t>::max();
    std::optional<bool> displayQualified_;
    std::optional<RetentionRange> retentionRange_;
    std::vector<RetentionSnapshot> retentionSnapshots_;
    Statistics statistics_;
    mutable MetadataWork metadataWork_;
};

using PreviewFrameCacheHandle = std::shared_ptr<PreviewFrameCache>;

// Both playback memory settings are resolved through one runtime::MemoryBudgetLedger. A missing,
// unparseable, or zero value is absent; the ledger applies the machine-derived split and clamps
// effective allocations so their sum never exceeds the usable budget.
[[nodiscard]] std::size_t ramPreviewByteBudgetFromSettings(const QSettings& settings);
[[nodiscard]] std::size_t operationCacheByteBudgetFromSettings(const QSettings& settings);
[[nodiscard]] runtime::MemoryBudgetAllocation
cacheMemoryBudgetsFromSettings(const QSettings& settings);
// Read-only configured weights for an explicit composition root. Sample delivery must not reset
// the ledger's pressure recovery state by reapplying unchanged configuration.
[[nodiscard]] runtime::MemoryBudgetAllocation
cacheMemoryBudgetAllocationFromSettings(const QSettings& settings,
                                        const runtime::MemoryBudgetLedger& ledger);
void setRamPreviewByteBudgetInSettings(QSettings& settings, std::size_t bytes);

} // namespace bloom::ui
