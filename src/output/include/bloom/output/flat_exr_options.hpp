#pragma once

#include <bloom/color/ocio_cpu_color_space_processor.hpp>
#include <bloom/runtime/evaluation.hpp>

namespace bloom::output {
// Portable serializer values, independent of OpenEXR's enum.
enum class FlatExrCompressionV1 : std::uint8_t { Zip = 1, Piz = 2, Zips = 3, None = 4 };
struct FlatExrRgba32fOptionsV1 final {
    // Empty means the frame's effective working space, preserving the legacy bytes.
    std::string outputColorSpaceId;
    FlatExrCompressionV1 compression = FlatExrCompressionV1::Zip;
};
class PreparedFlatExrOutputV1 final {
  public:
    [[nodiscard]] static std::shared_ptr<const PreparedFlatExrOutputV1>
    prepare(const runtime::EvaluationColorIntent&, FlatExrRgba32fOptionsV1);
    [[nodiscard]] const FlatExrRgba32fOptionsV1& options() const noexcept { return options_; }
    [[nodiscard]] const std::shared_ptr<const color::CpuColorSpaceProcessor>&
    processor() const noexcept {
        return processor_;
    }
    [[nodiscard]] bool matches(const runtime::EvaluationColorIntent&) const noexcept;
    [[nodiscard]] bool apply(std::span<const render::Rgba32f>,
                             std::span<std::array<float, 4>>) const noexcept;

  private:
    PreparedFlatExrOutputV1() = default;
    FlatExrRgba32fOptionsV1 options_;
    std::string sourceColorSpaceId_;
    core::Sha256Digest sourceRevision_;
    std::shared_ptr<const color::CpuColorSpaceProcessor> processor_;
};
[[nodiscard]] std::string_view flatExrCompressionNameV1(FlatExrCompressionV1) noexcept;
} // namespace bloom::output
