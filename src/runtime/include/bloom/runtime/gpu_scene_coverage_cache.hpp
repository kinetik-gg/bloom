#pragma once

// Small, bounded, thread-safe LRU cache of immutable bounded coverage geometry produced by the
// EXISTING CPU PathRaster::coverageGeometry for vector sources (fractional solids, shape fill and
// stroke, and shaped text). It keys ONLY on the raster geometry (the exact chain matrix, proxy
// scales, rectangle dimensions, output window and pixel aspect), so a changed colour or opacity
// does not rebuild geometry, and a changed transform does. Budget is charged by the actual geometry
// byte size (the row ranges plus the concatenated spans); an entry over the whole budget is
// refused. This is a CPU-side cache of the O(edges x rows) geometry; it owns no GPU resource, no
// per-pixel mask and no full RGBA image. The colour/opacity identity lives in the prepared
// command's pixel key, never here.

#include <bloom/render/path_raster.hpp>

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

using GpuSceneCoverageGeometry = render::PathRasterCoverageGeometry;

// The actual immutable byte size of one coverage geometry value.
[[nodiscard]] inline std::uint64_t
gpuSceneCoverageGeometryBytes(const GpuSceneCoverageGeometry& geometry) noexcept {
    return static_cast<std::uint64_t>(geometry.rows.size()) *
               sizeof(render::PathRasterCoverageRange) +
           static_cast<std::uint64_t>(geometry.spans.size()) *
               sizeof(render::PathRasterCoverageSpan);
}

class GpuSceneCoverageCache final {
  public:
    explicit GpuSceneCoverageCache(std::uint64_t maxBytes = 32ULL * 1024ULL * 1024ULL);

    [[nodiscard]] std::shared_ptr<const GpuSceneCoverageGeometry>
    find(const std::string& geometryKey) noexcept;
    void store(std::string geometryKey, std::shared_ptr<const GpuSceneCoverageGeometry> geometry);
    // Drops every retained coverage geometry. A caller already holding one keeps it valid; only
    // re-derivation is forced. Lifetime hit/miss counters are left intact. Thread-safe.
    void clear();

    [[nodiscard]] std::uint64_t retainedBytes() const;
    [[nodiscard]] std::size_t entryCount() const;
    [[nodiscard]] std::uint64_t hits() const;
    [[nodiscard]] std::uint64_t misses() const;

  private:
    struct Entry final {
        std::shared_ptr<const GpuSceneCoverageGeometry> geometry;
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
