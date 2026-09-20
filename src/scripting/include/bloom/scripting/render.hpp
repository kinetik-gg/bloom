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
    std::uint64_t gpuReadbacks = 0;
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
