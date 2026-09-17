#include <bloom/runtime/operation_cache.hpp>

#include <algorithm>
#include <new>

namespace bloom::runtime {
OperationCache::OperationCache(const std::size_t budget, MemoryBudgetLedger& ledger)
    : ledger_(ledger), budget_(budget) {
    ledger_.registerCache(
        this, budget, [this] { return retainedBytes(); },
        [this](const std::size_t bytes) {
            const bool pressure = ledger_.state().retentionPercent < 100;
            const std::lock_guard lock(mutex_);
            budget_ = bytes;
            std::uint64_t ordinaryDrops = 0;
            evictToLocked(bytes, pressure ? statistics_.pressureDrops : ordinaryDrops);
        });
}
OperationCache::~OperationCache() { ledger_.unregisterCache(this); }

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
    // CACHEFIX-1: a cache insert is an optimization, never a reason to fail the work that produced
    // the value. If the node, the index entry or the address entry cannot be allocated, the insert
    // is unwound and DROPPED -- the caller keeps its own value and the frame still renders. The
    // allocation that already failed is never retried here, and nothing past it is attempted.
    try {
        entries_.push_front(
            {std::move(content), revision, std::move(value), bytes, kind, protectedUntil});
    } catch (const std::bad_alloc&) {
        ++statistics_.allocationFailures;
        return;
    }
    try {
        index_.emplace(entries_.front().content, entries_.begin());
        addresses_.emplace(Address{revision, entries_.front().content}, entries_.begin());
    } catch (const std::bad_alloc&) {
        index_.erase(entries_.front().content);
        entries_.pop_front();
        ++statistics_.allocationFailures;
        return;
    } catch (...) {
        // Anything that is not an allocation failure keeps its original contract: unwind the
        // partial insert and let the caller see what went wrong.
        index_.erase(entries_.front().content);
        entries_.pop_front();
        throw;
    }
    bytes_ += bytes;
    if (kind == OperationCacheEntryKind::DecodedMedia)
        decodedMediaBytes_ += bytes;
    evict();
}
void OperationCache::removeLocked(const std::list<Entry>::iterator candidate) {
    bytes_ -= candidate->bytes;
    if (candidate->kind == OperationCacheEntryKind::DecodedMedia)
        decodedMediaBytes_ -= std::min(decodedMediaBytes_, candidate->bytes);
    addresses_.erase({candidate->revision, candidate->content});
    index_.erase(candidate->content);
    entries_.erase(candidate);
}

void OperationCache::evictToLocked(const std::size_t limit, std::uint64_t& counter) {
    while (bytes_ > limit && !entries_.empty()) {
        removeLocked(evictionCandidate());
        ++counter;
    }
}

void OperationCache::evict() {
    std::uint64_t ignored = 0;
    evictToLocked(budget_, ignored);
}

void OperationCache::trimToBytes(const std::size_t bytes) {
    const std::lock_guard lock(mutex_);
    evictToLocked(std::min(bytes, budget_), statistics_.pressureDrops);
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
    static_cast<void>(ledger_.setCacheCeiling(this, budget));
}
std::size_t OperationCache::byteBudget() const {
    const std::lock_guard lock(mutex_);
    return budget_;
}
std::size_t OperationCache::retainedBytes() const {
    const std::lock_guard lock(mutex_);
    return bytes_;
}
std::size_t OperationCache::retainedBytes(const OperationCacheEntryKind kind) const {
    const std::lock_guard lock(mutex_);
    return kind == OperationCacheEntryKind::DecodedMedia ? decodedMediaBytes_
                                                         : bytes_ - decodedMediaBytes_;
}
OperationCacheAccessStatistics OperationCache::statistics() const {
    const std::lock_guard lock(mutex_);
    return statistics_;
}
} // namespace bloom::runtime
