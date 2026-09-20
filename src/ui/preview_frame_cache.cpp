#include <bloom/runtime/operation_cache.hpp>
#include <bloom/ui/preview_frame_cache.hpp>

#include <QLatin1StringView>
#include <QSettings>
#include <QVariant>

#include <algorithm>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <utility>

namespace bloom::ui {
namespace {

constexpr auto ramPreviewByteBudgetKey = "playback/ram-preview-memory-bytes";

// What the cache retains for one frame: the packed display buffer and the identity, never the
// Float32 process image (task PERF1, FORMAL AMENDMENT 1). The pixels are copied once, on the way
// in; the process image the source frame holds is simply not carried over, so it is freed as soon
// as the last live reference to that frame goes.
[[nodiscard]] std::shared_ptr<const runtime::PreviewDisplayOnlyFrame>
retainable(const runtime::PreparedPreviewFrame& frame) {
    if (!frame.hasProcessFrame()) {
        // Already display-only -- a frame this cache published a moment ago -- so there is nothing
        // left to strip and no copy to make.
        return frame.displayOnlyFrame();
    }
    auto stripped =
        runtime::PreviewDisplayOnlyFrame::create(frame, std::numeric_limits<std::size_t>::max());
    if (!stripped.has_value()) {
        return nullptr;
    }
    return std::make_shared<const runtime::PreviewDisplayOnlyFrame>(std::move(*stripped));
}

[[nodiscard]] PreparedPreviewFrameHandle
restamp(const std::shared_ptr<const runtime::PreviewDisplayOnlyFrame>& frame,
        const std::uint64_t requestGeneration) {
    // The display buffer is what a retained frame IS; the request generation is only which ask it
    // answered. Re-creating the envelope over the same immutable buffer is therefore a copy of one
    // small identity struct and a shared-pointer increment -- no pixel is touched.
    auto rebuilt = runtime::PreparedPreviewFrame::createDisplayOnly(requestGeneration, frame);
    if (!rebuilt.has_value()) {
        return nullptr;
    }
    return std::make_shared<const runtime::PreparedPreviewFrame>(std::move(*rebuilt));
}

} // namespace

PreviewFrameCacheKey
PreviewFrameCacheKey::forIdentity(const runtime::PreviewRequestIdentity& identity) {
    return {.projectId = identity.projectId,
            .compositionId = identity.compositionId,
            .sourceRevision = identity.sourceRevision,
            .time = identity.time,
            .output = identity.output,
            .resolution = identity.resolution,
            .quality = identity.quality,
            .colorIntent = identity.colorIntent,
            .resolutionPolicy = identity.resolutionPolicy,
            .roi = identity.roi,
            .viewAdjust = identity.viewAdjust,
            .displayName = identity.displayName,
            .viewName = identity.viewName,
            .showLook = identity.showLook};
}

PreviewFrameCache::PreviewFrameCache(const std::size_t byteBudget,
                                     runtime::MemoryBudgetLedger& ledger)
    : ledger_(ledger), byteBudget_(byteBudget) {
    notificationTimer_.setSingleShot(true);
    notificationTimer_.setInterval(50);
    connect(&notificationTimer_, &QTimer::timeout, this, &PreviewFrameCache::contentsChanged);
    ledger_.registerCache(
        this, byteBudget, [this] { return residentBytes(); },
        [this](const std::size_t bytes) {
            const bool pressure = ledger_.state().retentionPercent < 100;
            if (pressure)
                trimToBytes(bytes);
            const bool changed = byteBudget_ != bytes;
            byteBudget_ = bytes;
            if (!pressure)
                evictToBudget();
            if (changed)
                emit byteBudgetChanged();
        });
}

PreviewFrameCache::~PreviewFrameCache() { ledger_.unregisterCache(this); }

void PreviewFrameCache::scheduleNotification() {
    if (!notificationTimer_.isActive()) {
        notificationTimer_.start();
    }
}

std::size_t PreviewFrameCache::frameByteCost(const runtime::PreparedPreviewFrame& frame) noexcept {
    // The resident arm is identified by its honest provenance, not by residentFrame(): that
    // accessor is a precondition-guarded resident-only view, and calling it on a CPU frame would
    // read a variant member that is not active. The resident retention cost is the ACTUAL native
    // allocation the lease charges plus its retained geometry/metadata -- never a host copy.
    if (frame.provenance().provider == runtime::PreviewDisplayProvider::GpuResident) {
        const auto& resident = frame.residentFrame();
        return resident != nullptr ? resident->retainedByteCost() : 0;
    }
    // What RETAINING a CPU frame costs, which is not what holding it costs right now: insertion
    // keeps packed display pixels and evaluated geometry while dropping the Float32 process image.
    const auto view = frame.displayBufferView();
    const auto geometryBytes = frame.evaluatedBounds().size_bytes();
    if (!view ||
        geometryBytes > std::numeric_limits<std::size_t>::max() - view->pixels.size_bytes())
        return 0;
    return view->pixels.size_bytes() + geometryBytes;
}

PreparedPreviewFrameHandle
PreviewFrameCache::take(const runtime::PreviewRequestIdentity& identity) {
    const auto key = PreviewFrameCacheKey::forIdentity(identity);
    const auto position =
        std::ranges::find_if(entries_, [&key](const Entry& entry) { return entry.key == key; });
    if (position == entries_.end() || !entryIsLive(*position)) {
        // A dead resident entry (invalidated lease) is a miss, never a hit: it can never be served.
        if (position != entries_.end()) {
            removeAt(static_cast<std::size_t>(position - entries_.begin()));
        }
        ++statistics_.misses;
        return nullptr;
    }
    PreparedPreviewFrameHandle frame;
    if (position->resident != nullptr) {
        // Resident re-stamp: share the same opaque lease, copy no pixels and no native allocation.
        auto rebuilt = runtime::PreparedPreviewFrame::createResident(identity.requestGeneration,
                                                                     position->resident);
        if (rebuilt.has_value()) {
            frame = std::make_shared<const runtime::PreparedPreviewFrame>(std::move(*rebuilt));
        }
    } else {
        frame = restamp(position->frame, identity.requestGeneration);
    }
    if (frame == nullptr || frame->desiredIdentity() != identity) {
        // The key matched but the rebuilt envelope does not answer this request exactly. Nothing
        // here can be served honestly, so the entry is dropped rather than published under an
        // identity it does not have.
        removeAt(static_cast<std::size_t>(position - entries_.begin()));
        ++statistics_.misses;
        return nullptr;
    }
    std::rotate(entries_.begin(), position, position + 1);
    ++statistics_.hits;
    return frame;
}

void PreviewFrameCache::insert(const PreparedPreviewFrameHandle& frame) {
    if (frame == nullptr) {
        return;
    }
    // Identify the resident arm by its honest provenance; residentFrame() is resident-only and
    // would read an inactive variant member on a CPU frame.
    const bool resident =
        frame->provenance().provider == runtime::PreviewDisplayProvider::GpuResident;
    if (resident) {
        // Only a live lease is retainable; an invalidated one must never enter the cache.
        if (!frame->isDisplayValid()) {
            ++statistics_.rejections;
            return;
        }
    } else if (!frame->displayBufferView().has_value()) {
        return;
    }
    const auto key = PreviewFrameCacheKey::forIdentity(frame->desiredIdentity());
    if (!workAreaAllows(key)) {
        // A late completion after a work-area trim must not resurrect an out-of-range entry. The
        // frame was already displayed by its caller; refusing only the retention is honest.
        ++statistics_.rangeDrops;
        return;
    }
    if (!provenanceAllows(key)) {
        // TEMPORAL-2B: the time-indexed session now accepts a different genuine snapshot for this
        // time, so a late completion for the changed interval is refused rather than retained.
        ++statistics_.staleDrops;
        return;
    }
    if (displayQualified_.has_value() && *displayQualified_ != frame->isOcioQualified()) {
        // The display transform itself changed (the qualified processor became available). Every
        // retained frame was produced by the other one, so none of them is this composition any
        // more.
        clear();
    }
    displayQualified_ = frame->isOcioQualified();
    if (!hasProvenancePolicy()) {
        // Standalone/no-policy fallback preserves the original conservative rule: a frame of a
        // newer revision drops every older-revision entry of the same project, because without the
        // session's time-indexed policy there is no way to know the two revisions are
        // time-disjoint. The shared preview cache always installs a policy, which retains segments.
        dropStaleRevisions(key);
    }

    const auto existing =
        std::ranges::find_if(entries_, [&key](const Entry& entry) { return entry.key == key; });
    if (existing != entries_.end()) {
        if (entryIsLive(*existing)) {
            // A live entry with this key already holds the best answer; reuse it in place.
            std::rotate(entries_.begin(), existing, existing + 1);
            return;
        }
        // CACHEFIX-2. A dead resident entry (an invalidated lease) can never be served, so it must
        // not shadow the live frame being published now. Drop it first -- which releases its byte
        // charge and its budget/LRU slot -- and fall through to retain the incoming frame.
        removeAt(static_cast<std::size_t>(existing - entries_.begin()));
    }

    const auto bytes = frameByteCost(*frame);
    if (bytes == 0 || bytes > byteBudget_) {
        // One frame that does not fit the whole budget must not empty the cache trying.
        ++statistics_.rejections;
        return;
    }
    // CACHEFIX-1: copying the display buffer and growing the entry vector both allocate. Under
    // real memory pressure either can throw, and RETAINING a frame is an optimization -- the frame
    // the caller is about to publish is unaffected, so a failed insert is counted and dropped
    // rather than propagated into the render path.
    try {
        if (resident) {
            // The resident arm is retained by shared pointer only: no pixel copy, no native
            // allocation, and no process image to strip.
            entries_.insert(entries_.begin(), Entry{.key = key,
                                                    .frame = nullptr,
                                                    .resident = frame->residentFrame(),
                                                    .bytes = bytes});
        } else {
            auto retained = retainable(*frame);
            if (retained == nullptr) {
                ++statistics_.rejections;
                return;
            }
            entries_.insert(entries_.begin(), Entry{.key = key,
                                                    .frame = std::move(retained),
                                                    .resident = nullptr,
                                                    .bytes = bytes});
        }
    } catch (const std::bad_alloc&) {
        ++statistics_.allocationFailures;
        return;
    }
    residentBytes_ += bytes;
    if (resident) {
        gpuResidentBytes_ += bytes;
        ++gpuResidentEntries_;
    }
    ++statistics_.insertions;
    scheduleNotification();
    evictToBudget();
    // The additive resident sublimits are enforced after the overall budget, so the GPU set is
    // bounded independently of how large the CPU cache budget is.
    if (resident) {
        evictGpuResidentToLimits();
    }
}

void PreviewFrameCache::trimToBytes(const std::size_t bytes) {
    // Deliberately not setByteBudget(): the budget is the artist's or the ledger's decision and
    // must survive the pressure that caused this trim, so the cache can fill back up once the
    // machine recovers.
    const auto limit = std::min(bytes, byteBudget_);
    while (residentBytes_ > limit && !entries_.empty()) {
        ++statistics_.pressureDrops;
        removeAt(entries_.size() - 1);
    }
}

bool PreviewFrameCache::entryIsLive(const Entry& entry) noexcept {
    return entry.resident == nullptr || entry.resident->isDisplayValid();
}

bool PreviewFrameCache::contains(const PreviewFrameCacheKey& key) const {
    return std::ranges::any_of(
        entries_, [&key](const Entry& entry) { return entry.key == key && entryIsLive(entry); });
}

std::vector<core::RationalTime>
PreviewFrameCache::timesFor(const PreviewFrameCacheKey& probe) const {
    // TEMPORAL-2B: a probe names ONE revision, but after finite clip-range edits the retained
    // timeline legitimately spans several genuine revisions. Cached markers are therefore matched
    // on everything that decides pixels EXCEPT the source revision, so a sub-range captured at an
    // older retained revision still paints its "cached" bar.
    std::vector<core::RationalTime> times;
    auto key = probe;
    for (const auto& entry : entries_) {
        if (!entryIsLive(entry)) {
            continue;
        }
        key.time = entry.key.time;
        if (hasProvenancePolicy())
            key.sourceRevision = entry.key.sourceRevision;
        if (key == entry.key) {
            times.push_back(entry.key.time);
        }
    }
    return times;
}

void PreviewFrameCache::setByteBudget(const std::size_t bytes) {
    static_cast<void>(ledger_.setCacheCeiling(this, bytes));
}

void PreviewFrameCache::clear() {
    if (!entries_.empty()) {
        scheduleNotification();
    }
    entries_.clear();
    residentBytes_ = 0;
    gpuResidentBytes_ = 0;
    gpuResidentEntries_ = 0;
}

bool PreviewFrameCache::workAreaAllows(const PreviewFrameCacheKey& key) const noexcept {
    if (!retentionRange_.has_value())
        return true;
    const auto& range = *retentionRange_;
    if (key.projectId != range.projectId || key.compositionId != range.compositionId)
        return true;
    return key.time >= range.start && key.time < range.end;
}

std::optional<document::Revision>
PreviewFrameCache::acceptedRevision(const PreviewFrameCacheKey& key) const noexcept {
    for (const auto& span : retentionSnapshots_) {
        if (span.projectId == key.projectId && span.compositionId == key.compositionId &&
            key.time >= span.start && key.time < span.end) {
            return span.revision;
        }
    }
    return std::nullopt;
}

bool PreviewFrameCache::provenanceAllows(const PreviewFrameCacheKey& key) const noexcept {
    if (!hasProvenancePolicy())
        return true;
    const auto accepted = acceptedRevision(key);
    return accepted.has_value() && key.sourceRevision == *accepted;
}

bool PreviewFrameCache::retains(const PreviewFrameCacheKey& key) const noexcept {
    return workAreaAllows(key) && provenanceAllows(key);
}

void PreviewFrameCache::pruneUnaccepted() {
    for (std::size_t index = entries_.size(); index > 0; --index) {
        const auto& entry = entries_[index - 1];
        if (!workAreaAllows(entry.key)) {
            ++statistics_.rangeDrops;
            removeAt(index - 1);
            continue;
        }
        if (!provenanceAllows(entry.key)) {
            ++statistics_.staleDrops;
            removeAt(index - 1);
        }
    }
}

void PreviewFrameCache::dropStaleRevisions(const PreviewFrameCacheKey& current) {
    for (std::size_t index = entries_.size(); index > 0; --index) {
        const auto& entry = entries_[index - 1];
        if (entry.key.projectId != current.projectId) {
            // CACHEFIX-3. A different project is not this revision's concern; its entries stay
            // retained (the cache key and the retention policy both carry project identity).
            continue;
        }
        if (entry.key.sourceRevision == current.sourceRevision) {
            continue;
        }
        ++statistics_.staleDrops;
        removeAt(index - 1);
    }
}

void PreviewFrameCache::setRetentionSnapshots(std::vector<RetentionSnapshot> snapshots) {
    if (retentionSnapshots_ == snapshots)
        return;
    retentionSnapshots_ = std::move(snapshots);
    pruneUnaccepted();
}

std::size_t PreviewFrameCache::pruneToRange(const RetentionRange& range) {
    std::size_t dropped = 0;
    for (std::size_t index = entries_.size(); index > 0; --index) {
        const auto& entry = entries_[index - 1];
        if (entry.key.projectId != range.projectId ||
            entry.key.compositionId != range.compositionId) {
            continue;
        }
        if (entry.key.time >= range.start && entry.key.time < range.end)
            continue;
        ++statistics_.rangeDrops;
        removeAt(index - 1);
        ++dropped;
    }
    return dropped;
}

void PreviewFrameCache::setRetentionRange(std::optional<RetentionRange> range) {
    if (retentionRange_ == range)
        return;
    retentionRange_ = range;
    if (retentionRange_.has_value())
        static_cast<void>(pruneToRange(*retentionRange_));
}

void PreviewFrameCache::evictToBudget() {
    while (residentBytes_ > byteBudget_ && !entries_.empty()) {
        ++statistics_.evictions;
        removeAt(entries_.size() - 1);
    }
}

void PreviewFrameCache::removeAt(const std::size_t index) {
    scheduleNotification();
    const Entry& entry = entries_[index];
    residentBytes_ -= entry.bytes;
    if (entry.resident != nullptr) {
        gpuResidentBytes_ -= entry.bytes;
        --gpuResidentEntries_;
    }
    entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(index));
}

