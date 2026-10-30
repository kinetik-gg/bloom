#pragma once

#include <bloom/host/export_job_runner.hpp>

#include <memory>
#include <optional>
#include <vector>

// Private to src/host: the per-output passes of ExportJobRunner.
namespace bloom::host::detail {

struct ExportDriverInput final {
    document::Snapshot snapshot;
    document::CompositionId composition;
    std::string compositionName;
    runtime::EvaluationColorIntent colorIntent;
    std::filesystem::path assetBaseDirectory;
    document::ExportJob job;
    ExportRunOutput output;
    ExportResolvedRange range;
    ExportEncoderStatus encoders;
    runtime::TaskOwner owner;
};

struct ExportDriverReport final {
    ExportOutputState state = ExportOutputState::Pending;
    std::optional<ExportFailure> failure;
    ExportOutputVerification verification;
    std::vector<std::filesystem::path> published;
    std::vector<ExportLogEntry> log;
};

class ExportOutputDriver {
  public:
    ExportOutputDriver() = default;
    ExportOutputDriver(const ExportOutputDriver&) = delete;
    ExportOutputDriver& operator=(const ExportOutputDriver&) = delete;
    virtual ~ExportOutputDriver() = default;
    virtual void poll() = 0;
    virtual void stop() = 0;
    virtual void setPaused(bool paused) = 0;
    [[nodiscard]] virtual bool pausedAtBoundary() const = 0;
    [[nodiscard]] virtual bool done() const = 0;
    [[nodiscard]] virtual ExportOutputState state() const = 0;
    [[nodiscard]] virtual std::uint64_t framesCompleted() const = 0;
    [[nodiscard]] virtual std::optional<std::uint64_t> currentFrame() const = 0;
    [[nodiscard]] virtual ExportDeviceRoute route() const = 0;
    [[nodiscard]] virtual std::optional<std::uint64_t> memoryNeededBytes() const {
        return std::nullopt;
    }
    [[nodiscard]] virtual std::vector<ExportLogEntry> takeLog() = 0;
    [[nodiscard]] virtual ExportDriverReport takeReport() = 0;
};

[[nodiscard]] std::unique_ptr<ExportOutputDriver>
makeExportOutputDriver(const ExportJobRunnerDependencies& dependencies, ExportDriverInput input);

// The file `frame` of an image output writes (the destination itself for a still).
[[nodiscard]] std::optional<std::filesystem::path>
exportFramePath(const document::ExportJob& job, const ExportRunOutput& output, std::uint64_t frame);

} // namespace bloom::host::detail
