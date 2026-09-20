#pragma once

#include <bloom/host/frame_range_runner.hpp>
#include <bloom/output/output_analysis.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/scripting/session.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace bloom::host {
class GpuExportProvider;
} // namespace bloom::host

namespace bloom::scripting {

struct RenderRequest final {
    document::CompositionId composition;
    std::optional<std::uint64_t> frame;
    std::optional<std::pair<std::uint64_t, std::uint64_t>> range;
    output::OutputPresetV1 preset = output::OutputPresetV1::PngRgba8SrgbV1;
    std::filesystem::path destination;
    std::function<bool()> cancelled = {};
};

struct RenderResult final {
    bool succeeded = false;
    std::uint64_t publishedFrames = 0;
    std::string preservationReport;
    std::string diagnostic;
    // Aggregate native GPU provenance over the frames this render evaluated. Diagnostics only:
    // it never enters preservation identity or the approval digest. All zero when no provider was
    // injected/available or every frame took the CPU reference path.
    std::uint64_t gpuEvaluatedFrames = 0;
    std::uint64_t gpuNativeDispatches = 0;
    // The real final device-to-host readback submission count. `gpuReadbacks` is the pre-existing
    // compatibility name for this same genuine counter (one final readback per evaluated frame);
    // `gpuReadbackSubmissions` is the explicit combined-readback name.
    std::uint64_t gpuReadbacks = 0;
    std::uint64_t gpuReadbackSubmissions = 0;
    // Distinct payloads those submissions carried (process-analysis + encoded output): one for the
    // identity/EXR arm, two for the display/PNG arm. Never derived from the submission count.
    std::uint64_t gpuTransferredPayloads = 0;
    // Exact process-analysis and encoded-output payload byte totals of the combined readback.
    std::uint64_t gpuProcessPayloadBytes = 0;
    std::uint64_t gpuEncodedPayloadBytes = 0;
    std::uint64_t gpuDeviceOwnershipEpoch = 0;
};

class Render final {
  public:
    [[nodiscard]] static RenderResult
    run(Session& session, runtime::TaskScheduler& scheduler,
        const runtime::SnapshotCompiler& compiler, RenderRequest request,
        std::filesystem::path scratchDirectory = {},
        std::shared_ptr<host::GpuExportProvider> gpuProvider = {});
};

} // namespace bloom::scripting
