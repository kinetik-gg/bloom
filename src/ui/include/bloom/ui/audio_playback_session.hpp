#pragma once

#include <bloom/document/document.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/ui/composition_plan_cache.hpp>

#include <QObject>

#include <optional>

namespace bloom::ui {

class CompositionSession;

// UI-side projection of the live composition's audio mix. The mix is derived from the LIVE
// document revision through the shared compiled-plan cache -- never from whatever frame the
// preview happens to be showing, because a preview frame can legitimately lag the document (a
// layout-only edit re-uses its pixels; an unsupported edit keeps the previous frame on screen) and
// audio must not fall silent for either. It never owns decoded samples or a device.
class AudioPlaybackSession final : public QObject {
    Q_OBJECT

  public:
    AudioPlaybackSession(CompositionSession& session, const runtime::SnapshotCompiler& compiler,
                         CompiledPlanCacheHandle planCache,
                         const runtime::CpuCompositionEvaluator& evaluator,
                         QObject* parent = nullptr);

    [[nodiscard]] document::Revision revision() const noexcept { return revision_; }
    [[nodiscard]] const std::optional<runtime::AudioMixDescription>& mix() const noexcept {
        return mix_;
    }

    // Recomputes the mix for the live snapshot at the session's current time. Returns true when
    // a mix is published; a composition that does not compile publishes no mix. mixChanged() is
    // emitted only when the published description actually differs, so a playback tick that moves
    // the time without changing any clip costs nothing downstream.
    bool refresh();

  signals:
    void mixChanged();

  private:
    void handleSnapshotChanged();
    void setMix(std::optional<runtime::AudioMixDescription> mix);

    CompositionSession& session_;
    const runtime::SnapshotCompiler& compiler_;
    CompiledPlanCacheHandle planCache_;
    const runtime::CpuCompositionEvaluator& evaluator_;
    document::Revision revision_;
    std::optional<runtime::AudioMixDescription> mix_;
};

} // namespace bloom::ui