void PreviewFrameCache::setGpuResidentLimits(const std::size_t maxBytes,
                                             const std::size_t maxEntries) {
    gpuResidentByteLimit_ = maxBytes;
    gpuResidentEntryLimit_ = maxEntries;
    evictGpuResidentToLimits();
}

void PreviewFrameCache::evictGpuResidentToLimits() {
    // The resident set is bounded independently of the overall cache budget so it can never outgrow
    // the service's lease registry. Entries are MRU-first, so the last resident entry is the least
    // recently used GPU frame. Dropping it releases only the cache's reference: the opaque lease
    // stays valid for the viewer and for any native pin that still holds it.
    while ((gpuResidentEntries_ > gpuResidentEntryLimit_ ||
            gpuResidentBytes_ > gpuResidentByteLimit_) &&
           gpuResidentEntries_ > 0) {
        std::size_t victim = entries_.size();
        for (std::size_t index = entries_.size(); index > 0; --index) {
            if (entries_[index - 1].resident != nullptr) {
                victim = index - 1;
                break;
            }
        }
        if (victim == entries_.size()) {
            break;
        }
        // The same boundary event is both a GPU-resident eviction and, for the RAM preview
        // controller, a budget eviction: the controller only watches `evictions` to stop a run that
        // has outgrown memory, so a GPU sublimit hit must advance that signal or the run would keep
        // churning every frame against the cap instead of stopping at the prefix that fits.
        ++statistics_.gpuResidentEvictions;
        ++statistics_.evictions;
        removeAt(victim);
    }
}

