#include <bloom/ui/composition_preview_controller.hpp>

#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/playback_controller.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

#include <bloom/document/project.hpp>

#include <QThread>

#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <utility>

namespace bloom::ui {
namespace {

QString submissionFailureMessage(const runtime::TaskSubmissionStatus status) {
    switch (status) {
    case runtime::TaskSubmissionStatus::QueueFull:
        return CompositionPreviewController::tr(
            "The preview could not start because the task queue is full");
    case runtime::TaskSubmissionStatus::ExecutorUnavailable:
        return CompositionPreviewController::tr("The requested task executor is unavailable");
    case runtime::TaskSubmissionStatus::ShuttingDown:
        return CompositionPreviewController::tr("The preview was cancelled during shutdown");
    case runtime::TaskSubmissionStatus::InvalidRequest:
        return CompositionPreviewController::tr("The preview request was invalid");
    case runtime::TaskSubmissionStatus::UnknownGroup:
    case runtime::TaskSubmissionStatus::CancelledGroup:
        return CompositionPreviewController::tr("The preview could not use its task group");
    case runtime::TaskSubmissionStatus::GroupRegistryFull:
        return CompositionPreviewController::tr("The preview could not allocate a task group");
    case runtime::TaskSubmissionStatus::IdExhausted:
        return CompositionPreviewController::tr("The task identifier limit was reached");
    case runtime::TaskSubmissionStatus::Accepted:
        break;
    }
    return CompositionPreviewController::tr("The preview could not start");
}

} // namespace

FrameFreshness CompositionPreviewController::freshnessFor(
    const PreparedPreviewFrameHandle& frame,
    const std::optional<runtime::PreviewRequestIdentity>& desiredIdentity) {
    if (frame == nullptr) {
        return FrameFreshness::None;
    }
    return desiredIdentity.has_value() && frame->desiredIdentity() == *desiredIdentity
               ? FrameFreshness::Current
               : FrameFreshness::Stale;
}

CompositionPreviewController::CompositionPreviewController(
    CompositionSession& session, runtime::TaskScheduler& scheduler, TaskUiBridge& taskUiBridge,
    PreviewPreparationFunction preparation, CompositionPreviewSettings settings,
    PreviewFrameCacheHandle frameCache, QObject* parent)
    : QObject(parent), session_(session), scheduler_(scheduler), taskUiBridge_(taskUiBridge),
      preparation_(std::move(preparation)), settings_(settings),
      frameCache_(frameCache != nullptr
                      ? std::move(frameCache)
                      : std::make_shared<PreviewFrameCache>(settings.ramPreviewByteBudget)) {
    connect(&session_, &CompositionSession::snapshotChanged, this,
            &CompositionPreviewController::requestRefresh);
    connect(&session_, &CompositionSession::compositionChanged, this,
            &CompositionPreviewController::handleCompositionChanged);
    connect(&session_, &CompositionSession::currentTimeChanged, this,
            &CompositionPreviewController::handleCurrentTimeChanged);
    connect(&session_, &CompositionSession::transformInteractionChanged, this,
            &CompositionPreviewController::handleTransformInteractionChanged);
    connect(&session_, &CompositionSession::liveValueChanged, this,
            &CompositionPreviewController::handleLiveValueChanged);
    connect(&session_, &CompositionSession::colorSettingsChanged, this, [this] {
        const auto intent = session_.colorIntent();
        if (settings_.colorIntent == intent) {
            return;
        }
        settings_.colorIntent = intent;
        requestRefresh();
    });
    connect(&taskUiBridge_, &TaskUiBridge::snapshotsPolled, this,
            &CompositionPreviewController::consumeReadyResult);
    interactiveCadenceTimer_.setSingleShot(true);
    interactiveCadenceTimer_.setTimerType(Qt::PreciseTimer);
    connect(&interactiveCadenceTimer_, &QTimer::timeout, this,
            &CompositionPreviewController::flushCadence);
    // Preview presentation must not wait for the Jobs monitor's 100 ms polling interval.
    // This timer only runs while a worker is active; polling only takes a ready result.
    completionPollTimer_.setInterval(4);
    completionPollTimer_.setTimerType(Qt::PreciseTimer);
    connect(&completionPollTimer_, &QTimer::timeout, this,
            &CompositionPreviewController::consumeReadyResult);
    requestPreview(true, PreviewRequestKind::Visible);
}

CompositionPreviewController::~CompositionPreviewController() {
    playbackController_.reset();
    cancelAndDetachActive();
}

PlaybackController& CompositionPreviewController::playbackController() {
    if (playbackController_ == nullptr) {
        playbackController_ = std::make_unique<PlaybackController>(session_, *this);
    }
    return *playbackController_;
}

const CompositionPreviewState& CompositionPreviewController::state() const noexcept {
    return state_;
}

std::vector<runtime::EvaluatedOperationBounds>
CompositionPreviewController::selectedLayerBounds() const {
    std::vector<runtime::EvaluatedOperationBounds> result;
    if (!state_.frame || state_.frame->desiredIdentity().compositionId != session_.compositionId())
        return result;
    const auto& selection = session_.selection();
    const auto* primary = std::get_if<document::LayerId>(&selection.primary);
    const auto layer = primary ? std::optional(*primary) : selection.contextualLayer;
    const auto& nodes = session_.selectedNodes();
    const auto& frame = *state_.frame;
    const auto operations = frame.processIdentity().plan->operations();
    const auto allBounds = frame.evaluatedBounds();
    for (std::size_t index = 0; index < allBounds.size(); ++index) {
        const auto& bounds = allBounds[index];
        const auto* boundary = std::get_if<runtime::CompiledLayerOutput>(&operations[index]);
        if (!boundary || !bounds.layerId.isValid() || bounds.output.empty())
            continue;
        const bool selected =
            (layer && bounds.layerId == *layer) || nodes.contains(boundary->sourceNodeId);
        if (selected)
            result.push_back(bounds);
    }
    return result;
}

bool CompositionPreviewController::isShuttingDown() const noexcept { return shuttingDown_; }

bool CompositionPreviewController::backgroundWorkAllowed() const noexcept {
    return !shuttingDown_ && !active_.has_value() && !pending_.has_value() &&
           !interactiveTimeChangeArmed_ && !session_.valueEditActive() &&
           session_.transformInteractionOverrides().empty() && !ramPreviewProgress_.has_value();
}

PreviewFrameCache& CompositionPreviewController::frameCache() const noexcept {
    return *frameCache_;
}

std::optional<PreviewFrameCacheKey>
CompositionPreviewController::cacheKeyForTime(const core::RationalTime time) const {
    const auto& snapshot = session_.snapshot();
    const auto compositionId = session_.compositionId();
    if (snapshot.project().findComposition(compositionId) == nullptr) {
        return std::nullopt;
    }
    return PreviewFrameCacheKey{
        .projectId = snapshot.project().id(),
        .compositionId = compositionId,
        .sourceRevision = snapshot.revision(),
        .time = time,
        .output = runtime::PreviewOutput::Composition,
        .resolution = resolution(),
        .quality = settings_.quality,
        .colorIntent = settings_.colorIntent,
        .resolutionPolicy = settings_.resolutionPolicy,
        .roi = regionOfInterest(),
    };
}

void CompositionPreviewController::setRegionOfInterest(std::optional<QRectF> region) {
    if (region &&
        (!std::isfinite(region->left()) || !std::isfinite(region->top()) ||
         !std::isfinite(region->right()) || !std::isfinite(region->bottom()) || region->isEmpty()))
        return;
    if (regionOfInterest_ == region || shuttingDown_)
        return;
    regionOfInterest_ = region;
    requestRefresh();
}

std::optional<render::ImageWindow> CompositionPreviewController::regionOfInterest() const {
    if (!regionOfInterest_)
        return std::nullopt;
    const auto* composition =
        session_.snapshot().project().findComposition(session_.compositionId());
    if (!composition)
        return std::nullopt;
    const auto format = composition->format();
    auto width = format.width();
    auto height = format.height();
    const auto requested = resolution();
    if (const auto* proxy = std::get_if<runtime::ProxyResolution>(&requested)) {
        width = proxy->extent.width();
        height = proxy->extent.height();
    }
    const double sx = static_cast<double>(width) / format.width();
    const double sy = static_cast<double>(height) / format.height();
    const auto left =
        std::clamp(std::floor(regionOfInterest_->left() * sx), 0.0, double(width - 1));
    const auto top = std::clamp(std::floor(regionOfInterest_->top() * sy), 0.0, double(height - 1));
    const auto right =
        std::clamp(std::ceil(regionOfInterest_->right() * sx), left + 1, double(width));
    const auto bottom =
        std::clamp(std::ceil(regionOfInterest_->bottom() * sy), top + 1, double(height));
    const auto window = render::ImageWindow::create(
        static_cast<std::int64_t>(left), static_cast<std::int64_t>(top),
        static_cast<std::uint64_t>(right - left), static_cast<std::uint64_t>(bottom - top));
    return *window.value();
}

std::uint32_t CompositionPreviewController::resolutionDivisor() const noexcept {
    switch (settings_.resolutionPolicy) {
    case runtime::PreviewResolutionPolicy::Full:
        return 1;
    case runtime::PreviewResolutionPolicy::Half:
        return 2;
    case runtime::PreviewResolutionPolicy::Quarter:
        return 4;
    case runtime::PreviewResolutionPolicy::Auto:
        break;
    }
    const auto* composition = session_.composition();
    if (composition == nullptr || displayedCompositionScale_ >= 1.0) {
        return 1;
    }
    const auto format = composition->format();
    for (const std::uint32_t divisor : {4U, 2U}) {
        const auto width = (format.width() + divisor - 1) / divisor;
        const auto height = (format.height() + divisor - 1) / divisor;
        if (width >= format.width() * displayedCompositionScale_ &&
            height >= format.height() * displayedCompositionScale_) {
            return divisor;
        }
    }
    return 1;
}

runtime::EvaluationResolution CompositionPreviewController::resolution() const {
    const auto divisor = resolutionDivisor();
    const auto* composition = session_.composition();
    if (divisor == 1 || composition == nullptr) {
        return runtime::CompositionFormatResolution{};
    }
    const auto format = composition->format();
    const auto extent = render::ImageExtent::create((format.width() + divisor - 1) / divisor,
                                                    (format.height() + divisor - 1) / divisor);
    return runtime::ProxyResolution{*extent.value()};
}

void CompositionPreviewController::setResolutionPolicy(
    const runtime::PreviewResolutionPolicy policy) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (shuttingDown_ || settings_.resolutionPolicy == policy) {
        return;
    }
    preparationEstimate_.reset();
    settings_.resolutionPolicy = policy;
    preparationEstimate_.reset();
    emit resolutionChanged();
    requestPreview(false, PreviewRequestKind::Visible);
}

