#include "video_source.hpp"
#include <bloom/media/image.hpp>
#include <bloom/runtime/operation_cache.hpp>

#include <algorithm>
#include <chrono>
#include <iterator>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace bloom::runtime {
namespace {
std::size_t addCharge(const std::size_t left, const std::size_t right) noexcept {
    return left + std::min(right, std::numeric_limits<std::size_t>::max() - left);
}
} // namespace
struct OperationCache::Accounting {
    mutable std::mutex mutex;
    std::size_t budget = 0;
    std::size_t bytes = 0;
    std::size_t decodedBytes = 0;
    std::uint64_t decodedMediaGeneration = 0;
    std::size_t preparedBytes = 0;
    std::uint64_t preparedPayload = 0;
    std::size_t preparedCount = 0;
    std::size_t pendingCount = 0;
    std::size_t pendingCandidates = 0;
    std::size_t pendingBytes = 0;
    std::uint64_t pendingPayload = 0;
    bool preparedBound = false;
    bool preparedClosed = false;
    bool preparedReady = false;
    MemoryBudgetLedger::Clock::time_point validUntil{};
    std::optional<std::uint64_t> payloadCap;
    std::size_t entryCap = std::numeric_limits<std::size_t>::max();
    std::optional<std::size_t> configuredEntryCap;
    OperationCacheAccessStatistics preparedStatistics;
    std::shared_ptr<render::detail::PackedHostRetirementState> retirement;
    std::weak_ptr<detail::VideoSourceContext> videoSources;
};
OperationCache::Entry::~Entry() {
    // Rejected insertion also drops refs before its registration can permit owner exit.
    value.image.reset();
    value.sourceRevision.reset();
    value.values.clear();
    if (registered)
        render::detail::releasePackedHostRegistration(retirement);
}
OperationCache::OperationCache(const std::size_t budget, MemoryBudgetLedger& ledger)
    : ledger_(ledger), accounting_(std::make_shared<Accounting>()) {
    accounting_->budget = budget;
    const auto apply = [this](const std::size_t bytes) {
        // The ledger serializes callbacks. Read it before taking the cache mutex, never inverse.
        const auto delivered = ledger_.state();
        const bool pressure = delivered.retentionPercent < 100;
        const std::lock_guard lock(accounting_->mutex);
        accounting_->budget = bytes;
        accounting_->preparedReady =
            !delivered.awaitingProcessSample &&
            (delivered.sampleSerial == 0 ||
             (delivered.processSampleFresh &&
              (isCompleteProcessMemorySample(delivered.process) ||
               delivered.process.status == ProcessMemoryStatus::Unsupported)));
        accounting_->validUntil =
            delivered.sampleSerial == 0
                ? MemoryBudgetLedger::Clock::time_point::max()
                : (delivered.process.sampledAt >
                           MemoryBudgetLedger::Clock::time_point::max() - std::chrono::seconds(15)
                       ? MemoryBudgetLedger::Clock::time_point::max()
                       : delivered.process.sampledAt + std::chrono::seconds(15));
        if (pressure || (preparedCandidate_ && preparedCandidate_->bytes > bytes))
            retireCandidateLocked();
        std::uint64_t ordinaryDrops = 0;
        evictToLocked(bytes, pressure ? statistics_.pressureDrops : ordinaryDrops);
    };
    apply(budget);
    ledger_.registerCache(this, budget, [this] { return retainedBytes(); }, apply);
}
OperationCache::~OperationCache() {
    closePreparedUploads();
    ledger_.unregisterCache(this);
    // Rooted refs never free on this caller. The explicit host owner outlives queued callbacks.
    const std::lock_guard lock(accounting_->mutex);
    retireCandidateLocked();
    removeKindLocked(OperationCacheEntryKind::Operation);
    removeKindLocked(OperationCacheEntryKind::PreparedUpload);
    removeKindLocked(OperationCacheEntryKind::DecodedMedia);
}
std::optional<OperationCacheValue> OperationCache::find(const std::string& content,
                                                        const document::Revision revision) {
    const std::lock_guard lock(accounting_->mutex);
    const Key key{content, false};
    const auto sourceUsable = [&](const Position position) {
        const auto& entry = **position;
        if (entry.value.sourceRevision && entry.registered &&
            (accounting_->preparedClosed || !accounting_->preparedReady ||
             MemoryBudgetLedger::Clock::now() > accounting_->validUntil)) {
            removeLocked(position);
            ++statistics_.misses;
            return false;
        }
        return true;
    };
    const auto exact = addresses_.find({revision, key});
    if (exact != addresses_.end()) {
        if (!sourceUsable(exact->second))
            return std::nullopt;
        touch(exact->second);
        ++statistics_.hits;
        return (*exact->second)->value;
    }
    const auto found = index_.find(key);
    if (found == index_.end()) {
        ++statistics_.misses;
        return std::nullopt;
    }
    if (!sourceUsable(found->second))
        return std::nullopt;
    auto& entry = **found->second;
    addresses_.emplace(Address{revision, {entry.content, false}}, found->second);
    addresses_.erase({entry.revision, key});
    entry.revision = revision;
    touch(found->second);
    ++statistics_.hits;
    return entry.value;
}
void OperationCache::insertLocked(std::unique_ptr<Entry>& entry) {
    const bool prepared = entry->kind == OperationCacheEntryKind::PreparedUpload;
    try {
        entries_.push_front(std::move(entry));
    } catch (const std::bad_alloc&) {
        ++statistics_.allocationFailures;
        return;
    }
    bool indexed = false;
    try {
        auto& placed = *entries_.front();
        const Key key{placed.content, prepared};
        const auto [indexPosition, indexInserted] = index_.emplace(key, entries_.begin());
        indexed = indexInserted;
        if (!indexInserted) {
            entry = std::move(entries_.front());
            entries_.pop_front();
            return;
        }
        const bool addressInserted =
            addresses_.emplace(Address{placed.revision, key}, entries_.begin()).second;
        if (!addressInserted) {
            index_.erase(indexPosition);
            entry = std::move(entries_.front());
            entries_.pop_front();
            return;
        }
    } catch (const std::bad_alloc&) {
        if (indexed)
            index_.erase(Key{entries_.front()->content, prepared});
        entry = std::move(entries_.front());
        entries_.pop_front();
        ++statistics_.allocationFailures;
        return;
    } catch (...) {
        if (indexed)
            index_.erase(Key{entries_.front()->content, prepared});
        entry = std::move(entries_.front());
        entries_.pop_front();
        throw;
    }
    auto& placed = *entries_.front();
    accounting_->bytes += placed.bytes;
    if (prepared) {
        accounting_->preparedBytes += placed.bytes;
        accounting_->preparedPayload += placed.value.image->pixels().size_bytes();
        ++accounting_->preparedCount;
    } else if (placed.kind == OperationCacheEntryKind::DecodedMedia) {
        accounting_->decodedBytes += placed.bytes;
    }
}
void OperationCache::store(std::string content, const document::Revision revision,
                           OperationCacheValue value, const OperationCacheEntryKind kind,
                           const std::optional<std::uint64_t> decodedMediaGeneration) {
    // Prepared admission needs its explicit owner and subset policy, not this ordinary API.
    if (kind == OperationCacheEntryKind::PreparedUpload)
        throw std::invalid_argument("prepared entries require storePreparedUpload");
    if (value.sourceRevision && kind != OperationCacheEntryKind::DecodedMedia)
        throw std::invalid_argument("source revisions require decoded-media ownership");
    std::unique_ptr<Entry> entry;
    try {
        entry = std::make_unique<Entry>();
    } catch (const std::bad_alloc&) {
        const std::lock_guard lock(accounting_->mutex);
        ++statistics_.allocationFailures;
        return;
    }
    entry->content = std::move(content);
    entry->revision = revision;
    entry->kind = kind;
    entry->value = std::move(value);
    entry->bytes =
        addCharge(sizeof(Entry) + sizeof(Address) + sizeof(void*) * 16, entry->content.capacity());
    if (entry->value.image)
        entry->bytes =
            addCharge(entry->bytes, addCharge(sizeof(render::Rgba32fImage),
                                              entry->value.image->pixels().size_bytes()));
    entry->bytes = addCharge(entry->bytes, entry->value.values.capacity() * sizeof(CompiledValue));
    if (entry->value.sourceRevision)
        entry->bytes = addCharge(entry->bytes, entry->value.sourceRevision->retainedBytes());
    for (const auto& item : entry->value.values)
        if (const auto* string = std::get_if<std::string>(&item))
            entry->bytes = addCharge(entry->bytes, string->capacity());
    // Worker-side stores register every payload when this pool has an explicit host owner.
    // Standalone CPU pools preserve their synchronous ownership; rooted image/value and source
    // payloads all use the same admission-independent retirement lane.
    {
        const std::lock_guard lock(accounting_->mutex);
        if (accounting_->preparedClosed)
            return;
        entry->retirement = accounting_->retirement;
    }
    if (entry->retirement) {
        if (!render::detail::preparePackedHostRetirement(entry->retirement) ||
            !render::detail::registerPackedHostAllocation(entry->retirement))
            return;
        entry->registered = true;
        entry->accounting = accounting_;
        entry->destroy =
            entry->value.sourceRevision ? destroySourceRevisionEntry : destroyOrdinaryEntry;
    }
    {
        const std::lock_guard lock(accounting_->mutex);
        if (accounting_->preparedClosed)
            return;
        if (entry->value.sourceRevision &&
            (!decodedMediaGeneration ||
             *decodedMediaGeneration != accounting_->decodedMediaGeneration ||
             accounting_->decodedMediaGeneration == std::numeric_limits<std::uint64_t>::max() ||
             (entry->registered && (!accounting_->preparedReady ||
                                    MemoryBudgetLedger::Clock::now() > accounting_->validUntil))))
            return;
        if (entry->bytes > accounting_->budget || index_.contains({entry->content, false}))
            return;
        ++accessEpoch_;
        entry->protectedUntil = kind == OperationCacheEntryKind::DecodedMedia
                                    ? accessEpoch_ + kDecodedMediaGraceAccesses
                                    : 0;
        std::uint64_t ignored = 0;
        if (accounting_->preparedCount == 0 && accounting_->pendingBytes == 0 &&
            entry->bytes <= std::numeric_limits<std::size_t>::max() - accounting_->bytes) {
            // Preserve ordinary-only grace: the incoming Operation itself can be the victim.
            insertLocked(entry);
            evictToLocked(accounting_->budget, ignored);
            return;
        }
        evictToLocked(accounting_->budget - entry->bytes, ignored);
        if (accounting_->bytes > accounting_->budget - entry->bytes)
            return; // Still-charged pending prepared refs cannot donate capacity.
        insertLocked(entry);
    } // Rejected Entry/image/registration destruction is outside the cache mutex.
}
void OperationCache::destroyPreparedEntry(render::detail::PackedHostRetirementNode* node) noexcept {
    auto* entry = static_cast<Entry*>(node);
    const auto accounting = entry->accounting;
    const auto bytes = entry->bytes;
    const auto payload = entry->value.image->pixels().size_bytes();
    const bool retiredCandidate = entry->retiredCandidate;
    entry->registered = false; // The generic worker releases its registration after this callback.
    delete entry; // Cache reference and metadata free before returning their logical retention
                  // charge.
    const std::lock_guard lock(accounting->mutex);
    accounting->bytes -= bytes;
    accounting->preparedBytes -= bytes;
    accounting->preparedPayload -= payload;
    --accounting->preparedCount;
    --accounting->pendingCount;
    accounting->pendingBytes -= bytes;
    accounting->pendingPayload -= payload;
    if (retiredCandidate)
        --accounting->pendingCandidates;
}
void OperationCache::destroySourceRevisionEntry(
    render::detail::PackedHostRetirementNode* node) noexcept {
    auto* entry = static_cast<Entry*>(node);
    const auto accounting = entry->accounting;
    const auto bytes = entry->bytes;
    entry->registered = false; // The generic worker releases registration after this callback.
    delete entry; // Drop encoded backing and metadata before folding back the logical debit.
    const std::lock_guard lock(accounting->mutex);
    accounting->bytes -= bytes;
    accounting->decodedBytes -= bytes;
    accounting->pendingBytes -= bytes;
}
void OperationCache::destroyOrdinaryEntry(render::detail::PackedHostRetirementNode* node) noexcept {
    auto* entry = static_cast<Entry*>(node);
    const auto accounting = entry->accounting;
    const auto bytes = entry->bytes;
    const auto decoded = entry->kind == OperationCacheEntryKind::DecodedMedia;
    entry->registered = false;
    delete entry;
    const std::lock_guard lock(accounting->mutex);
    accounting->bytes -= bytes;
    if (decoded)
        accounting->decodedBytes -= bytes;
    accounting->pendingBytes -= bytes;
}
void OperationCache::removeLocked(const Position candidate) {
    auto& entry = **candidate;
    const bool prepared = entry.kind == OperationCacheEntryKind::PreparedUpload;
    const Key key{entry.content, prepared};
    addresses_.erase({entry.revision, key});
    index_.erase(key);
    if (prepared) {
        ++accounting_->pendingCount;
        accounting_->pendingBytes += entry.bytes;
        accounting_->pendingPayload += entry.value.image->pixels().size_bytes();
        auto* node = candidate->release();
        entries_.erase(candidate);
        const auto retirement = node->retirement;
        render::detail::enqueuePackedHostRetirement(retirement, node);
    } else if (entry.registered) {
        accounting_->pendingBytes += entry.bytes;
        auto* node = candidate->release();
        entries_.erase(candidate);
        const auto retirement = node->retirement;
        render::detail::enqueuePackedHostRetirement(retirement, node);
    } else {
        accounting_->bytes -= entry.bytes;
        if (entry.kind == OperationCacheEntryKind::DecodedMedia)
            accounting_->decodedBytes -= entry.bytes;
        entries_.erase(candidate);
    }
}
void OperationCache::removeKindLocked(const OperationCacheEntryKind kind) {
    for (auto entry = entries_.begin(); entry != entries_.end();) {
        const auto next = std::next(entry);
        if ((*entry)->kind == kind)
            removeLocked(entry);
        entry = next;
    }
}
void OperationCache::evictToLocked(const std::size_t limit, std::uint64_t& counter) {
    while (accounting_->bytes - accounting_->pendingBytes > limit && !entries_.empty()) {
        removeLocked(evictionCandidate());
        ++counter;
    }
}
void OperationCache::trimToBytes(const std::size_t bytes) {
    const std::lock_guard lock(accounting_->mutex);
    retireCandidateLocked();
    evictToLocked(std::min(bytes, accounting_->budget), statistics_.pressureDrops);
}
void OperationCache::clearOperations() {
    const std::lock_guard lock(accounting_->mutex);
    removeKindLocked(OperationCacheEntryKind::Operation);
}
void OperationCache::clearDecodedMedia() {
    std::shared_ptr<detail::VideoSourceContext> video;
    {
        const std::lock_guard lock(accounting_->mutex);
        if (accounting_->decodedMediaGeneration != std::numeric_limits<std::uint64_t>::max())
            ++accounting_->decodedMediaGeneration;
        removeKindLocked(OperationCacheEntryKind::DecodedMedia);
        removeKindLocked(OperationCacheEntryKind::PreparedUpload);
        retireCandidateLocked();
        video = accounting_->videoSources.lock();
    }
    if (video)
        video->clear();
}
void OperationCache::invalidateSourceRevisions() {
    std::shared_ptr<detail::VideoSourceContext> video;
    {
        const std::lock_guard lock(accounting_->mutex);
        if (accounting_->decodedMediaGeneration != std::numeric_limits<std::uint64_t>::max())
            ++accounting_->decodedMediaGeneration;
        for (auto entry = entries_.begin(); entry != entries_.end();) {
            const auto next = std::next(entry);
            if ((*entry)->value.sourceRevision)
                removeLocked(entry);
            entry = next;
        }
        video = accounting_->videoSources.lock();
    }
    if (video)
        video->invalidateSources();
}
std::uint64_t OperationCache::decodedMediaGeneration() const {
    const std::lock_guard lock(accounting_->mutex);
    return accounting_->decodedMediaGeneration;
}
void OperationCache::touch(const Position position) {
    ++accessEpoch_;
    (*position)->protectedUntil = (*position)->kind == OperationCacheEntryKind::DecodedMedia
                                      ? accessEpoch_ + kDecodedMediaGraceAccesses
                                      : 0;
    entries_.splice(entries_.begin(), entries_, position);
}
OperationCache::Position OperationCache::evictionCandidate() {
    for (auto candidate = entries_.end(); candidate != entries_.begin();) {
        --candidate;
        if ((*candidate)->kind != OperationCacheEntryKind::DecodedMedia ||
            accessEpoch_ >= (*candidate)->protectedUntil)
            return candidate;
    }
    return std::prev(entries_.end());
}
void OperationCache::setByteBudget(const std::size_t budget) {
    static_cast<void>(ledger_.setCacheCeiling(this, budget));
}
std::size_t OperationCache::byteBudget() const {
    const std::lock_guard lock(accounting_->mutex);
    return accounting_->budget;
}
std::size_t OperationCache::retainedBytes() const {
    const std::lock_guard lock(accounting_->mutex);
    return accounting_->bytes;
}
std::size_t OperationCache::retainedBytes(const OperationCacheEntryKind kind) const {
    const std::lock_guard lock(accounting_->mutex);
    switch (kind) {
    case OperationCacheEntryKind::Operation:
        return accounting_->bytes - accounting_->decodedBytes - accounting_->preparedBytes;
    case OperationCacheEntryKind::DecodedMedia:
        return accounting_->decodedBytes;
    case OperationCacheEntryKind::PreparedUpload:
        return accounting_->preparedBytes;
    }
    return 0;
}
OperationCacheAccessStatistics OperationCache::statistics() const {
    const std::lock_guard lock(accounting_->mutex);
    return statistics_;
}
void OperationCache::bindPreparedUploads(
    const std::shared_ptr<render::PackedHostRetirementOwner>& owner,
    const std::optional<std::uint64_t> payloadCap, const std::optional<std::size_t> entryCap) {
    const auto retirement = owner ? owner->producerState() : nullptr;
    const std::lock_guard lock(accounting_->mutex);
    if (accounting_->preparedBound) {
        if ((retirement && retirement != accounting_->retirement) ||
            (payloadCap && payloadCap != accounting_->payloadCap) ||
            (entryCap && *entryCap != accounting_->entryCap))
            throw std::invalid_argument("incompatible shared prepared retention policy");
        return;
    }
    if (!retirement)
        throw std::invalid_argument("prepared retention requires explicit host owner");
    accounting_->retirement = retirement;
    accounting_->payloadCap = payloadCap;
    accounting_->entryCap = entryCap.value_or(std::numeric_limits<std::size_t>::max());
    accounting_->configuredEntryCap = entryCap;
    accounting_->preparedBound = true;
}
std::shared_ptr<render::detail::PackedHostRetirementState>
OperationCache::sourceRetirementState() const {
    const std::lock_guard lock(accounting_->mutex);
    return accounting_->retirement;
}
void OperationCache::bindVideoSources(const std::shared_ptr<detail::VideoSourceContext>& sources) {
    bool closed = false;
    {
        const std::lock_guard lock(accounting_->mutex);
        accounting_->videoSources = sources;
        closed = accounting_->preparedClosed;
    }
    if (closed && sources)
        sources->beginShutdown();
}
std::shared_ptr<const render::Rgba32fImage>
OperationCache::findPreparedUpload(const std::string& content) {
    const std::lock_guard lock(accounting_->mutex);
    promoteCandidateLocked();
    const auto found = index_.find({content, true});
    if (found == index_.end()) {
        ++accounting_->preparedStatistics.misses;
        return nullptr;
    }
    touch(found->second);
    ++accounting_->preparedStatistics.hits;
    return (*found->second)->value.image;
}
void OperationCache::retireCandidateLocked() {
    if (!preparedCandidate_)
        return;
    auto* const node = preparedCandidate_.release();
    node->retiredCandidate = true;
    ++accounting_->pendingCount;
    ++accounting_->pendingCandidates;
    accounting_->pendingBytes += node->bytes;
    accounting_->pendingPayload += node->value.image->pixels().size_bytes();
    const auto retirement = node->retirement;
    render::detail::enqueuePackedHostRetirement(retirement, node);
}
void OperationCache::promoteCandidateLocked() {
    if (!preparedCandidate_)
        return;
    const auto payload = preparedCandidate_->value.image->pixels().size_bytes();
    const auto cap = accounting_->payloadCap.value_or(std::numeric_limits<std::uint64_t>::max());
    if (accounting_->preparedClosed || !accounting_->preparedReady ||
        MemoryBudgetLedger::Clock::now() > accounting_->validUntil ||
        preparedCandidate_->bytes > accounting_->budget || payload > cap ||
        accounting_->entryCap == 0) {
        retireCandidateLocked();
        return;
    }
    // The candidate's own charge is already included in these totals. Pending victims donate
    // capacity only after their worker has deleted them and folded their charges back.
    if (accounting_->bytes > accounting_->budget || accounting_->preparedPayload > cap ||
        accounting_->preparedCount > accounting_->entryCap)
        return;
    auto candidate = std::move(preparedCandidate_);
    accounting_->bytes -= candidate->bytes;
    accounting_->preparedBytes -= candidate->bytes;
    accounting_->preparedPayload -= payload;
    --accounting_->preparedCount;
    const auto restoreCandidate = [&] {
        if (!candidate)
            return;
        accounting_->bytes += candidate->bytes;
        accounting_->preparedBytes += candidate->bytes;
        accounting_->preparedPayload += payload;
        ++accounting_->preparedCount;
        preparedCandidate_ = std::move(candidate);
    };
    try {
        insertLocked(candidate);
    } catch (...) {
        // An exceptional insertion returns the Entry to candidate. Restore its exact charge and
        // ownership so a later lookup can retry; the cache facade must not expose the failure.
        restoreCandidate();
        return;
    }
    restoreCandidate();
}
void OperationCache::storePreparedUpload(std::string content,
                                         std::shared_ptr<const render::Rgba32fImage> image) {
    if (!image || image->pixels().empty())
        return;
    std::shared_ptr<render::detail::PackedHostRetirementState> retirement;
    {
        const std::lock_guard lock(accounting_->mutex);
        if (accounting_->preparedClosed)
            return;
        retirement = accounting_->retirement;
    }
    if (!render::detail::preparePackedHostRetirement(retirement))
        return;
    std::unique_ptr<Entry> entry;
    try {
        entry = std::make_unique<Entry>();
    } catch (const std::bad_alloc&) {
        return;
    }
    entry->content = std::move(content);
    entry->value.image = std::move(image);
    entry->kind = OperationCacheEntryKind::PreparedUpload;
    entry->bytes =
        addCharge(sizeof(Entry) + sizeof(Address) + sizeof(void*) * 16, entry->content.capacity());
    const auto payload = entry->value.image->pixels().size_bytes();
    entry->bytes = addCharge(entry->bytes, addCharge(sizeof(render::Rgba32fImage), payload));
    entry->accounting = accounting_;
    entry->retirement = retirement;
    entry->destroy = destroyPreparedEntry;
    if (!render::detail::registerPackedHostAllocation(retirement))
        return;
    entry->registered = true;
    {
        const std::lock_guard lock(accounting_->mutex);
        promoteCandidateLocked();
        const auto cap =
            accounting_->payloadCap.value_or(std::numeric_limits<std::uint64_t>::max());
        if (accounting_->preparedClosed || !accounting_->preparedReady ||
            MemoryBudgetLedger::Clock::now() > accounting_->validUntil ||
            entry->bytes > accounting_->budget || payload > cap || accounting_->entryCap == 0)
            return;
        if (preparedCandidate_)
            return;
        const auto found = index_.find({entry->content, true});
        if (found != index_.end() && (*found->second)->value.image == entry->value.image) {
            touch(found->second);
            return; // Same immutable backing is already retained, not a replacement.
        }
        if (accounting_->pendingCandidates != 0)
            return; // One retiring candidate is the bound; no over-budget queue.
        if (found != index_.end())
            removeLocked(found->second);
        while (!entries_.empty() &&
               (accounting_->preparedPayload - accounting_->pendingPayload > cap - payload ||
                accounting_->preparedCount - accounting_->pendingCount >= accounting_->entryCap)) {
            auto victim = std::find_if(entries_.rbegin(), entries_.rend(), [](const auto& item) {
                return item->kind == OperationCacheEntryKind::PreparedUpload;
            });
            if (victim == entries_.rend())
                break;
            removeLocked(std::prev(victim.base()));
        }
        std::uint64_t ignored = 0;
        evictToLocked(accounting_->budget - entry->bytes, ignored);
        if (accounting_->bytes > accounting_->budget - entry->bytes ||
            accounting_->preparedPayload > cap - payload ||
            accounting_->preparedCount >= accounting_->entryCap) {
            // The producer already owns this decoded image. Retain ONE candidate so the cache
            // can admit it after actual retirement, while charging the extra cache reference and
            // metadata immediately. The transient total may exceed the configured ceiling by
            // at most this single individually eligible candidate; it is never reported as free.
            const bool pendingCanMakeRoom =
                accounting_->pendingBytes > 0 &&
                accounting_->bytes - accounting_->pendingBytes <=
                    accounting_->budget - entry->bytes &&
                accounting_->preparedPayload - accounting_->pendingPayload <= cap - payload &&
                accounting_->preparedCount - accounting_->pendingCount < accounting_->entryCap &&
                entry->bytes <= std::numeric_limits<std::size_t>::max() - accounting_->bytes &&
                payload <= std::numeric_limits<std::uint64_t>::max() - accounting_->preparedPayload;
            if (pendingCanMakeRoom) {
                accounting_->bytes += entry->bytes;
                accounting_->preparedBytes += entry->bytes;
                accounting_->preparedPayload += payload;
                ++accounting_->preparedCount;
                preparedCandidate_ = std::move(entry);
            }
            return;
        }
        insertLocked(entry);
    }
}
void OperationCache::clearPreparedUploads() {
    const std::lock_guard lock(accounting_->mutex);
    retireCandidateLocked();
    removeKindLocked(OperationCacheEntryKind::PreparedUpload);
}
void OperationCache::closePreparedUploads() {
    std::shared_ptr<detail::VideoSourceContext> video;
    {
        const std::lock_guard lock(accounting_->mutex);
        accounting_->preparedClosed = true;
        retireCandidateLocked();
        removeKindLocked(OperationCacheEntryKind::PreparedUpload);
        // Every rooted ordinary entry must leave before the shared host owner can acknowledge
        // shutdown. Otherwise an Operation/decoded image could keep its registration forever.
        removeKindLocked(OperationCacheEntryKind::DecodedMedia);
        removeKindLocked(OperationCacheEntryKind::Operation);
        video = accounting_->videoSources.lock();
    }
    if (video)
        video->beginShutdown();
}
bool OperationCache::preparedUploadsBound() const {
    const std::lock_guard lock(accounting_->mutex);
    return accounting_->preparedBound;
}
PreparedUploadRetentionPolicy OperationCache::preparedRetentionPolicy() const {
    const std::lock_guard lock(accounting_->mutex);
    return {accounting_->payloadCap, accounting_->configuredEntryCap};
}
std::uint64_t OperationCache::preparedPayloadBytes() const {
    const std::lock_guard lock(accounting_->mutex);
    return accounting_->preparedPayload;
}
std::size_t OperationCache::preparedEntryCount() const {
    const std::lock_guard lock(accounting_->mutex);
    return accounting_->preparedCount - accounting_->pendingCount -
           static_cast<std::size_t>(preparedCandidate_ != nullptr);
}
std::uint64_t OperationCache::preparedPayloadCeiling() const {
    const std::lock_guard lock(accounting_->mutex);
    return std::min<std::uint64_t>(
        accounting_->budget,
        accounting_->payloadCap.value_or(std::numeric_limits<std::uint64_t>::max()));
}
std::size_t OperationCache::preparedEntryCeiling() const {
    const std::lock_guard lock(accounting_->mutex);
    return accounting_->entryCap;
}
OperationCacheAccessStatistics OperationCache::preparedStatistics() const {
    const std::lock_guard lock(accounting_->mutex);
    return accounting_->preparedStatistics;
}
} // namespace bloom::runtime
