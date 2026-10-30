#pragma once

#include <bloom/host/export_queue.hpp>
#include <bloom/scripting/value.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

// ADR 0033 section 13: the export session calls shared by bloom-cli scripts, Python and MCP.
// They act on runtime state only (not transactional, never undoable, never saved):
// export.start, export.start-all, export.pause, export.resume, export.stop, export.stop-all,
// export.retry, export.confirm-replace, export.status (the snapshot as JSON, optionally
// `sinceRevision`), export.events (`afterSequence`), export.log (`job`, `afterSequence`),
// export.wait (a job or the drain, with a timeout; headless only), export.presets and
// export.script (exportJobScript).
namespace bloom::scripting {

struct ExportCallResult final {
    bool ok = false;
    // The call's answer as compact JSON.
    std::string json;
    // On refusal: a stable code (the availability code name or bloom.scripting.*) and a reason.
    std::string code;
    std::string message;
};

[[nodiscard]] bool isExportCall(std::string_view name) noexcept;

// Bound to one service on its owner thread (the headless client's thread, or in the desktop
// application the UI thread through the in-app server).
class ExportCalls final {
  public:
    explicit ExportCalls(host::ExportQueueService& service) : service_(service) {}
    // export.start without a `job` starts the job the last bloom.export.add-job added.
    void noteAddedJob(document::ExportJobId job) { lastAdded_ = job; }
    // `headless` enables export.wait (it pumps and sleeps on the calling thread).
    [[nodiscard]] ExportCallResult call(std::string_view name, const Arguments& arguments,
                                        bool headless);

  private:
    host::ExportQueueService& service_;
    std::optional<document::ExportJobId> lastAdded_;
};

[[nodiscard]] std::string exportSnapshotJson(const host::ExportQueueSnapshot& snapshot);
[[nodiscard]] std::string exportEventsJson(const host::ExportQueueEventBatch& batch);
[[nodiscard]] std::string exportVerificationJson(const host::ExportOutputVerification& output);
[[nodiscard]] std::string exportImportJson(const host::ExportAssetImportResult& result);

} // namespace bloom::scripting
