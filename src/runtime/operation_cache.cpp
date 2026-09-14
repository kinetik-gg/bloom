#include <bloom/runtime/operation_cache.hpp>

namespace bloom::runtime {
std::optional<OperationCacheValue> OperationCache::find(const std::string& content,
                                                        document::Revision revision) {
    const std::lock_guard lock(mutex_);
    const auto exact = addresses_.find({revision, content});
    if (exact != addresses_.end()) {
        entries_.splice(entries_.begin(), entries_, exact->second);
        return exact->second->value;
    }
    const auto found = index_.find(content);
    if (found == index_.end())
        return std::nullopt;
    // A new revision can adopt the exact same resolved operation content. Keep only the
    // latest revision address, rather than retaining an unbounded alias table.
    addresses_.emplace(Address{revision, found->second->content}, found->second);
    addresses_.erase({found->second->revision, content});
    found->second->revision = revision;
    entries_.splice(entries_.begin(), entries_, found->second);
    return found->second->value;
}
void OperationCache::store(std::string content, document::Revision revision,
                           OperationCacheValue value) {
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
    entries_.push_front({std::move(content), revision, std::move(value), bytes});
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
        bytes_ -= entries_.back().bytes;
        addresses_.erase({entries_.back().revision, entries_.back().content});
        index_.erase(entries_.back().content);
        entries_.pop_back();
    }
}
void OperationCache::setByteBudget(std::size_t budget) {
    const std::lock_guard lock(mutex_);
    budget_ = budget;
    evict();
}
std::size_t OperationCache::retainedBytes() const {
    const std::lock_guard lock(mutex_);
    return bytes_;
}
} // namespace bloom::runtime