void CompositionPreviewController::setDisplayedCompositionScale(const double scale) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (shuttingDown_) {
        return;
    }
    const auto previous = resolution();
    displayedCompositionScale_ = std::isfinite(scale) && scale > 0.0 ? scale : 1.0;
    if (previous == resolution()) {
        return;
    }
    preparationEstimate_.reset();
    emit resolutionChanged();
    requestPreview(false, PreviewRequestKind::Visible);
}

void CompositionPreviewController::requestRefresh() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!shuttingDown_) {
        if (session_.valueEditActive()) {
            handleLiveValueChanged();
            return;
        }
        valueEditPreviewActive_ = false;
        preparationEstimate_.reset();
        // Deliberately NOT from the cache: see requestPreview()'s `allowCachedFrame`.
        requestPreview(false, PreviewRequestKind::Visible, false);
    }
}

void CompositionPreviewController::handleCompositionChanged() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!shuttingDown_) {
        preparationEstimate_.reset();
        requestPreview(true, PreviewRequestKind::Visible);
    }
}

void CompositionPreviewController::handleCurrentTimeChanged() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (shuttingDown_ || playbackActive_) {
        return;
    }
    if (state_.desiredIdentity.has_value() &&
        state_.desiredIdentity->compositionId == session_.compositionId() &&
        state_.desiredIdentity->time == session_.currentTime()) {
        return;
    }
    // Every current-time change advances the desired request generation (docs/architecture/
    // animation-and-time.md, "Session Time And Scrubbing"); which priority it advances at depends
    // on whether the change came from an armed Interactive gesture (see beginInteractiveScrub()) or
    // a discrete change such as typed time entry, key selection, or document refresh.
    requestPreview(false, interactiveTimeChangeArmed_ ? PreviewRequestKind::Interactive
                                                      : PreviewRequestKind::Visible);
}

