#include <bloom/runtime/gpu_scene_coverage_cache.hpp>

#include <new>
#include <utility>

namespace bloom::runtime {

GpuSceneCoverageCache::GpuSceneCoverageCache(const std::uint64_t maxBytes)
    : maxBytes_(maxBytes == 0 ? 1 : maxBytes) {}

std::shared_ptr<const std::vector<std::uint8_t>>
GpuSceneCoverageCache::find(const std::string& geometryKey) noexcept {
    std::lock_guard lock(mutex_);
    const auto found = entries_.find(geometryKey);
    if (found == entries_.end()) {
        ++misses_;
        return nullptr;
    }
    order_.splice(order_.begin(), order_, found->second.position);
    ++hits_;
    return found->second.coverage;
}

void GpuSceneCoverageCache::store(std::string geometryKey,
                                  std::shared_ptr<const std::vector<std::uint8_t>> coverage) {
    if (coverage == nullptr) {
        return;
    }
    const std::uint64_t bytes = static_cast<std::uint64_t>(coverage->size());
    if (bytes == 0 || bytes > maxBytes_) {
        return;
    }
    std::lock_guard lock(mutex_);
    // Updating an existing key replaces the shared buffer in place: no order/map allocation can
    // fail, and the old entry is preserved until the new buffer is in hand.
    if (const auto existing = entries_.find(std::string_view{geometryKey});
        existing != entries_.end()) {
        retainedBytes_ -= existing->second.bytes;
        existing->second.coverage = std::move(coverage);
        existing->second.bytes = bytes;
        retainedBytes_ += bytes;
        order_.splice(order_.begin(), order_, existing->second.position);
        return;
    }
    while (retainedBytes_ > maxBytes_ - bytes && !order_.empty()) {
        const std::string victim = order_.back();
        const auto entry = entries_.find(victim);
        if (entry == entries_.end()) {
            order_.pop_back();
            continue;
        }
        retainedBytes_ -= entry->second.bytes;
        order_.pop_back();
        entries_.erase(entry);
    }
    // Transactional insertion: the pushed order node is tracked by iterator, so a failing map
    // emplace (or a throwing push_front, which leaves the list untouched) removes ONLY the node
    // this call created. Existing entries and their map keys are never disturbed.
    std::list<std::string>::iterator inserted{};
    bool pushed = false;
    try {
        order_.push_front(std::move(geometryKey));
        pushed = true;
        inserted = order_.begin();
        const auto placed =
            entries_.emplace(std::string_view{*inserted}, Entry{coverage, bytes, inserted});
        if (!placed.second) {
            order_.erase(inserted);
            return;
        }
    } catch (const std::bad_alloc&) {
        if (pushed) {
            order_.erase(inserted);
        }
        return;
    }
    retainedBytes_ += bytes;
}

std::uint64_t GpuSceneCoverageCache::retainedBytes() const {
    std::lock_guard lock(mutex_);
    return retainedBytes_;
}

std::size_t GpuSceneCoverageCache::entryCount() const {
    std::lock_guard lock(mutex_);
    return entries_.size();
}

std::uint64_t GpuSceneCoverageCache::hits() const {
    std::lock_guard lock(mutex_);
    return hits_;
}

std::uint64_t GpuSceneCoverageCache::misses() const {
    std::lock_guard lock(mutex_);
    return misses_;
}

} // namespace bloom::runtime
