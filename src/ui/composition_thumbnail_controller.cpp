#include <bloom/ui/composition_thumbnail_controller.hpp>

#include <bloom/document/project.hpp>
#include <bloom/render/image_types.hpp>
#include <bloom/runtime/prepared_preview_frame.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

#include <algorithm>
#include <limits>
#include <utility>

namespace bloom::ui {
namespace {
// The smallest whole divisor that fits the composition inside the thumbnail's longest edge, so the
// proxy extent is the same rounding the Viewer's own reduced resolutions use.
[[nodiscard]] std::optional<render::ImageExtent>
thumbnailExtent(const document::CompositionFormat& format) {
    const auto longest = std::max(format.width(), format.height());
    const auto edge = static_cast<std::uint32_t>(CompositionThumbnailController::kMaximumEdge);
    const auto divisor = std::max<std::uint32_t>(1, (longest + edge - 1) / edge);
    const auto extent = render::ImageExtent::create((format.width() + divisor - 1) / divisor,
                                                    (format.height() + divisor - 1) / divisor);
    return extent ? std::optional(*extent.value()) : std::nullopt;
}
} // namespace

CompositionThumbnailController::CompositionThumbnailController(
    CompositionSession& session, runtime::TaskScheduler& scheduler, TaskUiBridge& bridge,
    PreviewPreparationFunction preparation, const std::size_t byteLimit, QObject* parent)
    : QObject(parent), session_(session), scheduler_(scheduler), bridge_(bridge),
      preparation_(std::move(preparation)), byteLimit_(byteLimit) {
    connect(&bridge_, &TaskUiBridge::resultsAvailable, this, &CompositionThumbnailController::poll);
    session_.setCompositionThumbnails(this);
}

CompositionThumbnailController::~CompositionThumbnailController() {
    cancel();
    session_.setCompositionThumbnails(nullptr);
}

void CompositionThumbnailController::cancel() {
    for (auto& [composition, pending] : pending_)
        pending.handle.cancel();
    pending_.clear();
}

QImage CompositionThumbnailController::thumbnail(const document::CompositionId composition) {
    const auto& snapshot = session_.snapshot();
    if (snapshot.project().id() != project_) {
        // Another project: nothing kept belongs to it.
        cancel();
        entries_.clear();
        attempted_.clear();
        project_ = snapshot.project().id();
    }
    if (snapshot.project().findComposition(composition) == nullptr)
        return {};
    if (!current(composition))
        request(composition);
    const auto found = entries_.find(composition);
    if (found == entries_.end())
        return {};
    found->second.used = ++uses_;
    return found->second.image;
}

bool CompositionThumbnailController::current(const document::CompositionId composition) const {
    const auto found = entries_.find(composition);
    return found != entries_.end() && found->second.revision == session_.snapshot().revision();
}

void CompositionThumbnailController::request(const document::CompositionId composition) {
    const auto snapshot = session_.snapshot();
    const auto revision = snapshot.revision();
    // One attempt per revision: a frame that cannot be prepared is not retried until an edit.
    if (const auto attempted = attempted_.find(composition);
        attempted != attempted_.end() && attempted->second == revision)
        return;
    if (const auto pending = pending_.find(composition); pending != pending_.end()) {
        pending->second.handle.cancel();
        pending_.erase(pending);
    }
    const auto* record = snapshot.project().findComposition(composition);
    const auto extent = record ? thumbnailExtent(record->format()) : std::nullopt;
    // The worker keeps its own copy of the colour intent's strings.
    const auto colorIntent = runtime::captureEvaluationColorIntent(session_.colorIntent());
    if (!preparation_ || !extent || !colorIntent || bridge_.isShuttingDown())
        return;
    // The composition's start, at a proxy extent, through the same preparation the Viewer's
    // analysis uses; the display transform is the session's default view.
    runtime::PreviewRequestIdentity identity;
    identity.projectId = snapshot.project().id();
    identity.compositionId = composition;
    identity.sourceRevision = revision;
    identity.requestGeneration = submissions_ + 1;
    identity.time = core::RationalTime{};
    identity.resolution = runtime::ProxyResolution{*extent};
    identity.colorIntent = *colorIntent;
    runtime::TaskRequest request("Prepare composition thumbnail",
                                 {.kind = runtime::TaskOwnerKind::PanelRequest,
                                  .id = runtime::TaskOwnerId::fromRaw(composition.value())},
                                 runtime::TaskPriority::Background);
    request.sourceVersion = {.documentRevision = revision.value(),
                             .requestGeneration = identity.requestGeneration};
    auto submission = scheduler_.submit<std::shared_ptr<const Prepared>>(
        std::move(request), [snapshot, identity, preparation = preparation_,
                             limit = byteLimit_](runtime::TaskContext& context) {
            using Result = runtime::TaskResult<std::shared_ptr<const Prepared>>;
            if (context.isCancellationRequested())
                return Result::cancelled();
            auto prepared = preparation(snapshot, identity, limit, {}, context);
            if (context.isCancellationRequested())
                return Result::cancelled();
            const auto& value = prepared.value();
            const auto* frame = prepared.state() == runtime::TaskState::Succeeded && value && *value
                                    ? (*value)->frame().get()
                                    : nullptr;
            const auto view = frame ? frame->displayBufferView() : std::nullopt;
            if (!view) {
                runtime::TaskDiagnostic diagnostic;
                diagnostic.code = "bloom.ui.composition-thumbnail.unavailable";
                diagnostic.severity = runtime::DiagnosticSeverity::Warning;
                diagnostic.summary = "The composition frame could not be prepared";
                return Result::failed(std::move(diagnostic));
            }
            const auto window = view->dataWindow.value_or(view->displayWindow).extent();
            if (window.width() > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
                window.height() > static_cast<std::uint32_t>(std::numeric_limits<int>::max()))
                return Result::cancelled();
            // Copied out of the frame here, on the worker, so the interface thread only paints.
            const QImage borrowed(
                reinterpret_cast<const uchar*>(view->pixels.data()),
                static_cast<int>(window.width()), static_cast<int>(window.height()),
                static_cast<qsizetype>(view->layout.rowStrideBytes), QImage::Format_RGBA8888);
            auto image = borrowed.copy();
            if (std::max(image.width(), image.height()) > kMaximumEdge)
                image = image.scaled(kMaximumEdge, kMaximumEdge, Qt::KeepAspectRatio,
                                     Qt::SmoothTransformation);
            return Result::succeeded(std::make_shared<const Prepared>(
                Prepared{std::move(image), std::this_thread::get_id()}));
        });
    if (!submission.accepted())
        return;
    ++submissions_;
    attempted_[composition] = revision;
    pending_.emplace(composition, Pending{revision, std::move(submission.handle)});
    bridge_.wake();
}

void CompositionThumbnailController::poll() {
    bool published = false;
    for (auto pending = pending_.begin(); pending != pending_.end();) {
        auto result = pending->second.handle.tryTakeResult();
        if (!result) {
            ++pending;
            continue;
        }
        const auto composition = pending->first;
        const auto revision = pending->second.revision;
        pending = pending_.erase(pending);
        const auto& value = result->value();
        if (result->state() != runtime::TaskState::Succeeded || !value || !*value)
            continue;
        lastWorkerThread_ = (*value)->thread;
        entries_[composition] = Entry{revision, (*value)->image, ++uses_};
        published = true;
    }
    while (entries_.size() > kCapacity)
        entries_.erase(std::ranges::min_element(
            entries_, {}, [](const auto& entry) { return entry.second.used; }));
    if (published)
        emit changed();
}

} // namespace bloom::ui