void CompositionPreviewController::handleTransformInteractionChanged() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (shuttingDown_) {
        return;
    }
    // docs/architecture/animation-and-time.md: "Scrub, playback, and direct manipulation use
    // Interactive priority". beginInteractiveScrub()'s arming flag already means exactly "an
    // Interactive pointer gesture is driving preview-affecting signals right now" -- the Viewer
    // arms it for a position-interaction gesture the same way TimelineRuler arms it for a scrub, so
    // this reuses it rather than adding a duplicate boolean.
    requestPreview(false, interactiveTimeChangeArmed_ ? PreviewRequestKind::Interactive
                                                      : PreviewRequestKind::Visible);
}

void CompositionPreviewController::handleLiveValueChanged() {
    if (shuttingDown_)
        return;
    if (session_.valueEditActive()) {
        if (!valueEditPreviewActive_)
            interactiveSubmissionClock_.invalidate();
        valueEditPreviewActive_ = true;
        requestPreview(false, PreviewRequestKind::Interactive, false);
    } else if (valueEditPreviewActive_) {
        valueEditPreviewActive_ = false;
        requestPreview(false, PreviewRequestKind::Visible, false);
    }
}

void CompositionPreviewController::beginInteractiveScrub() {
    Q_ASSERT(QThread::currentThread() == thread());
    interactiveTimeChangeArmed_ = true;
    interactiveSubmissionClock_.invalidate();
    emit interactiveScrubStarted();
    emit foregroundWorkRequested();
}

