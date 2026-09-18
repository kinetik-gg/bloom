#pragma once
#include <bloom/color/ocio_cpu_display_processor.hpp>
#include <bloom/runtime/evaluation.hpp>

namespace bloom::output {
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

  private:
    std::shared_ptr<const color::PreparedCpuDisplayProcessorHandle> processor_;
    std::string description_;
    core::Sha256Digest digest_;
};
[[nodiscard]] std::uint64_t
outputLookEffectCountV1(const runtime::CompiledCompositionPlan&) noexcept;
[[nodiscard]] std::string outputLookDescriptionV1(const runtime::ProcessFrameIdentity&);
} // namespace bloom::output
