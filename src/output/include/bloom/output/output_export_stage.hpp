#pragma once

#include <chrono>
#include <cstdint>
#include <functional>

// docs/architecture/frame-output.md "Non-Blocking Execution": "Attempt and export progress are
// monotonic within each stage and use the ordered stage vocabulary Resolving, Evaluating,
// Identifying, ColorPreparing, Analyzing, PreparingOutput, Writing, Verifying, Publishing. EXR
// skips ColorPreparing". This header is the one closed enum shared by both the pre-approval
// OutputAnalysisAttempt graph (bloom::output::OutputAnalysisAttemptV1,
// bloom::host::beginOutputAnalysisAttemptV1) and the approved export job graph
// (bloom::host::executeExportPublication) -- design decision 1's "stage-progress vocabulary as a
// closed enum" living in src/output alongside the other Qt-free orchestration types.
namespace bloom::output {

// EXR never emits ColorPreparing or PreparingOutput (design decision 1/4: EXR has no display
// processor and exposes retained rows directly), but both enumerators stay in the closed
// vocabulary so a caller can format/compare stages from either preset without a preset-specific
// enum, exactly as the doc names all nine stages as one ordered list.
enum class OutputExportStageV1 : std::uint8_t {
    Resolving,
    Evaluating,
    Identifying,
    ColorPreparing,
    Analyzing,
    PreparingOutput,
    Writing,
    Verifying,
    Publishing,
};

struct OutputExportProgressV1 final {
    OutputExportStageV1 stage = OutputExportStageV1::Resolving;
    std::uint64_t completed = 0;
    std::uint64_t total = 0;

    friend bool operator==(const OutputExportProgressV1&,
                           const OutputExportProgressV1&) noexcept = default;
};

using OutputExportProgressCallbackV1 = std::function<void(const OutputExportProgressV1&)>;

// Design decision 5's injectable monotonic clock: "the 24-hour total deadline and 120-second
// no-progress interval enforced against an INJECTABLE monotonic clock (tests drive it; production
// uses the task system's existing clock source)". bloom::runtime::TaskSnapshot already timestamps
// queuedAt/startedAt/finishedAt with std::chrono::steady_clock::time_point (task_types.hpp) -- that
// IS the task system's existing clock source, so the production default below reuses it directly.
// A test supplies a deterministic substitute instead of sleeping in real time.
using OutputExportClockV1 = std::function<std::chrono::steady_clock::time_point()>;

[[nodiscard]] inline OutputExportClockV1 systemOutputExportClockV1() noexcept {
    return []() noexcept { return std::chrono::steady_clock::now(); };
}

} // namespace bloom::output