void CompositionPreviewController::notifyScrubEnded() {
    Q_ASSERT(QThread::currentThread() == thread());
    interactiveTimeChangeArmed_ = false;
    interactiveSubmissionClock_.invalidate();
    if (!interactiveCadenceTimer_.isActive()) {
        // Either nothing is pending, or the active-request gate is already holding the newest
        // pending request (it will submit once the active task reaches terminal) -- the gate is
        // untouched either way.
        return;
    }
    interactiveCadenceTimer_.stop();
    flushCadence();
}

std::uint64_t CompositionPreviewController::droppedFrameCount() const noexcept {
    return droppedFrameCount_;
}

bool CompositionPreviewController::isCountingDroppedFrames() const noexcept {
    return countingDroppedFrames_;
}

void CompositionPreviewController::beginDroppedFrameCounting() {
    Q_ASSERT(QThread::currentThread() == thread());
    countingDroppedFrames_ = true;
    droppedFrameCount_ = 0;
    emit droppedFrameCountChanged();
}

void CompositionPreviewController::endDroppedFrameCounting() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!countingDroppedFrames_) {
        return;
    }
    countingDroppedFrames_ = false;
    // The count is kept until the next beginDroppedFrameCounting() resets it, so a surface can
    // still report what the run that just ended dropped; it reads isCountingDroppedFrames() to
    // decide whether to show it at all.
    emit droppedFrameCountChanged();
}

const std::optional<RamPreviewProgress>&
CompositionPreviewController::ramPreviewProgress() const noexcept {
    return ramPreviewProgress_;
}

void CompositionPreviewController::beginRamPreviewProgress(const std::uint64_t totalFrames) {
    Q_ASSERT(QThread::currentThread() == thread());
    emit foregroundWorkRequested();
    ramPreviewProgress_ = RamPreviewProgress{.cachedFrames = 0, .totalFrames = totalFrames};
    emit ramPreviewProgressChanged();
}

