#include <bloom/runtime/operation_cache.hpp>

namespace bloom::runtime {
std::optional<OperationCacheValue> OperationCache::find(const std::string& content,
                                                        document::Revision revision) {
    const std::lock_guard lock(mutex_);
    const auto exact = addresses_.find({revision, content});
    if (exact != addresses_.end()) {
        touch(exact->second);
        ++statistics_.hits;
        return exact->second->value;
    }
    const auto found = index_.find(content);
    if (found == index_.end()) {
        ++statistics_.misses;
        return std::nullopt;
    }
    // A new revision can adopt the exact same resolved operation content. Keep only the
    // latest revision address, rather than retaining an unbounded alias table.
    addresses_.emplace(Address{revision, found->second->content}, found->second);
    addresses_.erase({found->second->revision, content});
    found->second->revision = revision;
    touch(found->second);
    ++statistics_.hits;
    return found->second->value;
}
void OperationCache::store(std::string content, document::Revision revision,
                           OperationCacheValue value, const OperationCacheEntryKind kind) {
    std::size_t bytes = sizeof(Entry) + content.capacity() + sizeof(Address) + sizeof(void*) * 16;
    if (value.image)
        bytes += sizeof(render::Rgba32fImage) + value.image->pixels().size_bytes();
    bytes += value.values.capacity() * sizeof(CompiledValue);
    for (const auto& item : value.values)
        if (const auto* string = std::get_if<std::string>(&item))
            bytes += string->capacity();
    const std::lock_guard lock(mutex_);
    if (bytes > budget_ || index_.contains(content))
        return;
    ++accessEpoch_;
    const auto protectedUntil = kind == OperationCacheEntryKind::DecodedMedia
                                    ? accessEpoch_ + kDecodedMediaGraceAccesses
                                    : 0;
    entries_.push_front(
        {std::move(content), revision, std::move(value), bytes, kind, protectedUntil});
    try {
        index_.emplace(entries_.front().content, entries_.begin());
        addresses_.emplace(Address{revision, entries_.front().content}, entries_.begin());
    } catch (...) {
        index_.erase(entries_.front().content);
        entries_.pop_front();
        throw;
    }
    bytes_ += bytes;
    evict();
}
void OperationCache::evict() {
    while (bytes_ > budget_) {
        const auto candidate = evictionCandidate();
        bytes_ -= candidate->bytes;
        addresses_.erase({candidate->revision, candidate->content});
        index_.erase(candidate->content);
        entries_.erase(candidate);
    }
}
void OperationCache::touch(const std::list<Entry>::iterator entry) {
    ++accessEpoch_;
    entry->protectedUntil = entry->kind == OperationCacheEntryKind::DecodedMedia
                                ? accessEpoch_ + kDecodedMediaGraceAccesses
                                : 0;
    entries_.splice(entries_.begin(), entries_, entry);
}
std::list<OperationCache::Entry>::iterator OperationCache::evictionCandidate() {
    for (auto candidate = entries_.end(); candidate != entries_.begin();) {
        --candidate;
        if (candidate->kind == OperationCacheEntryKind::Operation ||
            accessEpoch_ >= candidate->protectedUntil)
            return candidate;
    }
    // A single protected decoded image can still exceed a newly reduced budget. The budget wins;
    // this fallback keeps eviction bounded and is only reachable when every candidate is protected.
    return std::prev(entries_.end());
}
void OperationCache::setByteBudget(const std::size_t budget) {
    const std::lock_guard lock(mutex_);
    budget_ = budget;
    evict();
}
std::size_t OperationCache::byteBudget() const {
    const std::lock_guard lock(mutex_);
    return budget_;
}
std::size_t OperationCache::retainedBytes() const {
    const std::lock_guard lock(mutex_);
    return bytes_;
}
OperationCacheAccessStatistics OperationCache::statistics() const {
    const std::lock_guard lock(mutex_);
    return statistics_;
}
} // namespace bloom::runtime
