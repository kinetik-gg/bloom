#pragma once

#include <bloom/output/output_analysis_analyzer.hpp>

#include <cstdint>

namespace bloom::output::detail {

enum class OutputAnalysisAnalyzerFaultV1 : std::uint8_t {
    None,
    AllocationFailure,
    DescriptorTooLong,
    DescriptorStorageTooLarge,
    GeneratedReportInvariantViolation,
};

[[nodiscard]] OutputAnalysisAnalyzerResultV1
analyzePngRgba8SrgbV1WithFaultForTest(PngRgba8SrgbAnalysisInputV1 input,
                                      OutputAnalysisAnalyzerFaultV1 fault) noexcept;

[[nodiscard]] OutputAnalysisAnalyzerResultV1 analyzeFlatExrRgba32fLinRec709SceneV1WithFaultForTest(
    FlatExrRgba32fLinRec709SceneAnalysisInputV1 input,
    OutputAnalysisAnalyzerFaultV1 fault) noexcept;

} // namespace bloom::output::detail
