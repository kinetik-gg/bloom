#pragma once

#include <bloom/document/document.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>

#include <QObject>

#include <memory>
#include <optional>

namespace bloom::ui {

class CompositionSession;

// UI-side revision boundary for the device-free audio projection. Compilation/evaluation is done
// by a worker-owned pipeline; this object only publishes the completed mix when it still belongs
// to the live CompositionSession revision. It never owns decoded samples or a device.
class AudioPlaybackSession final : public QObject {
    Q_OBJECT

  public:
    explicit AudioPlaybackSession(CompositionSession& session, QObject* parent = nullptr);

    [[nodiscard]] document::Revision revision() const noexcept { return revision_; }
    [[nodiscard]] const std::optional<runtime::AudioMixDescription>& mix() const noexcept {
        return mix_;
    }

    // Returns false for a stale plan or an evaluator failure. A successful publication is the
    // only way a mix reaches playback, so an older revision can never replace a newer one.
    [[nodiscard]] bool publish(const std::shared_ptr<const runtime::CompiledCompositionPlan>& plan,
                               const runtime::CpuCompositionEvaluator& evaluator,
                               core::RationalTime time);

  signals:
    void mixChanged();

  private:
    void handleSnapshotChanged();

    CompositionSession& session_;
    document::Revision revision_;
    std::optional<runtime::AudioMixDescription> mix_;
};

} // namespace bloom::ui
