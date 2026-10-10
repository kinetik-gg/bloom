#pragma once

#include <bloom/render/image.hpp>
#include <bloom/render/packed_host_retirement.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/memory_budget_ledger.hpp>

#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace bloom::runtime {

enum class OperationCacheEntryKind : std::uint8_t {
    Operation,
    DecodedMedia,
    PreparedUpload,
};

struct PreparedUploadRetentionPolicy final {
    std::optional<std::uint64_t> payloadBytes;
    std::optional<std::size_t> entries;
    friend bool operator==(const PreparedUploadRetentionPolicy&,
                           const PreparedUploadRetentionPolicy&) = default;
};

struct OperationCacheAccessStatistics final {
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    // CACHEFIX-1: entries the cache refused because storing them threw std::bad_alloc. The insert
    // is dropped and the caller's frame still renders; this counter is how a test and a diagnostic
    // build can see that it happened at all.
    std::uint64_t allocationFailures = 0;
    // Entries dropped by a memory-pressure trim, as opposed to ordinary budget eviction.
    std::uint64_t pressureDrops = 0;

    friend bool operator==(const OperationCacheAccessStatistics&,
                           const OperationCacheAccessStatistics&) = default;
};

struct OperationCacheStatistics final {
    std::size_t hits = 0;
    std::size_t misses = 0;
    std::vector<document::NodeId> evaluatedNodes;
};
struct OperationCacheValue final {
    std::shared_ptr<const render::Rgba32fImage> image;
    std::vector<CompiledValue> values;
    EvaluatedOperationBounds bounds{};
};

// Owned by the evaluator session. Concurrent preview/export tasks share this bounded LRU.
class OperationCache final {
  public:
    explicit OperationCache(std::size_t budget = defaultOperationCacheByteBudget(),
                            MemoryBudgetLedger& ledger = processMemoryBudgetLedger());
    ~OperationCache();
    [[nodiscard]] std::optional<OperationCacheValue> find(const std::string& content,
                                                          document::Revision revision);
    void store(std::string content, document::Revision revision, OperationCacheValue value,
               OperationCacheEntryKind kind = OperationCacheEntryKind::Operation);
    void setByteBudget(std::size_t budget);
    [[nodiscard]] std::size_t byteBudget() const;
    [[nodiscard]] std::size_t retainedBytes() const;
    // CACHEFIX-1 accounting: the same total, split by what is holding it. Decoded media is the
    // expensive half (one 4K RGBA32F frame is 126.6 MiB), and it is the half a grace period keeps
    // alive, so an audit that cannot separate the two cannot say where the bytes went.
    [[nodiscard]] std::size_t retainedBytes(OperationCacheEntryKind kind) const;
    // Explicit one-shot trim without changing admission. The ledger pressure callback separately
    // lowers admission and evicts atomically; configured ceilings remain in the ledger.
    void trimToBytes(std::size_t bytes);
    // "Purge preview cache": removes every derived OPERATION entry, leaving decoded-media entries
    // alone. A consumer that already holds a value keeps it alive -- a value is a shared_ptr -- so
    // clearing here never invalidates work already in progress; it only forces the next request to
    // re-evaluate. Lifetime access counters are left intact.
    void clearOperations();
    // "Purge media cache": removes every DECODED-MEDIA entry, leaving derived operation results
    // alone. The same shared_ptr rule applies: no live product is invalidated, only re-derived.
    void clearDecodedMedia();
    [[nodiscard]] OperationCacheAccessStatistics statistics() const;

    // One immutable prepared subset policy, on this pool's existing retention registration.
    // Default attachments preserve the first binding; incompatible explicit bindings throw.
    void bindPreparedUploads(const std::shared_ptr<render::PackedHostRetirementOwner>& owner,
                             std::optional<std::uint64_t> payloadCap = {},
                             std::optional<std::size_t> entryCap = {});
    [[nodiscard]] std::shared_ptr<const render::Rgba32fImage>
    findPreparedUpload(const std::string& content);
    // Worker-only: prepares/registers on the existing host retirement owner before insertion.
    void storePreparedUpload(std::string content,
                             std::shared_ptr<const render::Rgba32fImage> image);
    void clearPreparedUploads();
    // Irreversible producer close, serialized with insertion; binding remains rooted.
    void closePreparedUploads();
    [[nodiscard]] bool preparedUploadsBound() const;
    [[nodiscard]] PreparedUploadRetentionPolicy preparedRetentionPolicy() const;
    [[nodiscard]] std::uint64_t preparedPayloadBytes() const;
    [[nodiscard]] std::size_t preparedEntryCount() const;
    [[nodiscard]] std::uint64_t preparedPayloadCeiling() const;
    [[nodiscard]] std::size_t preparedEntryCeiling() const;
    [[nodiscard]] OperationCacheAccessStatistics preparedStatistics() const;

  private:
    static constexpr std::uint64_t kDecodedMediaGraceAccesses = 8;
    struct Accounting;
    struct Key {
        std::string_view content;
        bool prepared = false;
        friend bool operator==(const Key&, const Key&) = default;
    };
    struct KeyHash {
        std::size_t operator()(const Key& key) const noexcept {
            return std::hash<std::string_view>{}(key.content) ^ std::hash<bool>{}(key.prepared);
        }
    };
    struct Address {
        document::Revision revision;
        Key key;
        friend bool operator==(const Address&, const Address&) = default;
    };
    struct AddressHash {
        std::size_t operator()(const Address& address) const noexcept {
            return KeyHash{}(address.key) ^ std::hash<std::uint64_t>{}(address.revision.value());
        }
    };
    struct Entry : render::detail::PackedHostRetirementNode {
        ~Entry();
        std::string content;
        document::Revision revision;
        OperationCacheValue value;
        std::size_t bytes = 0;
        OperationCacheEntryKind kind = OperationCacheEntryKind::Operation;
        std::uint64_t protectedUntil = 0;
        bool registered = false;
        std::shared_ptr<Accounting> accounting;
        std::shared_ptr<render::detail::PackedHostRetirementState> retirement;
    };
    using Entries = std::list<std::unique_ptr<Entry>>;
    using Position = Entries::iterator;
    static void destroyPreparedEntry(render::detail::PackedHostRetirementNode*) noexcept;
    void touch(Position entry);
    void evictToLocked(std::size_t limit, std::uint64_t& counter);
    void removeLocked(Position candidate);
    void removeKindLocked(OperationCacheEntryKind kind);
    [[nodiscard]] Position evictionCandidate();
    void insertLocked(std::unique_ptr<Entry>& entry);

    MemoryBudgetLedger& ledger_;
    std::shared_ptr<Accounting> accounting_;
    std::uint64_t accessEpoch_ = 0;
    OperationCacheAccessStatistics statistics_;
    Entries entries_;
    std::unordered_map<Key, Position, KeyHash> index_;
    std::unordered_map<Address, Position, AddressHash> addresses_;
};
} // namespace bloom::runtime
