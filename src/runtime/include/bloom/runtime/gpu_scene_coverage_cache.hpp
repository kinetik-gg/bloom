#pragma once

// Small, bounded, thread-safe LRU cache of immutable 8-bit coverage rasters produced by the
// EXISTING CPU PathRaster for fractional-translation solids. It keys ONLY on the raster geometry
// (the exact chain matrix, proxy scales, rectangle dimensions, output window and pixel aspect), so
// a changed colour or opacity does not reraster, and a changed transform does. Budget is charged by
// the actual R8 byte size; an entry over the whole budget is refused. This is a CPU-side cache; it
// owns no GPU resource and never holds a full RGBA image. The colour/opacity identity lives in the
// prepared command's pixel key, never here.

#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace bloom::runtime {

class GpuSceneCoverageCache final {
  public:
    explicit GpuSceneCoverageCache(std::uint64_t maxBytes = 32ULL * 1024ULL * 1024ULL);

    [[nodiscard]] std::shared_ptr<const std::vector<std::uint8_t>>
    find(const std::string& geometryKey) noexcept;
    void store(std::string geometryKey, std::shared_ptr<const std::vector<std::uint8_t>> coverage);

    [[nodiscard]] std::uint64_t retainedBytes() const;
    [[nodiscard]] std::size_t entryCount() const;
    [[nodiscard]] std::uint64_t hits() const;
    [[nodiscard]] std::uint64_t misses() const;

  private:
    struct Entry final {
        std::shared_ptr<const std::vector<std::uint8_t>> coverage;
        std::uint64_t bytes = 0;
        std::list<std::string>::iterator position;
    };

    mutable std::mutex mutex_;
    std::uint64_t maxBytes_;
    std::uint64_t retainedBytes_ = 0;
    std::uint64_t hits_ = 0;
    std::uint64_t misses_ = 0;
    std::list<std::string> order_;
    std::unordered_map<std::string_view, Entry> entries_;
};

} // namespace bloom::runtime
