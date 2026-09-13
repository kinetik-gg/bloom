#include <bloom/ui/preview_frame_cache.hpp>

#include <QLatin1StringView>
#include <QSettings>
#include <QVariant>

#include <algorithm>
#include <utility>

namespace bloom::ui {
namespace {

constexpr auto ramPreviewByteBudgetKey = "playback/ram-preview-memory-bytes";

[[nodiscard]] PreparedPreviewFrameHandle
restamp(const runtime::PreparedPreviewFrame& frame, const std::uint64_t requestGeneration) {
    // The display product is what a frame IS; the request generation is only which ask it answered.
    // Re-creating the envelope over the same immutable display frame is therefore a copy of one small
    // identity struct and a shared-pointer increment -- no pixel is touched.
    auto rebuilt = frame.isOcioQualified()
                       ? runtime::PreparedPreviewFrame::createQualified(requestGeneration,
                                                                        frame.qualifiedDisplayFrame())
                       : runtime::PreparedPreviewFrame::create(requestGeneration,
                                                               frame.displayFrame());
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
            .colorIntent = identity.colorIntent};
}

PreviewFrameCache::PreviewFrameCache(const std::size_t byteBudget) noexcept
    : byteBudget_(byteBudget) {}

std::size_t
PreviewFrameCache::frameByteCost(const runtime::PreparedPreviewFrame& frame) noexcept {
    std::size_t bytes = frame.processImage().pixels().size_bytes();
    if (const auto view = frame.displayBufferView(); view.has_value()) {
        bytes += view->pixels.size_bytes();
    }
    return bytes;
}

PreparedPreviewFrameHandle
PreviewFrameCache::take(const runtime::PreviewRequestIdentity& identity) {
    const auto key = PreviewFrameCacheKey::forIdentity(identity);
    const auto position = std::ranges::find_if(
        entries_, [&key](const Entry& entry) { return entry.key == key; });
    if (position == entries_.end()) {
        ++statistics_.misses;
        return nullptr;
    }
    auto frame = restamp(*position->frame, identity.requestGeneration);
    if (frame == nullptr || frame->desiredIdentity() != identity) {
        // The key matched but the rebuilt envelope does not answer this request exactly. Nothing here
        // can be served honestly, so the entry is dropped rather than published under an identity it
        // does not have.
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
        // retained frame was produced by the other one, so none of them is this composition any more.
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
    if (bytes > byteBudget_) {
        // One frame that does not fit the whole budget must not empty the cache trying.
        ++statistics_.rejections;
        return;
    }
    entries_.insert(entries_.begin(), Entry{.key = key, .frame = frame, .bytes = bytes});
    residentBytes_ += bytes;
    ++statistics_.insertions;
    evictToBudget();
}

bool PreviewFrameCache::contains(const PreviewFrameCacheKey& key) const {
    return std::ranges::any_of(entries_,
                               [&key](const Entry& entry) { return entry.key == key; });
}

void PreviewFrameCache::setByteBudget(const std::size_t bytes) {
    byteBudget_ = bytes;
    evictToBudget();
}

void PreviewFrameCache::clear() {
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
    residentBytes_ -= entries_[index].bytes;
    entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(index));
}

std::size_t ramPreviewByteBudgetFromSettings(const QSettings& settings) {
    const auto value = settings.value(QLatin1StringView(ramPreviewByteBudgetKey));
    if (!value.isValid()) {
        return kDefaultPreviewFrameCacheByteBudget;
    }
    bool parsed = false;
    const auto bytes = value.toString().toULongLong(&parsed);
    if (!parsed || bytes == 0) {
        return kDefaultPreviewFrameCacheByteBudget;
    }
    return static_cast<std::size_t>(bytes);
}

void setRamPreviewByteBudgetInSettings(QSettings& settings, const std::size_t bytes) {
    settings.setValue(QLatin1StringView(ramPreviewByteBudgetKey),
                      QString::number(static_cast<qulonglong>(bytes)));
}

} // namespace bloom::ui
