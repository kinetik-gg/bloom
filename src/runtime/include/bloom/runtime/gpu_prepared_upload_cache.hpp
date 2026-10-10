#pragma once

// Host source-preparation adapter. Rooted construction shares the OperationCache/source LRU,
// its live same-ledger retention assignment, metadata accounting and explicit host retirement
// owner. It has no second owning LRU or participant. Explicit byte/count caps are additional
// immutable subset ceilings; omitted later attachments preserve the pool's existing policy.
// Stores are worker-only; hits preserve source-semantic identity and bypass remains caller-owned.
// The numeric constructor is the separate fixed standalone legacy arm, not live-root authority.

#include <bloom/render/image.hpp>
#include <bloom/runtime/gpu_memory_budget.hpp>
#include <bloom/runtime/operation_cache.hpp>

#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace bloom::runtime {

// Fixed standalone legacy controls. Live rooted retention comes only from the bound shared pool.
inline constexpr std::uint64_t kDefaultPreparedUploadCacheBytes = 128ULL * 1024ULL * 1024ULL;
inline constexpr std::size_t kDefaultPreparedUploadCacheEntries = 4096;

class GpuPreparedUploadCache final {
  public:
    explicit GpuPreparedUploadCache(
        std::shared_ptr<OperationCache> pool,
        const std::shared_ptr<render::PackedHostRetirementOwner>& retirementOwner = {},
        std::optional<std::uint64_t> maxBytes = {}, std::optional<std::size_t> maxEntries = {});
    explicit GpuPreparedUploadCache(std::uint64_t maxBytes = gpuPreparedUploadCacheByteBudget(),
                                    std::size_t maxEntries = kDefaultPreparedUploadCacheEntries);

    [[nodiscard]] std::shared_ptr<const render::Rgba32fImage> find(const std::string& key) noexcept;
    void store(std::string key, std::shared_ptr<const render::Rgba32fImage> image);
    // Drops every retained upload, releasing the cache's own references. A caller already holding a
    // converted image keeps it valid; only re-derivation is forced. Lifetime hit/miss counters are
    // left intact. Thread-safe.
    void clear();

    [[nodiscard]] std::uint64_t retainedBytes() const;
    [[nodiscard]] std::size_t entryCount() const;
    // Immutable declared subset, separate from the live assigned maxBytes().
    [[nodiscard]] PreparedUploadRetentionPolicy configuredPolicy() const;
    [[nodiscard]] std::uint64_t maxBytes() const;
    [[nodiscard]] std::size_t maxEntries() const;
    [[nodiscard]] std::uint64_t hits() const;
    [[nodiscard]] std::uint64_t misses() const;

  private:
    struct Entry final {
        std::shared_ptr<const render::Rgba32fImage> image;
        std::uint64_t bytes = 0;
        std::list<std::string>::iterator position;
    };

    void evictToFitLocked(std::uint64_t incomingBytes, std::size_t reservedEntries);

    std::shared_ptr<OperationCache> pool_;
    mutable std::mutex mutex_;
    std::uint64_t maxBytes_;
    std::size_t maxEntries_;
    std::uint64_t retainedBytes_ = 0;
    std::uint64_t hits_ = 0;
    std::uint64_t misses_ = 0;
    std::list<std::string> order_;
    std::unordered_map<std::string_view, Entry> entries_;
};

} // namespace bloom::runtime