void CompositionPreviewController::setRamPreviewProgress(const std::uint64_t cachedFrames) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!ramPreviewProgress_.has_value() || ramPreviewProgress_->cachedFrames == cachedFrames) {
        return;
    }
    ramPreviewProgress_->cachedFrames = cachedFrames;
    emit ramPreviewProgressChanged();
}

void CompositionPreviewController::endRamPreviewProgress() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!ramPreviewProgress_.has_value()) {
        return;
    }
    ramPreviewProgress_.reset();
    emit ramPreviewProgressChanged();
}

const CompositionPreviewSettings& CompositionPreviewController::settings() const noexcept {
    return settings_;
}

void CompositionPreviewController::noteDroppedFrame() { noteDroppedFrames(1); }

void CompositionPreviewController::flushCadence() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!pending_.has_value() || active_.has_value()) {
        return;
    }
    if (pending_->kind == PreviewRequestKind::Interactive &&
        interactiveSubmissionClock_.isValid()) {
        const auto remaining =
            settings_.interactiveTrailingCadence.count() - interactiveSubmissionClock_.elapsed();
        if (remaining > 0) {
            interactiveCadenceTimer_.start(static_cast<int>(remaining));
            return;
        }
    }
    interactiveCadenceTimer_.stop();
    PendingRequest request = std::move(*pending_);
    pending_.reset();
    submitPreview(std::move(request), state_.frame);
}

void CompositionPreviewController::beginShutdown() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (shuttingDown_) {
        return;
    }

    if (playbackController_ != nullptr) {
        playbackController_->pause();
    }
    shuttingDown_ = true;
    emit foregroundWorkRequested();
    disconnect(&session_, nullptr, this, nullptr);
    interactiveCadenceTimer_.stop();
    interactiveTimeChangeArmed_ = false;
    pending_.reset();
    cancelAndDetachActive();

    CompositionPreviewState cancelled = state_;
    cancelled.activity = PreviewActivity::Cancelled;
    cancelled.freshness = freshnessFor(cancelled.frame, cancelled.desiredIdentity);
    cancelled.taskId.reset();
    cancelled.diagnostics.clear();
    cancelled.message = tr("Preview rendering was cancelled during application shutdown");
    publish(std::move(cancelled));
}

