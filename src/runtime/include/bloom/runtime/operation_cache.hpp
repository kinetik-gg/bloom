#pragma once

#include <bloom/render/image.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace bloom::runtime {
inline constexpr std::size_t kDefaultOperationCacheBytes = 1024ULL * 1024 * 1024;
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
    explicit OperationCache(std::size_t budget = kDefaultOperationCacheBytes) : budget_(budget) {}
    [[nodiscard]] std::optional<OperationCacheValue> find(const std::string& content,
                                                          document::Revision revision);
    void store(std::string content, document::Revision revision, OperationCacheValue value);
    void setByteBudget(std::size_t budget);
    [[nodiscard]] std::size_t retainedBytes() const;

  private:
    struct Address {
        document::Revision revision;
        std::string_view content;
        friend bool operator==(const Address&, const Address&) = default;
    };
    struct AddressHash {
        std::size_t operator()(const Address& address) const noexcept {
            return std::hash<std::string_view>{}(address.content) ^
                   std::hash<std::uint64_t>{}(address.revision.value());
        }
    };
    struct Entry {
        std::string content;
        document::Revision revision;
        OperationCacheValue value;
        std::size_t bytes;
    };
    void evict();
    mutable std::mutex mutex_;
    std::size_t budget_;
    std::size_t bytes_ = 0;
    std::list<Entry> entries_;
    std::unordered_map<std::string_view, std::list<Entry>::iterator> index_;
    std::unordered_map<Address, std::list<Entry>::iterator, AddressHash> addresses_;
};
} // namespace bloom::runtime
