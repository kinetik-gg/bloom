#pragma once
#include <bloom/color/ocio_cpu_display_processor.hpp>
#include <bloom/runtime/evaluation.hpp>

namespace bloom::output {
// The built-in OCIO config an output colour intent names, resolved exactly as
// PreparedOutputDisplayV1::prepare resolves it (an empty revision selects Bloom Neutral).
[[nodiscard]] std::optional<color::ResolvedBloomNeutralConfig>
resolveOutputDisplayConfigV1(const runtime::EvaluationColorIntent&);
class PreparedOutputDisplayV1 final {
  public:
    [[nodiscard]] static std::shared_ptr<const PreparedOutputDisplayV1>
    prepare(const runtime::EvaluationColorIntent&, std::string_view display = {},
            std::string_view view = {});
    [[nodiscard]] const color::PreparedCpuDisplayProcessorHandle& processor() const noexcept {
        return *processor_;
    }
    [[nodiscard]] const std::string& description() const noexcept { return description_; }
    [[nodiscard]] const core::Sha256Digest& digest() const noexcept { return digest_; }
    // The resolved display and view (the config's defaults when the request named none).
    [[nodiscard]] const std::string& displayName() const noexcept { return display_; }
    [[nodiscard]] const std::string& viewName() const noexcept { return view_; }

  private:
    PreparedOutputDisplayV1() = default;
    std::string display_;
    std::string view_;
    std::shared_ptr<const color::PreparedCpuDisplayProcessorHandle> processor_;
    std::string description_;
    core::Sha256Digest digest_;
};
// Issue #369: the container colour tags (H.273 primaries, transfer and matrix; FFmpeg range
// convention) that describe pixels encoded through `display`. Known display families map to
// their standard code points; an unknown display is tagged unspecified rather than guessed.
struct OutputColourTagsV1 final {
    std::int32_t primaries = 2, transfer = 2, matrix = 1, range = 1;
    friend bool operator==(const OutputColourTagsV1&, const OutputColourTagsV1&) = default;
};
[[nodiscard]] OutputColourTagsV1 outputColourTagsV1(std::string_view display) noexcept;
[[nodiscard]] std::uint64_t
outputLookEffectCountV1(const runtime::CompiledCompositionPlan&) noexcept;
[[nodiscard]] std::string outputLookDescriptionV1(const runtime::ProcessFrameIdentity&);
} // namespace bloom::output
