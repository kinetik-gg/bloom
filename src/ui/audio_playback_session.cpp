#include <bloom/ui/audio_playback_session.hpp>

#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/ui/asset_controller.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

#include <QThread>

#include <algorithm>
#include <limits>
#include <utility>

namespace bloom::ui {

struct AudioPlaybackSession::Preparation final {
    std::optional<runtime::AudioMixDescription> mix;
    bool timeDependent = true;
};

AudioPlaybackSession::AudioPlaybackSession(CompositionSession& session,
                                           runtime::TaskScheduler& scheduler, TaskUiBridge& bridge,
                                           const runtime::SnapshotCompiler& compiler,
                                           CompiledPlanCacheHandle planCache,
                                           const runtime::CpuCompositionEvaluator& evaluator,
                                           QObject* parent)
    : QObject(parent), session_(session), scheduler_(scheduler), bridge_(bridge),
      compiler_(compiler), planCache_(std::move(planCache)), evaluator_(evaluator),
      revision_(session.snapshot().revision()) {
    if (planCache_ == nullptr)
        planCache_ = std::make_shared<CompiledPlanCache>();
    connect(&session_, &CompositionSession::snapshotChanged, this,
            &AudioPlaybackSession::handleSnapshotChanged);
    connect(&session_, &CompositionSession::compositionChanged, this,
            &AudioPlaybackSession::handleCompositionChanged);
    connect(&session_, &CompositionSession::currentTimeChanged, this, [this] {
        if (!staticForCurrent_)
            (void)refresh();
    });
    connect(&bridge_, &TaskUiBridge::resultsAvailable, this, &AudioPlaybackSession::consumeReady);
    connect(&bridge_, &TaskUiBridge::snapshotsPolled, this, &AudioPlaybackSession::submitPending);
}

AudioPlaybackSession::~AudioPlaybackSession() { beginShutdown(); }

void AudioPlaybackSession::setAudioBufferSource(AssetController& source) {
    Q_ASSERT(QThread::currentThread() == thread());
    Q_ASSERT(audioSource_ == nullptr);
    audioSource_ = &source;
    connect(&source, &AssetController::changed, this, [this] { refreshPins(true); });
    refreshPins(true);
}

std::shared_ptr<const media::audio::AudioBuffer>
AudioPlaybackSession::audioBuffer(const document::AssetId id, const std::uint32_t stream) const {
    const auto found = pinnedAudio_.find(runtime::AudioStreamKey{id, stream});
    return found == pinnedAudio_.end() ? nullptr : found->second;
}

bool AudioPlaybackSession::refresh() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (shuttingDown_)
        return false;
    const auto snapshot = session_.snapshot();
    const auto compositionId = session_.compositionId();
    revision_ = snapshot.revision();
    if (snapshot.project().findComposition(compositionId) == nullptr) {
        if (active_)
            active_->handle.cancel();
        pending_.reset();
        staticForCurrent_ = false;
        setMix(std::nullopt);
        return false;
    }
    if (generation_ == std::numeric_limits<std::uint64_t>::max()) {
        beginShutdown();
        return false;
    }
    ++generation_;
    pending_.emplace(Request{snapshot, compositionId, session_.currentTime(), generation_});
    if (active_ && (&active_->request.snapshot.project() != &snapshot.project() ||
                    active_->request.compositionId != compositionId ||
                    active_->request.snapshot.revision() != snapshot.revision())) {
        // #358: the previous mix stays audible until the newer request publishes its own.
        active_->handle.cancel();
        staticForCurrent_ = false;
    }
    submitPending();
    return true;
}

void AudioPlaybackSession::beginShutdown() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (shuttingDown_)
        return;
    shuttingDown_ = true;
    if (active_)
        active_->handle.cancel();
    pending_.reset();
    staticForCurrent_ = false;
    setMix(std::nullopt);
}

void AudioPlaybackSession::handleSnapshotChanged() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (shuttingDown_)
        return;
    // #358: an edit that cannot change the active composition's render -- layout only, proven
    // unreachable, or in a composition this one does not nest -- cannot change its audio either.
    // The published mix still describes the document, so nothing is re-derived or silenced.
    if (!session_.lastDocumentChangeAffectedEvaluation() && mix_.has_value() && !active_ &&
        !pending_) {
        ++work_.neutralEdits;
        revision_ = session_.snapshot().revision();
        return;
    }
    // A render-affecting edit re-derives the mix; the previous one keeps playing until the new
    // one is ready, and consumeReady() never publishes a result for an older revision.
    staticForCurrent_ = false;
    (void)refresh();
}

void AudioPlaybackSession::handleCompositionChanged() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (shuttingDown_)
        return;
    staticForCurrent_ = false;
    if (active_)
        active_->handle.cancel();
    // Another composition (or a rebound document) must never be heard with this one's mix.
    silence();
    (void)refresh();
}

void AudioPlaybackSession::silence() {
    if (mix_.has_value())
        ++work_.silences;
    setMix(std::nullopt);
}