void CompositionPreviewController::requestPreview(const bool clearLastGoodFrame,
                                                  const PreviewRequestKind kind,
                                                  const bool allowCachedFrame) {
    Q_ASSERT(QThread::currentThread() == thread());

    if (active_.has_value() && active_->playbackOutstanding) {
        active_->playbackOutstanding = false;
        active_->handle.cancel();
        noteDroppedFrame();
    }
    const document::Snapshot snapshot = session_.snapshot();
    const document::CompositionId compositionId = session_.compositionId();
    PreparedPreviewFrameHandle retainedFrame = clearLastGoodFrame ? nullptr : state_.frame;
    if (retainedFrame != nullptr &&
        (retainedFrame->desiredIdentity().projectId != snapshot.project().id() ||
         retainedFrame->desiredIdentity().compositionId != compositionId)) {
        retainedFrame.reset();
    }

    if (generation_ == std::numeric_limits<std::uint64_t>::max()) {
        interactiveCadenceTimer_.stop();
        pending_.reset();
        if (active_.has_value()) {
            active_->handle.cancel();
        }
        CompositionPreviewState failed{
            .activity = PreviewActivity::Failed,
            .freshness = FrameFreshness::None,
            .desiredIdentity = std::nullopt,
            .taskId = std::nullopt,
            .frame = std::move(retainedFrame),
            .diagnostics = {},
            .message = tr("The preview request generation limit was reached"),
        };
        failed.freshness = freshnessFor(failed.frame, failed.desiredIdentity);
        publish(std::move(failed));
        return;
    }
    const std::uint64_t generation = ++generation_;
    const runtime::PreviewRequestIdentity desiredIdentity{
        .projectId = snapshot.project().id(),
        .compositionId = compositionId,
        .sourceRevision = snapshot.revision(),
        .requestGeneration = generation,
        .time = session_.currentTime(),
        .output = runtime::PreviewOutput::Composition,
        .resolution = resolution(),
        .quality = settings_.quality,
        .colorIntent = settings_.colorIntent,
        .resolutionPolicy = settings_.resolutionPolicy,
        .roi = regionOfInterest(),
    };

    const auto publishTerminal = [this, &desiredIdentity,
                                  &retainedFrame](const PreviewActivity activity, QString message) {
        interactiveCadenceTimer_.stop();
        pending_.reset();
        if (active_.has_value()) {
            active_->handle.cancel();
        }
        CompositionPreviewState terminal{
            .activity = activity,
            .freshness = FrameFreshness::None,
            .desiredIdentity = desiredIdentity,
            .taskId = std::nullopt,
            .frame = retainedFrame,
            .diagnostics = {},
            .message = std::move(message),
        };
        terminal.freshness = freshnessFor(terminal.frame, terminal.desiredIdentity);
        publish(std::move(terminal));
    };

    if (snapshot.project().findComposition(compositionId) == nullptr) {
        publishTerminal(PreviewActivity::Unsupported,
                        tr("No composition is available for preview rendering"));
        return;
    }
    if (!preparation_) {
        publishTerminal(PreviewActivity::Failed,
                        tr("No composition preview pipeline is available"));
        return;
    }
    if (settings_.pixelStorageByteLimit == 0) {
        publishTerminal(PreviewActivity::Failed,
                        tr("The composition preview memory budget is invalid"));
        return;
    }

    // Overrides ride ONLY Interactive requests from an active gesture (docs/architecture/
    // animation-and-time.md), and are read fresh here -- never cached across requests.
    std::vector<runtime::SnapshotParameterOverride> interactionOverride;
    if (kind == PreviewRequestKind::Interactive) {
        interactionOverride = session_.transformInteractionOverrides();
        const auto values = session_.valueEditOverrides();
        interactionOverride.insert(interactionOverride.end(), values.begin(), values.end());
    }

    // The RAM preview cache (docs/architecture/animation-and-time.md, "RAM preview"). A request
    // whose key is cached is answered right here: no task, no coalescing, no cadence -- which is
    // what makes cached playback frame-accurate rather than best-effort. An overridden request is
    // never served from the cache, because its pixels are the gesture's, not the revision's.
    if (allowCachedFrame && interactionOverride.empty()) {
        if (auto cached = frameCache_->take(desiredIdentity); cached != nullptr) {
            interactiveCadenceTimer_.stop();
            if (pending_.has_value()) {
                // A request that was waiting to be submitted and never will be: the frame it asked
                // for was never delivered, which is exactly what droppedFrameCount() counts.
                noteDroppedFrame();
                pending_.reset();
            }
            // An in-flight task is for an older ask. Cancelling it leaves the admission gate closed
            // until its terminal result is observed, which consumeReadyResult() already handles;
            // the cached frame is published now regardless.
            if (active_.has_value()) {
                active_->handle.cancel();
            }
            publishCachedFrame(desiredIdentity, std::move(cached));
            return;
        }
    }

    if (kind == PreviewRequestKind::Playback &&
        (active_.has_value() || pending_.has_value() || !preparationEstimate_.has_value() ||
         *preparationEstimate_ > playbackBudget_ / 2)) {
        noteDroppedFrame();
        publishTerminal(PreviewActivity::Cancelled,
                        tr("Playback skipped this frame; showing the previous frame"));
        return;
    }

    emit foregroundWorkRequested();
    PendingRequest pendingRequest{.snapshot = snapshot,
                                  .desiredIdentity = desiredIdentity,
                                  .pixelStorageByteLimit = settings_.pixelStorageByteLimit,
                                  .kind = kind,
                                  .interactionOverride = interactionOverride};

    if (active_.has_value()) {
        // Finish one live frame while coalescing newer pointer/text input. Cancelling every
        // active frame starves presentation for as long as the artist keeps moving.
        if (!active_->carriedInteractionOverride || interactionOverride.empty() ||
            !liveSessionMatches(active_->desiredIdentity) ||
            active_->desiredIdentity.time != desiredIdentity.time)
            active_->handle.cancel();
        // The cancelled handle remains the admission gate until its terminal result is observed.
        // Cadence is irrelevant beneath this gate: it delays SUBMISSION, and this request cannot
        // submit before the active task reaches terminal regardless of kind or timer state.
        if (pending_.has_value()) {
            noteDroppedFrame();
        }
        pending_.emplace(std::move(pendingRequest));
        publishRendering(desiredIdentity, std::nullopt, std::move(retainedFrame));
        taskUiBridge_.wake();
        return;
    }

    if (kind == PreviewRequestKind::Interactive) {
        // The first request submits immediately. Only subsequent requests share the trailing
        // window, always retaining the newest value.
        if (pending_.has_value()) {
            noteDroppedFrame();
        }
        pending_.emplace(std::move(pendingRequest));
        publishRendering(desiredIdentity, std::nullopt, std::move(retainedFrame));
        flushCadence();
        return;
    }

    // Visible bypasses the cadence entirely: any Interactive request still waiting out its window
    // is superseded immediately.
    interactiveCadenceTimer_.stop();
    interactiveSubmissionClock_.invalidate();
    if (pending_.has_value()) {
        noteDroppedFrame();
    }
    pending_.reset();
    submitPreview(std::move(pendingRequest), std::move(retainedFrame));
}

