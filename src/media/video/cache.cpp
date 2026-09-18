#include <algorithm>
#include <bloom/media/video/session.hpp>
namespace bloom::media::video {
DecodedVideoCache::DecodedVideoCache(const std::size_t budget, runtime::MemoryBudgetLedger& ledger)
    : ledger_(ledger), budget_(budget) {
    ledger_.registerCache(
        this, budget, [this] { return residentBytes(); },
        [this](const std::size_t bytes) {
            std::lock_guard lock(mutex_);
            budget_ = bytes;
            while (!entries_.empty() && resident_ > budget_) {
                resident_ -= entries_.back().bytes;
                entries_.pop_back();
            }
        });
}
DecodedVideoCache::~DecodedVideoCache() { ledger_.unregisterCache(this); }

std::shared_ptr<const provider::FrameProduct> DecodedVideoCache::find(const FrameKey& key) {
    std::lock_guard lock(mutex_);
    auto found = std::ranges::find(entries_, key, &Entry::key);
    if (found == entries_.end())
        return {};
    entries_.splice(entries_.begin(), entries_, found);
    return entries_.front().frame;
}
void DecodedVideoCache::store(const FrameKey& key,
                              std::shared_ptr<const provider::FrameProduct> frame) {
    if (!frame || !provider::valid(*frame))
        return;
    std::size_t bytes = 0;
    for (const auto& plane : frame->planes)
        bytes += plane.bytes.size();
    std::lock_guard lock(mutex_);
    if (bytes > budget_)
        return;
    const auto existing = std::ranges::find(entries_, key, &Entry::key);
    if (existing != entries_.end()) {
        resident_ -= existing->bytes;
        entries_.erase(existing);
    }
    while (!entries_.empty() && resident_ > budget_ - bytes) {
        resident_ -= entries_.back().bytes;
        entries_.pop_back();
    }
    entries_.push_front({key, std::move(frame), bytes});
    resident_ += bytes;
}
void DecodedVideoCache::setByteBudget(const std::size_t budget) {
    static_cast<void>(ledger_.setCacheCeiling(this, budget));
}
std::size_t DecodedVideoCache::residentBytes() const {
    std::lock_guard lock(mutex_);
    return resident_;
}
std::size_t DecodedVideoCache::byteBudget() const {
    std::lock_guard lock(mutex_);
    return budget_;
}
} // namespace bloom::media::video
