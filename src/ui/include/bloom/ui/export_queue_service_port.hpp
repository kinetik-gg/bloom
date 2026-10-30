#pragma once

// The desktop ExportQueuePort: a thin forwarder over host::ExportQueueService for the reads and the
// session commands, and over ExportQueueEdits (undoable commands on the live document) for the
// project-truth edits. It owns nothing. The service is asked for on every call, because
// ProjectHost replaces it with the session (File > New); with none the port reads as an empty queue
// that refuses every command.

#include <bloom/ui/export_queue_adapter.hpp>

#include <functional>

namespace bloom::ui {

class ExportQueueEdits;

class ServiceExportQueuePort final : public ExportQueuePort {
  public:
    using ServiceSource = std::function<host::ExportQueueService*()>;
    ServiceExportQueuePort(ServiceSource source, ExportQueueEdits& edits);

    std::shared_ptr<const host::ExportQueueSnapshot> snapshot() const override;
    std::shared_ptr<const host::ExportPresetCatalogueSnapshot> catalogue() const override;
    host::ExportQueueEventBatch events(std::uint64_t afterSequence) const override;
    host::ExportLogBatch log(document::ExportJobId job, std::uint64_t afterSequence) const override;
    std::optional<document::ExportJobId> jobForTaskOwner(runtime::TaskOwnerId owner) const override;
    std::string jobScript(const document::ExportJob& job,
                          host::ExportScriptFormat format) const override;
    void pump() override;

    host::ExportCommandResult startAll() override;
    host::ExportCommandResult start(std::span<const document::ExportJobId> jobs) override;
    host::ExportCommandResult pause(document::ExportJobId job) override;
    host::ExportCommandResult resume(document::ExportJobId job) override;
    host::ExportCommandResult stop(document::ExportJobId job) override;
    host::ExportCommandResult stopAll() override;
    host::ExportCommandResult retry(document::ExportJobId job) override;
    host::ExportCommandResult confirmReplace(document::ExportJobId job,
                                             document::ExportOutputId output) override;
    void refreshPreflight() override;

    ExportEditResult addJobs(const std::vector<document::CompositionId>& compositions,
                             std::optional<std::size_t> index) override;
    ExportEditResult duplicateJob(document::ExportJobId job) override;
    ExportEditResult removeJob(document::ExportJobId job) override;
    ExportEditResult removeFinished() override;
    ExportEditResult moveJob(document::ExportJobId job, std::size_t index) override;
    ExportEditResult editJob(document::ExportJobId job, const ExportJobEdit& edit) override;
    ExportEditResult addOutput(document::ExportJobId job, const std::string& presetId) override;
    ExportEditResult applyPreset(document::ExportJobId job, document::ExportOutputId output,
                                 const std::string& presetId) override;
    ExportEditResult editOutput(document::ExportJobId job, document::ExportOutputId output,
                                const ExportOutputEdit& edit) override;
    ExportEditResult removeOutput(document::ExportJobId job,
                                  document::ExportOutputId output) override;

  private:
    ExportEditResult published(ExportEditResult result);
    [[nodiscard]] host::ExportQueueService* service() const { return source_(); }
    ServiceSource source_;
    std::shared_ptr<const host::ExportQueueSnapshot> none_;
    ExportQueueEdits& edits_;
};

} // namespace bloom::ui
