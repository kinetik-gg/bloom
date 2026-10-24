#include <bloom/ui/cryptomatte_controller.hpp>

#include <bloom/document/channel_nodes.hpp>
#include <bloom/runtime/cryptomatte_matte_list.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

#include <QTimer>

#include <algorithm>
#include <utility>

namespace bloom::ui {
namespace {

[[nodiscard]] runtime::TaskRequest workerRequest(const char* name,
                                                 const document::CompositionId composition) {
    return runtime::TaskRequest(name,
                                {.kind = runtime::TaskOwnerKind::PanelRequest,
                                 .id = runtime::TaskOwnerId::fromRaw(composition.value())},
                                runtime::TaskPriority::Visible);
}

} // namespace

CryptomatteController::CryptomatteController(CompositionSession& session,
                                             runtime::TaskScheduler& scheduler,
                                             TaskUiBridge& bridge, LayerProvider layers,
                                             PickProvider pick, QObject* parent)
    : QObject(parent), session_(session), scheduler_(scheduler), bridge_(bridge),
      layerProvider_(std::move(layers)), pickProvider_(std::move(pick)) {
    connect(&bridge_, &TaskUiBridge::resultsAvailable, this, &CryptomatteController::poll);
    session_.setCryptomatteController(this);
}

CryptomatteController::~CryptomatteController() {
    cancel();
    session_.setCryptomatteController(nullptr);
}

void CryptomatteController::cancel() {
    for (auto& [node, task] : listingTasks_)
        task.handle.cancel();
    listingTasks_.clear();
    if (pickTask_) {
        pickTask_->handle.cancel();
        pickTask_.reset();
    }
}

void CryptomatteController::requestLayers(const document::NodeId node) {
    const auto revision = session_.snapshot().revision();
    if (const auto known = listings_.find(node);
        known != listings_.end() && known->second.revision == revision)
        return;
    if (const auto pending = listingTasks_.find(node); pending != listingTasks_.end()) {
        if (pending->second.revision == revision)
            return;
        pending->second.handle.cancel();
        listingTasks_.erase(pending);
    }
    if (!layerProvider_ || bridge_.isShuttingDown())
        return;
    const auto composition = session_.compositionId();
    auto submission = scheduler_.submit<std::shared_ptr<const runtime::CryptomatteLayerListing>>(
        workerRequest("List Cryptomatte layers", composition),
        [provider = layerProvider_, snapshot = session_.snapshot(), composition, node,
         time = session_.currentTime(),
         colorIntent = session_.colorIntent()](runtime::TaskContext& context) {
            using Result =
                runtime::TaskResult<std::shared_ptr<const runtime::CryptomatteLayerListing>>;
            if (context.isCancellationRequested())
                return Result::cancelled();
            auto listing =
                provider(snapshot, composition, node, time, colorIntent, context.cancellation());
            if (context.isCancellationRequested())
                return Result::cancelled();
            return Result::succeeded(
                std::make_shared<const runtime::CryptomatteLayerListing>(std::move(listing)));
        });
    if (submission.accepted()) {
        listingTasks_.emplace(node, PendingListing{revision, std::move(submission.handle)});
        bridge_.wake();
    }
}

// The latest listing for the node, kept while a newer revision is re-read so a matte edit does
// not blank the layer picker or the name list (requestLayers() starts the re-read).
const std::vector<std::string>* CryptomatteController::layers(const document::NodeId node) const {
    const auto found = listings_.find(node);
    return found != listings_.end() ? &found->second.value.layers : nullptr;
}

QString CryptomatteController::layerDiagnostic(const document::NodeId node) const {
    const auto found = listings_.find(node);
    return found != listings_.end() ? QString::fromStdString(found->second.value.diagnostic)
                                    : QString{};
}

const runtime::CryptomatteLayerListing*
CryptomatteController::listing(const document::NodeId node) const {
    const auto found = listings_.find(node);
    return found != listings_.end() ? &found->second.value : nullptr;
}

bool CryptomatteController::listingCurrent(const document::NodeId node) const {
    const auto found = listings_.find(node);
    return found != listings_.end() && found->second.revision == session_.snapshot().revision();
}

bool CryptomatteController::preview(const document::NodeId node) const {
    return !previewOff_.contains(node);
}

void CryptomatteController::setPreview(const document::NodeId node, const bool on) {
    if (preview(node) == on)
        return;
    if (on)
        previewOff_.erase(node);
    else
        previewOff_.insert(node);
    emit changed();
}

void CryptomatteController::requestPick() { emit pickRequested(); }

std::vector<std::string> CryptomatteController::mattes(const document::NodeId node) const {
    const auto* composition = session_.composition();
    const auto* record = composition != nullptr ? composition->graph().findNode(node) : nullptr;
    if (record == nullptr)
        return {};
    for (const auto& binding : record->parameters)
        if (binding.role == document::kCryptomatteMattesParameterRole)
            return runtime::splitCryptomatteMatteList(
                session_.constantStringValue(binding.parameterId)
                    .value_or(QString{})
                    .toStdString());
    return {};
}

bool CryptomatteController::setMattes(const document::NodeId node,
                                      const std::vector<std::string>& mattes,
                                      const QString& label) {
    if (mattes == this->mattes(node))
        return true;
    return session_.setNodeParameterValue(
        node, std::string(document::kCryptomatteMattesParameterRole),
        document::ParameterValue{runtime::joinCryptomatteMatteList(mattes)}, label);
}

void CryptomatteController::reportPick(QString message) {
    if (pickTask_) {
        pickTask_->handle.cancel();
        pickTask_.reset();
    }
    lastPickMessage_ = std::move(message);
    emit changed();
}

void CryptomatteController::pick(const document::NodeId node, const std::int64_t x,
                                 const std::int64_t y, const PickAction action,
                                 std::optional<runtime::ChannelInspection> inspection) {
    if (!pickProvider_ || bridge_.isShuttingDown())
        return;
    if (pickTask_) {
        pickTask_->handle.cancel();
        pickTask_.reset();
    }
    const auto composition = session_.compositionId();
    auto submission = scheduler_.submit<std::shared_ptr<const runtime::CryptomattePick>>(
        workerRequest("Pick Cryptomatte", composition),
        [provider = pickProvider_, snapshot = session_.snapshot(), composition, node,
         time = session_.currentTime(), colorIntent = session_.colorIntent(),
         inspection = std::move(inspection), x, y](runtime::TaskContext& context) {
            using Result = runtime::TaskResult<std::shared_ptr<const runtime::CryptomattePick>>;
            if (context.isCancellationRequested())
                return Result::cancelled();
            auto pick = provider(snapshot, composition, node, time, colorIntent, inspection, x,
                                 y, context.cancellation());
            if (context.isCancellationRequested())
                return Result::cancelled();
            return Result::succeeded(
                std::make_shared<const runtime::CryptomattePick>(std::move(pick)));
        });
    if (submission.accepted()) {
        pickTask_ = PendingPick{node, action, std::move(submission.handle)};
        lastPickMessage_ = tr("Reading the Cryptomatte…");
        emit changed();
        bridge_.wake();
    }
}

void CryptomatteController::poll() {
    bool any = false;
    for (auto task = listingTasks_.begin(); task != listingTasks_.end();) {
        auto result = task->second.handle.tryTakeResult();
        if (!result) {
            ++task;
            continue;
        }
        if (result->state() == runtime::TaskState::Succeeded && result->value() && *result->value())
            listings_.insert_or_assign(task->first,
                                       Listing{task->second.revision, **result->value()});
        task = listingTasks_.erase(task);
        any = true;
    }
    if (pickTask_) {
        if (auto result = pickTask_->handle.tryTakeResult()) {
            const auto node = pickTask_->node;
            const auto action = pickTask_->action;
            pickTask_.reset();
            any = true;
            if (result->state() != runtime::TaskState::Succeeded || !result->value() ||
                !*result->value()) {
                lastPickMessage_ = tr("The Cryptomatte could not be read");
            } else if (const auto& pick = **result->value(); !pick.diagnostic.empty()) {
                lastPickMessage_ = QString::fromStdString(pick.diagnostic);
            } else if (!pick.matte) {
                lastPickMessage_ = tr("No object under the cursor");
            } else {
                auto list = mattes(node);
                const auto found = std::ranges::find(list, *pick.matte);
                const auto name = QString::fromStdString(*pick.matte);
                if (action == PickAction::Add && found == list.end()) {
                    list.push_back(*pick.matte);
                    lastPickMessage_ = setMattes(node, list, tr("Add Cryptomatte Matte"))
                                           ? tr("Added %1").arg(name)
                                           : tr("The matte list could not be edited");
                } else if (action == PickAction::Remove && found != list.end()) {
                    list.erase(found);
                    lastPickMessage_ = setMattes(node, list, tr("Remove Cryptomatte Matte"))
                                           ? tr("Removed %1").arg(name)
                                           : tr("The matte list could not be edited");
                } else {
                    lastPickMessage_ = action == PickAction::Add
                                           ? tr("%1 is already selected").arg(name)
                                           : tr("%1 is not selected").arg(name);
                }
            }
        }
    }
    // A worker that finished between the bridge wake and this poll is collected on the next one.
    if (!listingTasks_.empty() || pickTask_)
        QTimer::singleShot(20, &bridge_, &TaskUiBridge::wake);
    if (any)
        emit changed();
}

} // namespace bloom::ui
