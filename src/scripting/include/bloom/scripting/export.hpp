#pragma once

#include <bloom/host/export_queue.hpp>
#include <bloom/scripting/value.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
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
namespace bloom::runtime {
class GpuOcioContextResolver;
}

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

class Session;

// The export queue of a headless session (bloom-cli, bloom-mcp). Construction probes the encoders
// (waiting up to 30 s on the calling thread) and must happen on a client thread, never a UI
// thread. Destruction stops every job and retires the GPU provider and encoder workers.
// `ocioResolver` is the composition root's packaged GPU shader-tool resolver
// (host::makePackagedGpuOcioResolver in the executable that stages the tools). It must come from
// the executable: only that target carries the packaging definitions, and without it the queue's
// output colour (display/view for ProRes, DNxHR, H.264, PNG) has no GPU preparation.
// `vulkanLoader` overrides the platform Vulkan loader (tests name the qualified prefix's loader);
// `bootstrapGate` is the provider's shared native-bootstrap gate (GpuExportProvider::create).
class HeadlessExportQueue final {
  public:
    HeadlessExportQueue(Session& session,
                        std::shared_ptr<runtime::GpuOcioContextResolver> ocioResolver,
                        std::filesystem::path vulkanLoader = {},
                        std::shared_ptr<std::timed_mutex> bootstrapGate = {});
    ~HeadlessExportQueue();
    HeadlessExportQueue(const HeadlessExportQueue&) = delete;
    HeadlessExportQueue& operator=(const HeadlessExportQueue&) = delete;
    [[nodiscard]] host::ExportQueueService& service();
    [[nodiscard]] ExportCalls& calls();

  private:
    struct State;
    std::unique_ptr<State> state_;
};

[[nodiscard]] std::string exportSnapshotJson(const host::ExportQueueSnapshot& snapshot);
[[nodiscard]] std::string exportEventsJson(const host::ExportQueueEventBatch& batch);
[[nodiscard]] std::string exportVerificationJson(const host::ExportOutputVerification& output);
[[nodiscard]] std::string exportImportJson(const host::ExportAssetImportResult& result);

} // namespace bloom::scripting