std::size_t physicalMemoryBytes() noexcept { return runtime::physicalMemoryBytes(); }

std::size_t defaultPreviewFrameCacheByteBudget() noexcept {
    return runtime::MemoryBudgetLedger{}.allocate().previewFrameCacheByteBudget;
}

namespace {

[[nodiscard]] std::optional<std::size_t> budgetOverride(const QSettings& settings,
                                                        const QLatin1StringView key) {
    bool parsed = false;
    const auto bytes = settings.value(key).toString().toULongLong(&parsed);
    if (!parsed || bytes == 0 || bytes > std::numeric_limits<std::size_t>::max())
        return std::nullopt;
    return static_cast<std::size_t>(bytes);
}

} // namespace

runtime::MemoryBudgetAllocation cacheMemoryBudgetsFromSettings(const QSettings& settings) {
    const runtime::MemoryBudgetLedger ledger(physicalMemoryBytes());
    const auto allocation = ledger.allocate(
        budgetOverride(settings, QLatin1StringView("playback/operation-cache-bytes")),
        budgetOverride(settings, QLatin1StringView(ramPreviewByteBudgetKey)));
    runtime::processMemoryBudgetLedger().setConfiguredTotal(allocation.operationCacheByteBudget +
                                                            allocation.previewFrameCacheByteBudget);
    return allocation;
}

std::size_t ramPreviewByteBudgetFromSettings(const QSettings& settings) {
    return cacheMemoryBudgetsFromSettings(settings).previewFrameCacheByteBudget;
}

std::size_t operationCacheByteBudgetFromSettings(const QSettings& settings) {
    return cacheMemoryBudgetsFromSettings(settings).operationCacheByteBudget;
}

void setRamPreviewByteBudgetInSettings(QSettings& settings, const std::size_t bytes) {
    settings.setValue(QLatin1StringView(ramPreviewByteBudgetKey),
                      QString::number(static_cast<qulonglong>(bytes)));
}

} // namespace bloom::ui
