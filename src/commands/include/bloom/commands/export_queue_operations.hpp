#pragma once

#include <bloom/commands/operation.hpp>
#include <bloom/document/export_queue.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Undoable export queue edits (ADR 0033, section 12). Every operation edits project truth only
// (document::Project::exportJobs()) and none can change a rendered pixel, so each answers
// renderAffecting() == false: a queue edit dirties and saves the project and enters undo history,
// but never invalidates a preview cache.
//
// Operations do not know about runs. "Locked while running" is the export queue service's
// availability rule; a script may still edit a running job's record, and the run keeps the
// settings it captured when it started.
namespace bloom::commands {

inline constexpr std::string_view kExportJobOutput = "exportJob";
inline constexpr std::string_view kExportOutputOutput = "exportOutput";

// The ids an export queue edit created, read from its CommandResult so a caller never diffs the
// queue: add-job and duplicate-job report their job and every output in order, add-output its
// output. `operationIndex` selects the operation within the transaction.
struct AddedExportIds final {
    std::optional<document::ExportJobId> job;
    std::vector<document::ExportOutputId> outputs;
};
[[nodiscard]] AddedExportIds addedExportIds(const CommandResult& result,
                                            std::size_t operationIndex = 0);

class ExportQueueOperation : public Operation {
  public:
    [[nodiscard]] bool renderAffecting() const noexcept final { return false; }
};

// Adds `job` at `index` (the end when absent). Invalid ids in the job or its outputs are
// allocated; valid ids are kept when they are new (redo and paste). Outputs: one "exportJob", then
// one "exportOutput" per output in order.
class AddExportJob final : public ExportQueueOperation {
  public:
    explicit AddExportJob(document::ExportJob job, std::optional<std::size_t> index = std::nullopt)
        : job_(std::move(job)), index_(index) {}
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.export.add-job";
    }
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::ExportJob job_;
    std::optional<std::size_t> index_;
};

class RemoveExportJobs final : public ExportQueueOperation {
  public:
    // `typeId` distinguishes "remove-jobs" from "remove-finished-jobs" in history and scripts;
    // the caller resolves which jobs are finished from the runtime snapshot.
    explicit RemoveExportJobs(std::vector<document::ExportJobId> jobs, bool finishedOnly = false)
        : jobs_(std::move(jobs)), finishedOnly_(finishedOnly) {}
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return finishedOnly_ ? "bloom.export.remove-finished-jobs" : "bloom.export.remove-jobs";
    }
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    std::vector<document::ExportJobId> jobs_;
    bool finishedOnly_ = false;
};

// Copies the job with new job and output ids, directly after the original.
class DuplicateExportJob final : public ExportQueueOperation {
  public:
    explicit DuplicateExportJob(document::ExportJobId job) : job_(job) {}
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.export.duplicate-job";
    }
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::ExportJobId job_;
};

// Moves the jobs, in their current relative order, so the first lands at `index` of the queue
// with the moved jobs removed (clamped to the end).
class MoveExportJobs final : public ExportQueueOperation {
  public:
    MoveExportJobs(std::vector<document::ExportJobId> jobs, std::size_t index)
        : jobs_(std::move(jobs)), index_(index) {}
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.export.move-jobs";
    }
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    std::vector<document::ExportJobId> jobs_;
    std::size_t index_ = 0;
};

class SetExportJobName final : public ExportQueueOperation {
  public:
    SetExportJobName(document::ExportJobId job, std::string name)
        : job_(job), name_(std::move(name)) {}
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.export.set-job-name";
    }
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::ExportJobId job_;
    std::string name_;
};

class SetExportJobRange final : public ExportQueueOperation {
  public:
    SetExportJobRange(document::ExportJobId job, document::ExportRange range)
        : job_(job), range_(range) {}
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.export.set-job-range";
    }
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::ExportJobId job_;
    document::ExportRange range_;
};

class SetExportJobEnabled final : public ExportQueueOperation {
  public:
    SetExportJobEnabled(document::ExportJobId job, bool enabled) : job_(job), enabled_(enabled) {}
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.export.set-job-enabled";
    }
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::ExportJobId job_;
    bool enabled_ = true;
};

class SetExportJobLabelColor final : public ExportQueueOperation {
  public:
    SetExportJobLabelColor(document::ExportJobId job,
                           std::optional<std::array<std::uint8_t, 3>> color)
        : job_(job), color_(color) {}
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.export.set-job-label-color";
    }
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::ExportJobId job_;
    std::optional<std::array<std::uint8_t, 3>> color_;
};

// Appends `output` (its id is allocated when invalid). Output: "exportOutput".
class AddExportOutput final : public ExportQueueOperation {
  public:
    AddExportOutput(document::ExportJobId job, document::ExportOutput output)
        : job_(job), output_(std::move(output)) {}
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.export.add-output";
    }
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::ExportJobId job_;
    document::ExportOutput output_;
};

class RemoveExportOutput final : public ExportQueueOperation {
  public:
    explicit RemoveExportOutput(document::ExportOutputId output) : output_(output) {}
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.export.remove-output";
    }
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::ExportOutputId output_;
};

// Replaces the output's settings. The preset id is kept (the "modified" marker compares them).
class SetExportOutputSettings final : public ExportQueueOperation {
  public:
    SetExportOutputSettings(document::ExportOutputId output,
                            document::ExportOutputSettings settings)
        : output_(output), settings_(std::move(settings)) {}
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.export.set-output-settings";
    }
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::ExportOutputId output_;
    document::ExportOutputSettings settings_;
};

// Copies a catalogue preset's settings into the output and records its id. The caller resolves
// the preset (bloom::host::ExportPresetCatalogue); the record never refers back to it.
class ApplyExportPreset final : public ExportQueueOperation {
  public:
    ApplyExportPreset(document::ExportOutputId output, std::string presetId,
                      document::ExportOutputSettings settings)
        : output_(output), presetId_(std::move(presetId)), settings_(std::move(settings)) {}
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.export.apply-preset";
    }
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::ExportOutputId output_;
    std::string presetId_;
    document::ExportOutputSettings settings_;
};

class SetExportOutputDestination final : public ExportQueueOperation {
  public:
    SetExportOutputDestination(document::ExportOutputId output,
                               document::ExportDestination destination)
        : output_(output), destination_(std::move(destination)) {}
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.export.set-output-destination";
    }
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::ExportOutputId output_;
    document::ExportDestination destination_;
};

class SetExportOutputEnabled final : public ExportQueueOperation {
  public:
    SetExportOutputEnabled(document::ExportOutputId output, bool enabled)
        : output_(output), enabled_(enabled) {}
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.export.set-output-enabled";
    }
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::ExportOutputId output_;
    bool enabled_ = true;
};

class SetExportOutputImport final : public ExportQueueOperation {
  public:
    SetExportOutputImport(document::ExportOutputId output, bool importIntoAssets)
        : output_(output), import_(importIntoAssets) {}
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.export.set-output-import";
    }
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::ExportOutputId output_;
    bool import_ = false;
};

} // namespace bloom::commands
