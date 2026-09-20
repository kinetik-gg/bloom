#pragma once

// Small, bounded, thread-safe LRU of already converted and frozen ImageSource/VideoSource uploads.
//
// The CPU evaluator's media caches retain the DECODED native frame, not the converted/premultiplied
// lin_rec709_scene image that the Layer Output stage consumes, so every evaluation of an unchanged
// source still re-runs the conversion. This store keys ONLY on the source semantic key -- validated
// asset/sequence/frame identity, interpretation and input/working colour space, configuration and
// processor revision, and the proxy/composition descriptor -- so:
//
//   * an unchanged source behind a changed layer transform is a hit and reuses the same immutable
//     allocation (the transform is not part of the key),
//   * a changed source, frame, colour interpretation or proxy is a miss and reconverts,
//   * an explicit `request.bypassOperationCache` never reads or writes it.
//
// It is charged by the actual pixel bytes of each frozen image AND by a hard entry-count ceiling,
// so neither the byte bound nor the metadata bound can be exceeded -- including on replacement of
// an existing key, which evicts other entries until the NEW size fits. The charge is PER ENTRY, not
// a deduplicated physical-memory measure: two distinct keys that happen to alias the same image
// allocation are charged twice, which over-states (never under-states) retained bytes and is the
// honest accounting for a keyed store. A budget of zero disables
// retention entirely (every store is refused and no entry is kept) rather than silently
// substituting a one-byte budget. The byte default is 128 MiB, large enough for one 1920x1080
// RGBA32F frame (33,177,600 B); the future service memory budget is expected to size it explicitly.
// It owns no GPU resource and never touches a decoder.

#include <bloom/render/image.hpp>
#include <bloom/runtime/gpu_memory_budget.hpp>

#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace bloom::runtime {

// The live default is capacity-aware (gpuPreparedUploadCacheByteBudget()): an ample-memory machine
// retains a large converted source instead of re-decoding it every request, and an explicitly tiny
// assigned budget stays tiny.
inline constexpr std::uint64_t kDefaultPreparedUploadCacheBytes = 128ULL * 1024ULL * 1024ULL;
inline constexpr std::size_t kDefaultPreparedUploadCacheEntries = 4096;

class GpuPreparedUploadCache final {
  public:
    explicit GpuPreparedUploadCache(std::uint64_t maxBytes = gpuPreparedUploadCacheByteBudget(),
                                    std::size_t maxEntries = kDefaultPreparedUploadCacheEntries);

    [[nodiscard]] std::shared_ptr<const render::Rgba32fImage> find(const std::string& key) noexcept;
    void store(std::string key, std::shared_ptr<const render::Rgba32fImage> image);

    [[nodiscard]] std::uint64_t retainedBytes() const;
    [[nodiscard]] std::size_t entryCount() const;
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
