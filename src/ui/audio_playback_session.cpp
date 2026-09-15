#include <bloom/ui/audio_playback_session.hpp>

#include <bloom/ui/composition_session.hpp>

#include <QThread>

namespace bloom::ui {

AudioPlaybackSession::AudioPlaybackSession(CompositionSession& session, QObject* parent)
    : QObject(parent), session_(session), revision_(session.snapshot().revision()) {
    connect(&session_, &CompositionSession::snapshotChanged, this,
            &AudioPlaybackSession::handleSnapshotChanged);
}

bool AudioPlaybackSession::publish(
    const std::shared_ptr<const runtime::CompiledCompositionPlan>& plan,
    const runtime::CpuCompositionEvaluator& evaluator, const core::RationalTime time) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!plan || plan->sourceRevision() != session_.snapshot().revision() ||
        plan->compositionId() != session_.compositionId()) {
        return false;
    }
    const auto evaluated = evaluator.evaluateAudioMix(plan, time);
    if (!evaluated.has_value()) {
        return false;
    }
    revision_ = plan->sourceRevision();
    mix_ = *evaluated;
    emit mixChanged();
    return true;
}

void AudioPlaybackSession::handleSnapshotChanged() {
    Q_ASSERT(QThread::currentThread() == thread());
    revision_ = session_.snapshot().revision();
    mix_.reset();
    emit mixChanged();
}

} // namespace bloom::ui