void AudioPlaybackSession::submitPending() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (shuttingDown_ || active_ || !pending_)
        return;
    const auto request = *pending_;
    runtime::TaskRequest task("Prepare audio mix",
                              {.kind = runtime::TaskOwnerKind::Composition,
                               .id = runtime::TaskOwnerId::fromRaw(request.compositionId.value())},
                              runtime::TaskPriority::Visible);
    task.sourceVersion = {.documentRevision = request.snapshot.revision().value(),
                          .requestGeneration = request.generation};
    const auto submission = scheduler_.submit<ResultHandle>(
        std::move(task), [request, compiler = &compiler_, evaluator = &evaluator_,
                          cache = planCache_](runtime::TaskContext& context) {
            if (context.isCancellationRequested())
                return runtime::TaskResult<ResultHandle>::cancelled();
            const auto compiled = cache->compile(
                *compiler, runtime::SnapshotCompileRequest{request.snapshot, request.compositionId},
                context.cancellation());
            if (context.isCancellationRequested())
                return runtime::TaskResult<ResultHandle>::cancelled();
            auto prepared = std::make_shared<Preparation>();
            if (compiled.status == runtime::SnapshotCompileStatus::Compiled && compiled.plan) {
                const auto& audio = compiled.plan->audioMix();
                // A value-driven Time Remap is resolved once per mix (ADR 0024), so it is
                // re-evaluated with the transport like an animated level.
                prepared->timeDependent =
                    !audio.nestedLayers.empty() ||
                    std::ranges::any_of(audio.sources,
                                        [](const auto& source) {
                                            return !std::holds_alternative<double>(
                                                source.level.source);
                                        }) ||
                    std::ranges::any_of(audio.layers, [](const auto& layer) {
                        return layer.time && layer.time->timeRemap &&
                               std::holds_alternative<runtime::ValueOutputIndex>(
                                   layer.time->timeRemap->source);
                    });
                prepared->mix = evaluator->evaluateAudioMix(compiled.plan, request.time,
                                                            context.cancellation());
            } else {
                prepared->timeDependent = false;
            }
            if (context.isCancellationRequested())
                return runtime::TaskResult<ResultHandle>::cancelled();
            return runtime::TaskResult<ResultHandle>::succeeded(ResultHandle{std::move(prepared)});
        });
    if (!submission.accepted()) {
        // A full queue may free on a bridge poll. Closed admission is terminal for this session.
        if (submission.status == runtime::TaskSubmissionStatus::ShuttingDown) {
            pending_.reset();
            setMix(std::nullopt);
        }
        return;
    }
    ++work_.mixRequests;
    active_.emplace(Active{request, submission.handle});
    pending_.reset();
    bridge_.wake();
}

void AudioPlaybackSession::consumeReady() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!active_)
        return;
    auto result = active_->handle.tryTakeResult();
    if (!result)
        return;
    auto request = std::move(active_->request);
    active_.reset();
    if (!shuttingDown_) {
        const auto current = session_.snapshot();
        const bool sameRevision = request.compositionId == session_.compositionId() &&
                                  request.snapshot.revision() == current.revision() &&
                                  &request.snapshot.project() == &current.project();
        if (sameRevision && result->state() == runtime::TaskState::Succeeded &&
            result->value().has_value() && *result->value()) {
            const auto& prepared = **result->value();
            if (!prepared.timeDependent ||
                (request.generation == generation_ && request.time == session_.currentTime())) {
                staticForCurrent_ = !prepared.timeDependent;
                if (staticForCurrent_)
                    pending_.reset();
                setMix(prepared.mix);
            }
        }
        submitPending();
    }
}

void AudioPlaybackSession::setMix(std::optional<runtime::AudioMixDescription> mix) {
    if (mix_ == mix) {
        if (shuttingDown_) {
            pinnedAudio_.clear();
        }
        if (audioSource_ && (shuttingDown_ || (!active_ && !pending_)))
            audioSource_->releaseAudioDelivery();
        return;
    }
    mix_ = std::move(mix);
    refreshPins(false);
    emit mixChanged();
}

void AudioPlaybackSession::refreshPins(const bool notify) {
    if (!audioSource_)
        return;
    decltype(pinnedAudio_) next;
    if (mix_ && !shuttingDown_)
        for (const auto& clip : mix_->clips) {
            const runtime::AudioStreamKey key{clip.assetId, clip.audioStream};
            auto buffer = audioSource_->audioBuffer(clip.assetId, clip.audioStream);
            if (!buffer && !audioSource_->missing(clip.assetId)) {
                const auto old = pinnedAudio_.find(key);
                if (old != pinnedAudio_.end())
                    buffer = old->second;
            }
            if (buffer)
                next.insert_or_assign(key, std::move(buffer));
        }
    const bool changed = next != pinnedAudio_;
    pinnedAudio_ = std::move(next);
    if (mix_ || shuttingDown_ || (!active_ && !pending_ && session_.composition() == nullptr))
        audioSource_->releaseAudioDelivery();
    if (notify && changed)
        emit mixChanged();
}

} // namespace bloom::ui
