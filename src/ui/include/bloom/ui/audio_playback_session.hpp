#pragma once

#include <bloom/document/document.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/composition_plan_cache.hpp>

#include <QObject>

#include <optional>

namespace bloom::ui {

class CompositionSession;
class TaskUiBridge;

// UI-side projection of the live composition's audio mix. The mix is derived from the LIVE
// document revision through the shared compiled-plan cache -- never from whatever frame the
// preview happens to be showing, because a preview frame can legitimately lag the document (a
// layout-only edit re-uses its pixels; an unsupported edit keeps the previous frame on screen) and
// audio must not fall silent for either. It never owns decoded samples or a device.
class AudioPlaybackSession final : public QObject {
    Q_OBJECT

  public:
    AudioPlaybackSession(CompositionSession& session, runtime::TaskScheduler& scheduler,
                         TaskUiBridge& bridge, const runtime::SnapshotCompiler& compiler,
                         CompiledPlanCacheHandle planCache,
                         const runtime::CpuCompositionEvaluator& evaluator,
                         QObject* parent = nullptr);
    ~AudioPlaybackSession() override;

    [[nodiscard]] document::Revision revision() const noexcept { return revision_; }
    [[nodiscard]] const std::optional<runtime::AudioMixDescription>& mix() const noexcept {
        return mix_;
    }

    // Requests a worker evaluation for the live snapshot/time. At most one task runs and one
    // newest request waits. A static direct mix skips subsequent time ticks at the same revision.
    bool refresh();
    void beginShutdown();

  signals:
    void mixChanged();

  private:
    void handleSnapshotChanged();
    void consumeReady();
    void submitPending();
    void setMix(std::optional<runtime::AudioMixDescription> mix);

    struct Preparation;
    using ResultHandle = std::shared_ptr<const Preparation>;
    struct Request final {
        document::Snapshot snapshot;
        document::CompositionId compositionId;
        core::RationalTime time;
        std::uint64_t generation = 0;
    };
    struct Active final {
        Request request;
        runtime::TaskHandle<ResultHandle> handle;
    };

    CompositionSession& session_;
    runtime::TaskScheduler& scheduler_;
    TaskUiBridge& bridge_;
    const runtime::SnapshotCompiler& compiler_;
    CompiledPlanCacheHandle planCache_;
    const runtime::CpuCompositionEvaluator& evaluator_;
    document::Revision revision_;
    std::optional<runtime::AudioMixDescription> mix_;
    std::optional<Active> active_;
    std::optional<Request> pending_;
    std::uint64_t generation_ = 0;
    bool staticForCurrent_ = false;
    bool shuttingDown_ = false;
};

} // namespace bloom::ui
