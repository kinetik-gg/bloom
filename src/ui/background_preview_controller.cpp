#include <bloom/ui/background_preview_controller.hpp>

#include <bloom/core/frame_time_mapping.hpp>
#include <bloom/document/project.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

#include <QApplication>
#include <QEvent>

#include <algorithm>
#include <limits>
#include <utility>

namespace bloom::ui {

BackgroundPreviewController::BackgroundPreviewController(
    CompositionSession& session, CompositionPreviewController& previewController,
    runtime::TaskScheduler& scheduler, TaskUiBridge& bridge, PreviewPreparationFunction preparation,
    QObject* parent)
    : QObject(parent), session_(session), previewController_(previewController),
      scheduler_(scheduler), bridge_(bridge), preparation_(std::move(preparation)) {
    qApp->installEventFilter(this);
    idleTimer_.setInterval(50);
    connect(&idleTimer_, &QTimer::timeout, this, &BackgroundPreviewController::fillNextFrame);
    connect(&bridge_, &TaskUiBridge::snapshotsPolled, this,
            &BackgroundPreviewController::consumeReadyResult);
    connect(&previewController_, &CompositionPreviewController::foregroundWorkRequested, this,
            &BackgroundPreviewController::restart);
    connect(&previewController_, &CompositionPreviewController::playbackActiveChanged, this,
            &BackgroundPreviewController::setPlaying);
    connect(&previewController_, &CompositionPreviewController::resolutionChanged, this,
            &BackgroundPreviewController::restart);
    connect(&session_, &CompositionSession::snapshotChanged, this,
            &BackgroundPreviewController::restart);
    connect(&session_, &CompositionSession::compositionChanged, this,
            &BackgroundPreviewController::restart);
    connect(&session_, &CompositionSession::currentTimeChanged, this, [this] {
        if (!playing_) {
            restart();
        } else {
            // A useful in-flight frame remains useful as the clock advances. Only the next ask
            // moves ahead of the new playhead; cancelling every tick would prevent slow fills.
            cursor_ = 0;
            considered_ = 0;
            exhausted_ = false;
        }
    });
    connect(&previewController_.frameCache(), &PreviewFrameCache::byteBudgetChanged, this,
            &BackgroundPreviewController::restart);
    idleTimer_.start();
}

BackgroundPreviewController::~BackgroundPreviewController() { beginShutdown(); }

bool BackgroundPreviewController::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::MouseButtonPress ||
        event->type() == QEvent::NonClientAreaMouseButtonPress) {
        // Node/layer drags may not change preview state until release. Yield at press, before
        // their first movement, and keep the ordinary mouse-button guard on subsequent fills.
        restart();
    }
    return QObject::eventFilter(watched, event);
}

void BackgroundPreviewController::cancelActive() {
    if (active_.has_value()) {
        active_->cancel();
        discardActive_ = true;
    }
}

void BackgroundPreviewController::restart() {
    cancelActive();
    cursor_ = 0;
    considered_ = 0;
    exhausted_ = false;
}

void BackgroundPreviewController::setPlaying(const bool playing) {
    if (playing_ != playing) {
        playing_ = playing;
        restart();
    }
}

void BackgroundPreviewController::beginShutdown() {
    shuttingDown_ = true;
    idleTimer_.stop();
    cancelActive();
}

