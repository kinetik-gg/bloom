#pragma once

#include <bloom/document/document.hpp>
#include <bloom/media/audio/audio.hpp>
#include <bloom/runtime/audio_stream_key.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/composition_plan_cache.hpp>

#include <QObject>

#include <map>
#include <memory>
#include <optional>

namespace bloom::ui {

class CompositionSession;
class TaskUiBridge;
class AssetController;

// UI-side projection of the live composition's audio mix. The mix is derived from the LIVE
// document revision through the shared compiled-plan cache -- never from whatever frame the
// preview happens to be showing, because a preview frame can legitimately lag the document (a
// layout-only edit re-uses its pixels; an unsupported edit keeps the previous frame on screen) and
// audio must not fall silent for either. It pins buffers used by the current mix after the asset
// worker's admission, so cache retention cannot discard them before playback adoption. It owns
// no device.
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
    // #358 work counters: a render-neutral edit requests no remix, and a remix never silences the
    // previous mix while the new one is prepared.
    struct Work final {
        std::uint64_t mixRequests = 0;
        std::uint64_t neutralEdits = 0;
        std::uint64_t silences = 0;
    };
    [[nodiscard]] Work work() const noexcept { return work_; }
    [[nodiscard]] const std::optional<runtime::AudioMixDescription>& mix() const noexcept {
        return mix_;
    }
    void setAudioBufferSource(AssetController& source);
    [[nodiscard]] std::shared_ptr<const media::audio::AudioBuffer>
    audioBuffer(document::AssetId id, std::uint32_t stream = 0) const;

    // Requests a worker evaluation for the live snapshot/time. At most one task runs and one
    // newest request waits. A static direct mix skips subsequent time ticks at the same revision.
    bool refresh();
    void beginShutdown();

  signals:
    void mixChanged();

  private:
    void handleSnapshotChanged();
    void handleCompositionChanged();
    void silence();
    Work work_;
    void consumeReady();
    void submitPending();
    void setMix(std::optional<runtime::AudioMixDescription> mix);
    void refreshPins(bool notify);

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
    AssetController* audioSource_ = nullptr;
    std::map<runtime::AudioStreamKey, std::shared_ptr<const media::audio::AudioBuffer>>
        pinnedAudio_;
    std::optional<Active> active_;
    std::optional<Request> pending_;
    std::uint64_t generation_ = 0;
    bool staticForCurrent_ = false;
    bool shuttingDown_ = false;
};

} // namespace bloom::ui