void CompositionPreviewController::submitPreview(PendingRequest pendingRequest,
                                                 PreparedPreviewFrameHandle retainedFrame) {
    Q_ASSERT(QThread::currentThread() == thread());
    Q_ASSERT(!active_.has_value());

    const auto desiredIdentity = pendingRequest.desiredIdentity;
    const auto priority = pendingRequest.kind == PreviewRequestKind::Interactive
                              ? runtime::TaskPriority::Interactive
                              : runtime::TaskPriority::Visible;

    runtime::TaskRequest request(
        "Render composition preview",
        {.kind = runtime::TaskOwnerKind::Composition,
         .id = runtime::TaskOwnerId::fromRaw(desiredIdentity.compositionId.value())},
        priority);
    request.coalescingKey = "bloom.preview.render";
    request.sourceVersion = {
        .documentRevision = desiredIdentity.sourceRevision.value(),
        .requestGeneration = desiredIdentity.requestGeneration,
    };

    auto preparation = preparation_;
    const std::size_t pixelStorageByteLimit = pendingRequest.pixelStorageByteLimit;
    const auto interactionOverride = pendingRequest.interactionOverride;
    const auto submittedAt = std::chrono::steady_clock::now();
    auto submission = scheduler_.submit<PreviewPreparationResultHandle>(
        std::move(request),
        [snapshot = std::move(pendingRequest.snapshot), desiredIdentity, pixelStorageByteLimit,
         interactionOverride,
         preparation = std::move(preparation)](runtime::TaskContext& context) mutable {
            if (context.isCancellationRequested()) {
                return runtime::TaskResult<PreviewPreparationResultHandle>::cancelled();
            }
            return preparation(snapshot, desiredIdentity, pixelStorageByteLimit,
                               interactionOverride, context);
        });

    if (!submission.accepted()) {
        if (pendingRequest.kind == PreviewRequestKind::Playback) {
            noteDroppedFrame();
        }
        CompositionPreviewState rejected{
            .activity = submission.status == runtime::TaskSubmissionStatus::ShuttingDown
                            ? PreviewActivity::Cancelled
                            : PreviewActivity::Failed,
            .freshness = FrameFreshness::None,
            .desiredIdentity = desiredIdentity,
            .taskId = std::nullopt,
            .frame = std::move(retainedFrame),
            .diagnostics = {},
            .message = submissionFailureMessage(submission.status),
        };
        if (submission.diagnostic.has_value()) {
            rejected.diagnostics.push_back(std::move(*submission.diagnostic));
        }
        rejected.freshness = freshnessFor(rejected.frame, rejected.desiredIdentity);
        publish(std::move(rejected));
        return;
    }

    const runtime::TaskId taskId = submission.handle.id();
    active_.emplace(
        ActiveRequest{.handle = std::move(submission.handle),
                      .desiredIdentity = desiredIdentity,
                      .carriedInteractionOverride = !interactionOverride.empty(),
                      .submittedAt = submittedAt,
                      .playbackDeadline = pendingRequest.kind == PreviewRequestKind::Playback
                                              ? std::optional{submittedAt + playbackBudget_}
                                              : std::nullopt,
                      .playbackOutstanding = pendingRequest.kind == PreviewRequestKind::Playback});
    if (pendingRequest.kind == PreviewRequestKind::Interactive)
        interactiveSubmissionClock_.restart();
    completionPollTimer_.start();
    publishRendering(desiredIdentity, taskId, std::move(retainedFrame));
    taskUiBridge_.wake();
}

