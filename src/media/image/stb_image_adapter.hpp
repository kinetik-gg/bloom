#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace bloom::media::detail {
struct ImageInfo {
    int width = 0;
    int height = 0;
    bool sixteenBit = false;
};

// Request-scoped live parser-allocation ceiling for the calling thread. The stb callbacks enforce
// it while this scope is alive and restore the previous ceiling on destruction, so two concurrent
// decodes with different budgets on different threads cannot interfere.
class ImageAllocationBudgetScope final {
  public:
    explicit ImageAllocationBudgetScope(std::size_t limitBytes) noexcept;
    ~ImageAllocationBudgetScope() noexcept;
    ImageAllocationBudgetScope(const ImageAllocationBudgetScope&) = delete;
    ImageAllocationBudgetScope& operator=(const ImageAllocationBudgetScope&) = delete;
    ImageAllocationBudgetScope(ImageAllocationBudgetScope&&) = delete;
    ImageAllocationBudgetScope& operator=(ImageAllocationBudgetScope&&) = delete;

  private:
    std::size_t previous_;
};

// Header-only PNG/JPEG inspection. Reads through a bounded callback stream and never buffers the
// whole encoded file. Returns false for a non-image, an unreadable file, or a codec-rejected
// header.
[[nodiscard]] bool imageInfo(const std::filesystem::path& path, ImageInfo& info);

// Streams the PNG/JPEG decode into a caller-bounded parser temporary (the thread-local allocation
// scope) staging the RGBA16 samples. `pixelBudget` is the explicit peak-admission budget: the
// adapter refuses before entering the parser when final RGBA32F plus the RGBA16 staging buffer
// would exceed it, so the parser never makes an unbounded allocation.
[[nodiscard]] std::vector<std::uint16_t>
imageSamples(const std::filesystem::path& path, const ImageInfo& info, std::size_t pixelBudget);
} // namespace bloom::media::detail
