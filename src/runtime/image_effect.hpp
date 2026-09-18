#pragma once

#include <bloom/color/ocio_cpu_color_space_processor.hpp>
#include <bloom/color/ocio_cpu_file_transform_processor.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <mutex>
#include <unordered_map>

namespace bloom::runtime::detail {

struct PreparedImageEffect final {
    std::shared_ptr<const color::CpuColorSpaceProcessor> processor;
    std::shared_ptr<const color::CpuColorSpaceProcessor> afterProcessor;
    std::shared_ptr<const color::CpuFileTransformProcessor> fileProcessor;
    std::optional<EvaluationDiagnostic> diagnostic;
    std::string cacheIdentity;
    bool cancelled = false;
    [[nodiscard]] bool identity() const noexcept {
        return !diagnostic && !cancelled ? (!processor || processor->isIdentity()) &&
                                               (!afterProcessor || afterProcessor->isIdentity()) &&
                                               (!fileProcessor || fileProcessor->isIdentity())
                                         : true;
    }
};

// Prepared built-in transforms are shared across nodes and frames by exact config/from/to.
// No OCIO work runs on the UI thread. The bounded cache belongs to the evaluator session.
class ImageEffectContext final {
  public:
    [[nodiscard]] PreparedImageEffect prepare(const CompiledImageEffect& effect,
                                              const EvaluationColorIntent& intent,
                                              const std::filesystem::path& base = {},
                                              const CancellationToken& cancellation = {});

  private:
    std::mutex mutex_;
    std::unordered_map<std::string, PreparedImageEffect> processors_;
};

struct ImageEffectResult final {
    std::shared_ptr<const render::Rgba32fImage> image;
    std::optional<EvaluationDiagnostic> diagnostic;
    bool cancelled = false;
};

[[nodiscard]] ImageEffectResult
applyImageEffect(const PreparedImageEffect& effect, const render::Rgba32fImage& input,
                 std::size_t pixelBudget, const CancellationToken& cancellation,
                 OperationIndex operation, const EvaluationProgressCallback& progress);
} // namespace bloom::runtime::detail
