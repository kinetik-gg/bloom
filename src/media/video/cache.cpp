#include <algorithm>
#include <bloom/media/video/session.hpp>
namespace bloom::media::video {
std::shared_ptr<const provider::FrameProduct> DecodedVideoCache::find(const FrameKey& key) {
    std::lock_guard lock(mutex_);
    auto found = std::ranges::find(entries_, key, &Entry::key);
    if (found == entries_.end())
        return {};
    entries_.splice(entries_.begin(), entries_, found);
    return entries_.front().frame;
}
void DecodedVideoCache::store(FrameKey key, std::shared_ptr<const provider::FrameProduct> frame) {
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
void DecodedVideoCache::setByteBudget(std::size_t budget) {
    std::lock_guard lock(mutex_);
    budget_ = budget;
    while (!entries_.empty() && resident_ > budget_) {
        resident_ -= entries_.back().bytes;
        entries_.pop_back();
    }
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
