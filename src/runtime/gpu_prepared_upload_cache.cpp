#include <bloom/runtime/gpu_prepared_upload_cache.hpp>

#include <new>
#include <utility>

namespace bloom::runtime {

GpuPreparedUploadCache::GpuPreparedUploadCache(const std::uint64_t maxBytes,
                                               const std::size_t maxEntries)
    : maxBytes_(maxBytes), maxEntries_(maxEntries) {}

void GpuPreparedUploadCache::evictToFitLocked(const std::uint64_t incomingBytes,
                                              const std::size_t reservedEntries) {
    // Both bounds are enforced with subtraction-only comparators. `reservedEntries` lets a
    // replacement count the entry it is about to overwrite without touching the list yet.
    const auto overBytes = [&] {
        return retainedBytes_ > maxBytes_ || incomingBytes > maxBytes_ - retainedBytes_;
    };
    const auto overEntries = [&] {
        return entries_.size() > maxEntries_ || reservedEntries > maxEntries_ - entries_.size();
    };
    while (!order_.empty() && (overBytes() || overEntries())) {
        const std::string victim = order_.back();
        const auto entry = entries_.find(victim);
        if (entry == entries_.end()) {
            order_.pop_back();
            continue;
        }
        // The map key is a string_view into the order list node, so the map entry is erased FIRST,
        // while that string is still alive; only then is the list node erased. unordered_map::erase
        // (iterator) is permitted to hash its key, so erasing the list node first would read freed
        // storage through the dangling key.
        const auto position = entry->second.position;
        retainedBytes_ -= entry->second.bytes;
        entries_.erase(entry);
        order_.erase(position);
    }
}

std::shared_ptr<const render::Rgba32fImage>
GpuPreparedUploadCache::find(const std::string& key) noexcept {
    std::lock_guard lock(mutex_);
    const auto found = entries_.find(key);
    if (found == entries_.end()) {
        ++misses_;
        return nullptr;
    }
    order_.splice(order_.begin(), order_, found->second.position);
    ++hits_;
    return found->second.image;
}

void GpuPreparedUploadCache::store(std::string key,
                                   std::shared_ptr<const render::Rgba32fImage> image) {
    if (image == nullptr || maxBytes_ == 0 || maxEntries_ == 0) {
        return;
    }
    const std::uint64_t bytes = static_cast<std::uint64_t>(image->pixels().size_bytes());
    if (bytes == 0 || bytes > maxBytes_) {
        return;
    }
    std::lock_guard lock(mutex_);
    // Replacing an existing key drops the old entry FIRST (releasing its bytes and its slot), then
    // evicts other entries until the new size and the entry ceiling both fit, then inserts the new
    // buffer. Erasing before inserting is what makes the accessor to the new entry fresh rather
    // than a stale iterator that eviction could have invalidated, and it can never double-count the
    // old bytes.
    if (const auto existing = entries_.find(std::string_view{key}); existing != entries_.end()) {
        // Map key first (its string_view points into the order node), then the order node, so the
        // key is never hashed after its backing string has been freed.
        const auto position = existing->second.position;
        retainedBytes_ -= existing->second.bytes;
        entries_.erase(existing);
        order_.erase(position);
    }
    // A new key needs one slot plus its bytes.
    evictToFitLocked(bytes, 1);
    // Transactional insertion: only the order node this call creates is erased on failure, so an
    // existing entry and its string_view map key can never dangle.
    std::list<std::string>::iterator inserted{};
    bool pushed = false;
    try {
        order_.push_front(std::move(key));
        pushed = true;
        inserted = order_.begin();
        const auto placed =
            entries_.emplace(std::string_view{*inserted}, Entry{image, bytes, inserted});
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

std::uint64_t GpuPreparedUploadCache::retainedBytes() const {
    std::lock_guard lock(mutex_);
    return retainedBytes_;
}

std::size_t GpuPreparedUploadCache::entryCount() const {
    std::lock_guard lock(mutex_);
    return entries_.size();
}

std::uint64_t GpuPreparedUploadCache::maxBytes() const {
    std::lock_guard lock(mutex_);
    return maxBytes_;
}

std::size_t GpuPreparedUploadCache::maxEntries() const {
    std::lock_guard lock(mutex_);
    return maxEntries_;
}

std::uint64_t GpuPreparedUploadCache::hits() const {
    std::lock_guard lock(mutex_);
    return hits_;
}

std::uint64_t GpuPreparedUploadCache::misses() const {
    std::lock_guard lock(mutex_);
    return misses_;
}

} // namespace bloom::runtime
