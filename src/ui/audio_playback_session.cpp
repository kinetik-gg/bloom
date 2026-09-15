#include <bloom/ui/audio_playback_session.hpp>

#include <bloom/runtime/cancellation.hpp>
#include <bloom/ui/composition_session.hpp>

#include <QThread>

#include <utility>

namespace bloom::ui {

AudioPlaybackSession::AudioPlaybackSession(CompositionSession& session,
                                           const runtime::SnapshotCompiler& compiler,
                                           CompiledPlanCacheHandle planCache,
                                           const runtime::CpuCompositionEvaluator& evaluator,
                                           QObject* parent)
    : QObject(parent), session_(session), compiler_(compiler), planCache_(std::move(planCache)),
      evaluator_(evaluator), revision_(session.snapshot().revision()) {
    if (planCache_ == nullptr)
        planCache_ = std::make_shared<CompiledPlanCache>();
    connect(&session_, &CompositionSession::snapshotChanged, this,
            &AudioPlaybackSession::handleSnapshotChanged);
    connect(&session_, &CompositionSession::currentTimeChanged, this, [this] { (void)refresh(); });
}

bool AudioPlaybackSession::refresh() {
    Q_ASSERT(QThread::currentThread() == thread());
    const auto snapshot = session_.snapshot();
    revision_ = snapshot.revision();
    if (snapshot.project().findComposition(session_.compositionId()) == nullptr) {
        setMix(std::nullopt);
        return false;
    }
    const auto compiled = planCache_->compile(
        compiler_, runtime::SnapshotCompileRequest{snapshot, session_.compositionId()},
        runtime::CancellationToken{});
    if (compiled.status != runtime::SnapshotCompileStatus::Compiled || !compiled.plan) {
        setMix(std::nullopt);
        return false;
    }
    auto evaluated = evaluator_.evaluateAudioMix(compiled.plan, session_.currentTime());
    const bool published = evaluated.has_value();
    setMix(std::move(evaluated));
    return published;
}

void AudioPlaybackSession::handleSnapshotChanged() {
    Q_ASSERT(QThread::currentThread() == thread());
    (void)refresh();
}

void AudioPlaybackSession::setMix(std::optional<runtime::AudioMixDescription> mix) {
    if (mix_ == mix)
        return;
    mix_ = std::move(mix);
    emit mixChanged();
}

} // namespace bloom::ui
