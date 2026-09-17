#include <bloom/runtime/operation_cache.hpp>
#include <bloom/ui/preview_frame_cache.hpp>

#include <QLatin1StringView>
#include <QSettings>
#include <QVariant>

#include <algorithm>
#include <limits>
#include <memory>
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
PreviewFrameCacheKey::forIdentity(const runtime::PreviewRequestIdentity& identity) noexcept {
    return {.projectId = identity.projectId,
            .compositionId = identity.compositionId,
            .sourceRevision = identity.sourceRevision,
            .time = identity.time,
            .output = identity.output,
            .resolution = identity.resolution,
            .quality = identity.quality,
            .colorIntent = identity.colorIntent,
            .resolutionPolicy = identity.resolutionPolicy};
}

PreviewFrameCache::PreviewFrameCache(const std::size_t byteBudget) noexcept
    : byteBudget_(byteBudget) {
    notificationTimer_.setSingleShot(true);
    notificationTimer_.setInterval(50);
    connect(&notificationTimer_, &QTimer::timeout, this, &PreviewFrameCache::contentsChanged);
}

void PreviewFrameCache::scheduleNotification() {
    if (!notificationTimer_.isActive()) {
        notificationTimer_.start();
    }
}

std::size_t PreviewFrameCache::frameByteCost(const runtime::PreparedPreviewFrame& frame) noexcept {
    // What RETAINING this frame costs, which is not what holding it costs right now: insertion
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
    if (position == entries_.end()) {
        ++statistics_.misses;
        return nullptr;
    }
    auto frame = restamp(position->frame, identity.requestGeneration);
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
    if (frame == nullptr || !frame->displayBufferView().has_value()) {
        return;
    }
    const auto key = PreviewFrameCacheKey::forIdentity(frame->desiredIdentity());
    if (displayQualified_.has_value() && *displayQualified_ != frame->isOcioQualified()) {
        // The display transform itself changed (the qualified processor became available). Every
        // retained frame was produced by the other one, so none of them is this composition any
        // more.
        clear();
    }
    displayQualified_ = frame->isOcioQualified();
    dropStaleRevisions(key);

    const auto existing =
        std::ranges::find_if(entries_, [&key](const Entry& entry) { return entry.key == key; });
    if (existing != entries_.end()) {
        std::rotate(entries_.begin(), existing, existing + 1);
        return;
    }

    const auto bytes = frameByteCost(*frame);
    if (bytes == 0 || bytes > byteBudget_) {
        // One frame that does not fit the whole budget must not empty the cache trying.
        ++statistics_.rejections;
        return;
    }
    auto retained = retainable(*frame);
    if (retained == nullptr) {
        ++statistics_.rejections;
        return;
    }
    entries_.insert(entries_.begin(),
                    Entry{.key = key, .frame = std::move(retained), .bytes = bytes});
    residentBytes_ += bytes;
    ++statistics_.insertions;
    scheduleNotification();
    evictToBudget();
}

bool PreviewFrameCache::contains(const PreviewFrameCacheKey& key) const {
    return std::ranges::any_of(entries_, [&key](const Entry& entry) { return entry.key == key; });
}

std::vector<core::RationalTime>
PreviewFrameCache::timesFor(const PreviewFrameCacheKey& probe) const {
    std::vector<core::RationalTime> times;
    auto key = probe;
    for (const auto& entry : entries_) {
        key.time = entry.key.time;
        if (key == entry.key) {
            times.push_back(entry.key.time);
        }
    }
    return times;
}

void PreviewFrameCache::setByteBudget(const std::size_t bytes) {
    if (byteBudget_ == bytes) {
        return;
    }
    byteBudget_ = bytes;
    evictToBudget();
    emit byteBudgetChanged();
}

void PreviewFrameCache::clear() {
    if (!entries_.empty()) {
        scheduleNotification();
    }
    entries_.clear();
    residentBytes_ = 0;
}

void PreviewFrameCache::dropStaleRevisions(const PreviewFrameCacheKey& current) {
    for (std::size_t index = entries_.size(); index > 0; --index) {
        const auto& entry = entries_[index - 1];
        if (entry.key.projectId == current.projectId &&
            entry.key.sourceRevision == current.sourceRevision) {
            continue;
        }
        ++statistics_.staleDrops;
        removeAt(index - 1);
    }
}

void PreviewFrameCache::evictToBudget() {
    while (residentBytes_ > byteBudget_ && !entries_.empty()) {
        ++statistics_.evictions;
        removeAt(entries_.size() - 1);
    }
}

void PreviewFrameCache::removeAt(const std::size_t index) {
    scheduleNotification();
    residentBytes_ -= entries_[index].bytes;
    entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(index));
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
    return ledger.allocate(
        budgetOverride(settings, QLatin1StringView("playback/operation-cache-bytes")),
        budgetOverride(settings, QLatin1StringView(ramPreviewByteBudgetKey)));
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