void CompositionPreviewController::publishCachedFrame(
    const runtime::PreviewRequestIdentity& desiredIdentity, PreparedPreviewFrameHandle frame) {
    CompositionPreviewState ready{
        .activity = PreviewActivity::Ready,
        .freshness = FrameFreshness::Current,
        .desiredIdentity = desiredIdentity,
        .taskId = std::nullopt,
        .frame = std::move(frame),
        .diagnostics = {},
        .message = tr("The current composition frame is ready"),
    };
    publish(std::move(ready));
}

void CompositionPreviewController::publishRendering(runtime::PreviewRequestIdentity desiredIdentity,
                                                    std::optional<runtime::TaskId> taskId,
                                                    PreparedPreviewFrameHandle retainedFrame) {
    CompositionPreviewState rendering{
        .activity = PreviewActivity::Rendering,
        .freshness = FrameFreshness::None,
        .desiredIdentity = desiredIdentity,
        .taskId = taskId,
        .frame = std::move(retainedFrame),
        .diagnostics = {},
        .message = {},
    };
    rendering.freshness = freshnessFor(rendering.frame, rendering.desiredIdentity);
    rendering.message = rendering.freshness == FrameFreshness::Stale
                            ? tr("Rendering the current composition; showing the previous frame")
                            : tr("Rendering the current composition");
    publish(std::move(rendering));
}

void CompositionPreviewController::cancelAndDetachActive() noexcept {
    completionPollTimer_.stop();
    if (!active_.has_value()) {
        return;
    }
    active_->handle.cancel();
    active_.reset();
}

void CompositionPreviewController::publish(CompositionPreviewState state) {
    Q_ASSERT((state.freshness == FrameFreshness::None) == (state.frame == nullptr));
    Q_ASSERT(state.freshness != FrameFreshness::Current ||
             (state.desiredIdentity.has_value() && state.frame != nullptr &&
              state.frame->desiredIdentity() == *state.desiredIdentity));
    Q_ASSERT(
        state.freshness != FrameFreshness::Stale ||
        (state.frame != nullptr && (!state.desiredIdentity.has_value() ||
                                    state.frame->desiredIdentity() != *state.desiredIdentity)));
    Q_ASSERT(state.activity != PreviewActivity::Ready ||
             state.freshness == FrameFreshness::Current);
    state_ = std::move(state);
    emit stateChanged();
}

bool CompositionPreviewController::isCurrent(const ActiveRequest& request) const {
    return request.desiredIdentity.requestGeneration == generation_ &&
           state_.desiredIdentity.has_value() &&
           *state_.desiredIdentity == request.desiredIdentity &&
           state_.taskId == request.handle.id();
}

bool CompositionPreviewController::liveSessionMatches(
    const runtime::PreviewRequestIdentity& desiredIdentity) const noexcept {
    const auto& snapshot = session_.snapshot();
    return session_.compositionId() == desiredIdentity.compositionId &&
           snapshot.revision() == desiredIdentity.sourceRevision &&
           snapshot.project().id() == desiredIdentity.projectId;
}

} // namespace bloom::ui