void BackgroundPreviewController::fillNextFrame() {
    if (shuttingDown_ || previewController_.isShuttingDown()) {
        beginShutdown();
        return;
    }
    if (!previewController_.backgroundWorkAllowed() ||
        QApplication::mouseButtons() != Qt::NoButton) {
        restart();
        return;
    }
    if (active_.has_value() || exhausted_ || !preparation_) {
        return;
    }
    const auto* composition = session_.composition();
    if (composition == nullptr) {
        return;
    }
    const auto format = composition->format();
    const auto rate = format.frameRate();
    const auto mapping = core::FrameTimeMapping::create(composition->duration(), rate.numerator(),
                                                        rate.denominator());
    if (!mapping.hasValue()) {
        return;
    }
    const auto divisor = previewController_.resolutionDivisor();
    const auto bytes = std::uint64_t{4} * ((format.width() + divisor - 1) / divisor) *
                       ((format.height() + divisor - 1) / divisor);
    auto& cache = previewController_.frameCache();
    const auto capacity = bytes == 0 ? 0 : cache.byteBudget() / bytes;
    const auto range = session_.workArea();
    const auto first = mapping.value()->nearestFrameIndex(range.start);
    const auto endMapping =
        core::FrameTimeMapping::create(range.end, rate.numerator(), rate.denominator());
    if (!endMapping)
        return;
    const auto maximum = endMapping.value()->maximumFrameIndex() - first;
    const auto anchor = std::clamp(mapping.value()->nearestFrameIndex(session_.currentTime()),
                                   first, first + maximum) -
                        first;
    // Each pass visits only the nearest budget-sized set. It never cycles around evicting its
    // own useful frames. Bound each UI turn too, even for enormous compositions/sparse ranges.
    for (int inspected = 0; inspected < 256; ++inspected) {
        if (considered_ >= std::min(capacity, maximum + 1) ||
            cursor_ == std::numeric_limits<std::uint64_t>::max()) {
            exhausted_ = true;
            return;
        }
        const auto ordinal = cursor_++;
        std::uint64_t index = anchor;
        if (playing_) {
            const auto distance = ordinal % (maximum + 1);
            index =
                distance > maximum - anchor ? distance - (maximum - anchor) - 1 : anchor + distance;
        } else if (ordinal != 0) {
            const auto distance = ordinal / 2 + ordinal % 2;
            if (ordinal % 2 != 0) {
                if (distance > maximum - anchor) {
                    continue;
                }
                index += distance;
            } else {
                if (distance > anchor) {
                    continue;
                }
                index -= distance;
            }
        }
        ++considered_;
        const auto time = mapping.value()->timeForFrame(first + index);
        if (!time.hasValue() || generation_ == std::numeric_limits<std::uint64_t>::max()) {
            exhausted_ = true;
            return;
        }
        const auto key = previewController_.cacheKeyForTime(*time.value());
        if (!key.has_value()) {
            return;
        }
        const runtime::PreviewRequestIdentity identity{
            .projectId = key->projectId,
            .compositionId = key->compositionId,
            .sourceRevision = key->sourceRevision,
            .requestGeneration = ++generation_,
            .time = key->time,
            .output = key->output,
            .resolution = key->resolution,
            .quality = key->quality,
            .colorIntent = key->colorIntent,
            .resolutionPolicy = key->resolutionPolicy,
        };
        if (cache.contains(*key)) {
            // Protect nearer existing entries from eviction by this pass's farther entries.
            (void)cache.take(identity);
            continue;
        }
        runtime::TaskRequest request(
            "Cache background preview frame",
            {.kind = runtime::TaskOwnerKind::Composition,
             .id = runtime::TaskOwnerId::fromRaw(key->compositionId.value())},
            runtime::TaskPriority::Background);
        request.sourceVersion = {.documentRevision = key->sourceRevision.value(),
                                 .requestGeneration = identity.requestGeneration};
        submittedAt_ = std::chrono::steady_clock::now();
        auto submission = scheduler_.submit<PreviewPreparationResultHandle>(
            std::move(request),
            [snapshot = session_.snapshot(), identity, preparation = preparation_,
             limit = previewController_.settings().pixelStorageByteLimit](
                runtime::TaskContext& context) mutable {
                if (context.isCancellationRequested()) {
                    return runtime::TaskResult<PreviewPreparationResultHandle>::cancelled();
                }
                return preparation(snapshot, identity, limit, {}, context);
            });
        if (!submission.accepted()) {
            --cursor_;
            --considered_;
            return;
        }
        discardActive_ = false;
        activeIdentity_ = identity;
        active_.emplace(std::move(submission.handle));
        bridge_.wake();
        return;
    }
}

void BackgroundPreviewController::consumeReadyResult() {
    if (!active_.has_value()) {
        return;
    }
    auto result = active_->tryTakeResult();
    if (!result.has_value()) {
        return;
    }
    active_.reset();
    if (discardActive_ || shuttingDown_ || previewController_.isShuttingDown() ||
        !activeIdentity_.has_value()) {
        return;
    }
    const auto key = previewController_.cacheKeyForTime(activeIdentity_->time);
    if (!key.has_value() || *key != PreviewFrameCacheKey::forIdentity(*activeIdentity_)) {
        return;
    }
    const auto& value = result->value();
    if (result->state() != runtime::TaskState::Succeeded || !value.has_value() ||
        *value == nullptr || (*value)->frame() == nullptr ||
        (*value)->frame()->desiredIdentity() != *activeIdentity_) {
        // Unsupported/failed work is not retried indefinitely while the app is idle. Its task
        // retains the pipeline's diagnostics; the next revision/resolution/playhead restarts.
        exhausted_ = true;
        return;
    }
    previewController_.recordPreparationDuration(*activeIdentity_,
                                                 std::chrono::steady_clock::now() - submittedAt_);
    previewController_.frameCache().insert((*value)->frame());
    fillNextFrame();
}

} // namespace bloom::ui
