#include <bloom/ui/composition_session.hpp>

#include "animation_components.hpp"

#include <bloom/color/bloom_neutral_builtin.hpp>
#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/commands/animation_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/result.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/core/utf8.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/project.hpp>
#include <bloom/document/value_utility_nodes.hpp>
#include <bloom/runtime/animation_sampling.hpp>
#include <bloom/runtime/curve_compilation.hpp>

#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <atomic>
#include <bloom/runtime/qualified_display_processor_provider.hpp>
#include <mutex>

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace bloom::ui {
namespace {
double* editedComponent(document::ParameterValue& value, document::AnimationComponent component);
}

struct SessionColorConverterState final {
    std::mutex mutex;
    std::shared_ptr<const color::PreparedCpuDisplayProcessorHandle> processor;
    std::atomic<bool> cancelled{false};
    bool finished = false;
};

[[nodiscard]] std::shared_ptr<const color::PreparedCpuDisplayProcessorHandle>
buildColorProcessor(const document::ColorSettings& settings) {
    const auto* builtIn =
        std::get_if<document::BuiltInOcioConfigLocator>(&settings.ocioConfig.locator);
    if (builtIn == nullptr ||
        settings.ocioConfig.expectedRevision.algorithm != document::OcioRevisionAlgorithm::Sha256) {
        return {};
    }
    auto resolution = color::resolveOcioBuiltIn(
        color::OcioConfigLocatorKind::BloomBuiltIn, builtIn->uri,
        settings.ocioConfig.expectedRevision.digest, settings.processColorSpaceId);
    if (!resolution.ready()) {
        return {};
    }
    auto resolved = std::move(resolution).takeResolved();
    if (!resolved.has_value()) {
        return {};
    }
    auto built = color::buildBloomNeutralCpuDisplayProcessor(*resolved);
    auto handle = std::move(built).takeHandle();
    if (!handle.has_value()) {
        return {};
    }
    return std::make_shared<const color::PreparedCpuDisplayProcessorHandle>(std::move(*handle));
}

kit::KColorConverter CompositionSession::colorConverter(const std::string_view schemaKey) {
    static_assert(document::kSolidColorEncoding == "bloom.reference.linear-srgb");
    // Every Color4d-valued schema -- including the shape source's fillColor/strokeColor and any
    // future colour parameter added to isColor4AnimatableSchemaKey() -- authors straight RGBA in
    // this one reference-linear-sRGB encoding, plus the constant-or-driven colour operand. A schema
    // key outside that set is genuinely unknown and fails closed, exactly as before.
    if (!document::isColor4AnimatableSchemaKey(schemaKey) &&
        schemaKey != document::kColorOperandParameterSchemaKey)
        return {};
    if (!colorConverterState_) {
        colorConverterState_ = std::make_shared<SessionColorConverterState>();
        const auto state = colorConverterState_;
        auto settings = colorSettings_;
        if (const auto* currentComposition = composition();
            currentComposition != nullptr &&
            currentComposition->workingColorSpaceId().has_value()) {
            settings.processColorSpaceId = *currentComposition->workingColorSpaceId();
        }
        connect(this, &QObject::destroyed, [state] { state->cancelled.store(true); });
        // Only the immutable, bounded built-in runs here. No widget/session pointer crosses
        // into the worker. Destruction cancels publication and never waits on the UI thread.
        QThreadPool::globalInstance()->start([state, settings] {
            if (state->cancelled.load())
                return;
            std::shared_ptr<const color::PreparedCpuDisplayProcessorHandle> result;
            try {
                result = buildColorProcessor(settings);
            } catch (...) {
                result.reset();
            }
            const std::lock_guard lock(state->mutex);
            if (!state->cancelled.load()) {
                state->processor = result;
                state->finished = true;
            }
        });
        auto* timer = new QTimer(this);
        timer->setInterval(16);
        connect(timer, &QTimer::timeout, this, [this, state, timer] {
            bool finished = false;
            bool ready = false;
            {
                const std::lock_guard lock(state->mutex);
                finished = state->finished;
                ready = state->processor != nullptr;
            }
            if (!finished)
                return;
            timer->stop();
            timer->deleteLater();
            if (!ready)
                reportUnavailable(tr("Colour conversion unavailable"));
            Q_EMIT snapshotChanged();
            // The qualified display transform landing changes pixels without changing the document
            // revision, so it is an evaluation transition rather than a live-document edit.
            Q_EMIT evaluationChanged();
        });
        timer->start();
    }
    std::shared_ptr<const color::PreparedCpuDisplayProcessorHandle> processor;
    {
        const std::lock_guard lock(colorConverterState_->mutex);
        processor = colorConverterState_->processor;
    }
    if (!processor)
        return {};
    return [processor](const kit::KColor& input,
                       const kit::ColorSpace target) -> std::optional<kit::KColor> {
        const core::Color4d value{static_cast<double>(input.red), static_cast<double>(input.green),
                                  static_cast<double>(input.blue),
                                  static_cast<double>(input.alpha)};
        const auto converted = target == kit::ColorSpace::Display
                                   ? processor->referenceToDisplay(value)
                                   : processor->displayToReference(value);
        if (!converted)
            return std::nullopt;
        return kit::KColor::fromRgba(static_cast<float>(converted->red),
                                     static_cast<float>(converted->green),
                                     static_cast<float>(converted->blue), input.alpha, target);
    };
}

runtime::EvaluationColorIntent CompositionSession::colorIntent() const noexcept {
    const auto* currentComposition = composition();
    const auto workingColorSpaceId =
        currentComposition != nullptr && currentComposition->workingColorSpaceId().has_value()
            ? std::string_view{*currentComposition->workingColorSpaceId()}
            : std::string_view{colorSettings_.processColorSpaceId};
    std::string_view ocioConfigUri;
    if (const auto* builtIn =
            std::get_if<document::BuiltInOcioConfigLocator>(&colorSettings_.ocioConfig.locator))
        ocioConfigUri = builtIn->uri;
    else if (const auto* externalArchive =
                 std::get_if<document::ExternalOciozLocator>(&colorSettings_.ocioConfig.locator))
        ocioConfigUri = externalArchive->uri;
    else if (const auto* externalConfig = std::get_if<document::ExternalOcioConfigLocator>(
                 &colorSettings_.ocioConfig.locator))
        ocioConfigUri = externalConfig->uri;
    return {.workingColorSpaceId = workingColorSpaceId,
            .ocioConfigRevision = colorSettings_.ocioConfig.expectedRevision.digest,
            .ocioConfigUri = ocioConfigUri};
}

void CompositionSession::setColorSettings(document::ColorSettings settings) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (colorSettings_ == settings) {
        return;
    }
    if (colorConverterState_ != nullptr) {
        colorConverterState_->cancelled.store(true);
    }
    colorConverterState_.reset();
    colorSettings_ = std::move(settings);
    emit colorSettingsChanged();
    emit snapshotChanged();
    emit evaluationChanged();
}

bool CompositionSession::setWorkingColorSpaceOverride(std::optional<std::string> colorSpaceId) {
    const auto* currentComposition = composition();
    if (currentComposition == nullptr) {
        reportUnavailable(QStringLiteral("No composition is available"));
        return false;
    }
    if (colorSpaceId.has_value()) {
        if (!document::validateWorkingColorSpaceId(*colorSpaceId).ok()) {
            reportUnavailable(QStringLiteral("Invalid working colour space id"));
            return false;
        }
        const auto* builtIn =
            std::get_if<document::BuiltInOcioConfigLocator>(&colorSettings_.ocioConfig.locator);
        if (builtIn == nullptr || colorSettings_.ocioConfig.expectedRevision.algorithm !=
                                      document::OcioRevisionAlgorithm::Sha256) {
            reportUnavailable(QStringLiteral("The selected OCIO configuration needs its colour "
                                             "helper before this override can be used"));
            return false;
        }
        const auto resolution = color::resolveOcioBuiltIn(
            color::OcioConfigLocatorKind::BloomBuiltIn, builtIn->uri,
            colorSettings_.ocioConfig.expectedRevision.digest, *colorSpaceId);
        if (!resolution.ready()) {
            reportUnavailable(QStringLiteral("The selected colour space is not a scene-linear "
                                             "space in this configuration"));
            return false;
        }
    }
    commands::Transaction transaction("Set Composition Working Space", snapshot_.revision());
    transaction.emplace<commands::SetCompositionWorkingColorSpace>(compositionId_,
                                                                   std::move(colorSpaceId));
    const auto result = executeTransaction(std::move(transaction));
    return result.status == commands::CommandStatus::Succeeded ||
           result.status == commands::CommandStatus::NoChange;
}

// The ParameterSample alternatives a CURVE can hold. sampleParameterValue() returns only these
// four, because only these four have a midpoint; the three task DRIVE-1 added to ParameterSample --
// a String, an Integer, a Boolean -- are values a row shows, never values a keyframe command
// receives, and no keyframe command has an overload for one.
template <typename Value>
inline constexpr bool interpolable =
    std::is_same_v<Value, double> || std::is_same_v<Value, document::Vec2d> ||
    std::is_same_v<Value, document::Vec3d> || std::is_same_v<Value, core::Color4d>;

document::WorkArea CompositionSession::workArea() const noexcept {
    const auto* current = composition();
    return current ? current->workArea().value_or(document::WorkArea{{}, current->duration()})
                   : document::WorkArea{};
}

namespace {

QString statusMessage(const commands::CommandResult& result) {
    if (!result.operationFailures.empty()) {
        return QString::fromStdString(result.operationFailures.front().issue.message);
    }
    if (!result.validation.ok()) {
        return QString::fromStdString(result.validation.issues().front().message);
    }

    switch (result.status) {
    case commands::CommandStatus::Succeeded:
    case commands::CommandStatus::NoChange:
        return {};
    case commands::CommandStatus::Rejected:
        return QStringLiteral("The edit was rejected");
    case commands::CommandStatus::ValidationFailed:
        return QStringLiteral("The edit would make the composition invalid");
    case commands::CommandStatus::StaleRevision:
        return QStringLiteral("The composition changed before the edit could be applied");
    case commands::CommandStatus::ForeignDocument:
        return QStringLiteral("The command belongs to a different document");
    case commands::CommandStatus::DraftBaseMismatch:
        return QStringLiteral("The command draft no longer matches its document snapshot");
    case commands::CommandStatus::RevisionOverflow:
        return QStringLiteral("The document revision limit was reached");
    case commands::CommandStatus::NothingToUndo:
        return QStringLiteral("There is nothing to undo");
    case commands::CommandStatus::NothingToRedo:
        return QStringLiteral("There is nothing to redo");
    }
    return QStringLiteral("The edit could not be applied");
}

[[nodiscard]] document::Vec2d compositionCenter(const document::Composition& composition) {
    const auto format = composition.format();
    return {static_cast<double>(format.width()) * 0.5, static_cast<double>(format.height()) * 0.5};
}

// The contract's "lowest valid CompositionId" (docs/architecture/project-session.md, "Session
// Publication", item 5), mirroring ProjectHost::lowestCompositionId()'s own min-scan (issue #75:
// this used to be `compositions().front().id()`, which is insertion order, not id order -- a
// document whose compositions were inserted out of id order picked the wrong fallback
// composition). Precondition: `project.compositions()` is non-empty; callers below only invoke
// this after checking that.
// The two honest undo labels one diamond click can produce, named from the parameter's own SCHEMA
// KEY rather than from a generic "Keyframe", so a history entry says which parameter the artist
// clicked. The schema key is the parameter's identity (a role is node-local and two different
// schemas deliberately share the role "color" -- see document::kTextColorParameterRole), so the
// label is derived from its last dotted segment: "bloom.transform.position" reads "Position",
// "bloom.text.size" reads "Size".
struct KeyframeCommandLabels final {
    QString addKey;
    QString removeKey;
};

[[nodiscard]] KeyframeCommandLabels keyframeCommandLabel(const std::string_view schemaKey) {
    const auto lastDot = schemaKey.rfind('.');
    const auto leaf = lastDot == std::string_view::npos ? schemaKey : schemaKey.substr(lastDot + 1);
    QString display = QString::fromUtf8(leaf.data(), static_cast<qsizetype>(leaf.size()));
    display.replace(QLatin1Char('-'), QLatin1Char(' '));
    if (!display.isEmpty()) {
        display[0] = display[0].toUpper();
    }
    return {QStringLiteral("Add %1 Keyframe").arg(display),
            QStringLiteral("Remove %1 Keyframe").arg(display)};
}

[[nodiscard]] document::CompositionId lowestCompositionId(const document::Project& project) {
    const auto compositions = project.compositions();
    auto lowest = compositions.front().id();
    for (const auto& candidate : compositions) {
        if (candidate.id().value() < lowest.value()) {
            lowest = candidate.id();
        }
    }
    return lowest;
}

} // namespace

struct CompositionSession::CommandObserverState final {
    CompositionSession* owner = nullptr;
};

CompositionSession::CompositionSession(document::Document& document,
                                       commands::CommandStack& commandStack,
                                       document::CompositionId compositionId, QObject* parent)
    : QObject(parent),
      colorSettings_(document::makeBloomNeutralColorSettingsV1(color::kBloomNeutralV1ConfigDigest)),
      document_(&document), commandStack_(&commandStack), snapshot_(document.snapshot()),
      evaluationSnapshot_(snapshot_), compositionId_(compositionId) {
    attachCommandObserver();
    if (composition() == nullptr && !snapshot_.project().compositions().empty()) {
        compositionId_ = lowestCompositionId(snapshot_.project());
    }
}

CompositionSession::~CompositionSession() {
    if (commandObserverState_ != nullptr) {
        commandObserverState_->owner = nullptr;
        commandObserverState_.reset();
    }
}

void CompositionSession::attachCommandObserver() {
    auto state = std::make_shared<CommandObserverState>();
    state->owner = this;
    commandObserverState_ = state;
    commandObserverId_ =
        commandStack_->addObserver([weakState = std::weak_ptr<CommandObserverState>(state)](
                                       const commands::CommandEvent& event) {
            if (const auto locked = weakState.lock(); locked != nullptr && locked->owner != nullptr)
                locked->owner->handleCommandEvent(event);
        });
}

void CompositionSession::rebind(document::Document& document, commands::CommandStack& commandStack,
                                const document::CompositionId compositionId) {
    Q_ASSERT(QThread::currentThread() == thread());
    // ProjectHost replaces the live ProjectSession before emitting sessionReplaced(), so the old
    // stack may already have been destroyed by the time this adapter is rebound. Invalidate the
    // callback unconditionally; remove it only when the stack identity is known to be unchanged.
    if (commandStack_ == &commandStack && commandStack_ != nullptr && commandObserverId_ != 0) {
        commandStack_->removeObserver(commandObserverId_);
    }
    if (commandObserverState_ != nullptr) {
        commandObserverState_->owner = nullptr;
        commandObserverState_.reset();
    }
    document_ = &document;
    commandStack_ = &commandStack;
    commandObserverId_ = 0;
    attachCommandObserver();
    snapshot_ = document_->snapshot();
    // Reset the retained evaluation snapshot to the newly bound document BEFORE any signal is
    // published, so no observer can ever pair a new document with the old document's eval state.
    evaluationSnapshot_ = snapshot_;
    compositionId_ = compositionId;
    currentTime_ = core::RationalTime::fromInteger(0);
    selection_ = {};
    keyframeClipboard_.clear();
    selectedNodes_.clear();
    // The OLD document/command-stack are left untouched, but this session's own interaction state
    // targets them and must not survive the swap.
    transformInteraction_.reset();
    valueEdit_.reset();

    // One coherent transition (docs/architecture/project-session.md, "Session Publication"):
    // observers must never see a new document paired with stale selection/time/history, so every
    // existing changed signal fires here, unconditionally, in this order. documentRebound() is
    // first, so caches keyed by colliding numeric ids are dropped before any refresh is built.
    emit documentRebound();
    emit liveValueChanged();
    emit snapshotChanged();
    emit evaluationChanged();
    emit compositionChanged();
    emit currentTimeChanged();
    emit selectionChanged();
    emit historyChanged();
}

const document::Snapshot& CompositionSession::snapshot() const noexcept { return snapshot_; }

const document::Snapshot& CompositionSession::evaluationSnapshot() const noexcept {
    return evaluationSnapshot_;
}

document::CompositionId CompositionSession::compositionId() const noexcept {
    return compositionId_;
}

const document::Composition* CompositionSession::composition() const noexcept {
    return snapshot_.project().findComposition(compositionId_);
}

const CompositionSelection& CompositionSession::selection() const noexcept { return selection_; }

core::RationalTime CompositionSession::currentTime() const noexcept { return currentTime_; }

bool CompositionSession::setComposition(const document::CompositionId compositionId) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (compositionId == compositionId_) {
        return true;
    }
    if (snapshot_.project().findComposition(compositionId) == nullptr) {
        reportUnavailable(QStringLiteral("The selected composition is no longer available"));
        return false;
    }

    // A composition switch cancels any active interaction (docs/architecture/animation-and-time.md,
    // "Direct Manipulation And Preview Overrides"): its frozen target/mapping belong to the OLD
    // composition.
    cancelValueEdit();
    cancelTransformInteraction();
    compositionId_ = compositionId;
    const bool timeChanged = currentTime_ != core::RationalTime::fromInteger(0);
    currentTime_ = core::RationalTime::fromInteger(0);
    const bool hadSelection = selection_.primary.index() != 0;
    selection_ = {};
    keyframeClipboard_.clear();
    selectedNodes_.clear();
    emit compositionChanged();
    if (timeChanged) {
        emit currentTimeChanged();
    }
    if (hadSelection) {
        emit selectionChanged();
    }
    return true;
}

bool CompositionSession::setCurrentTime(const core::RationalTime time) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (currentTime_ == time) {
        return false;
    }
    cancelValueEdit();
    cancelTransformInteraction();
    currentTime_ = time;
    emit currentTimeChanged();
    return true;
}

void CompositionSession::clearSelection() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (selection_ == CompositionSelection{} && selectedNodes_.empty()) {
        return;
    }
    selection_ = {};
    selectedNodes_.clear();
    emit selectionChanged();
}

void CompositionSession::selectLayer(const document::LayerId layerId, const bool extend) {
    Q_ASSERT(QThread::currentThread() == thread());
    const auto boundary = boundaryNodeForLayer(layerId);
    if (!boundary.has_value()) {
        reportUnavailable(QStringLiteral("The selected layer is no longer available"));
        return;
    }
    CompositionSelection next{.primary = layerId, .contextualLayer = layerId};
    auto nextNodes = extend ? selectedNodes_ : std::set<document::NodeId>{};
    nextNodes.insert(*boundary);
    if (selection_ != next || selectedNodes_ != nextNodes) {
        selection_ = next;
        selectedNodes_ = nextNodes;
        emit selectionChanged();
    }
}

const document::LayerStack* CompositionSession::timelineMerge() const noexcept {
    const auto* comp = composition();
    if (!comp)
        return nullptr;
    const auto id = comp->graph().outputMergeId();
    return id ? comp->graph().merge(*id) : nullptr;
}

void CompositionSession::selectNode(const document::NodeId nodeId) {
    selectNodes({nodeId}, nodeId);
}

void CompositionSession::selectNodes(std::set<document::NodeId> nodes,
                                     const document::NodeId primary) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (nodes.empty() && !primary.isValid()) {
        clearSelection();
        return;
    }
    const auto* current = composition();
    if (!current || !nodes.contains(primary) || std::ranges::any_of(nodes, [&](const auto id) {
            return current->graph().findNode(id) == nullptr;
        })) {
        reportUnavailable(
            QStringLiteral("Node selection requires existing nodes and a primary in the set"));
        return;
    }
    CompositionSelection next{.primary = primary, .contextualLayer = layerForNode(primary)};
    if (selection_ == next && selectedNodes_ == nodes)
        return;
    selection_ = next;
    selectedNodes_ = std::move(nodes);
    emit selectionChanged();
}

void CompositionSession::toggleNodeSelection(const document::NodeId nodeId) {
    Q_ASSERT(QThread::currentThread() == thread());
    const auto* current = composition();
    if (!current || !current->graph().findNode(nodeId)) {
        reportUnavailable(QStringLiteral("The selected node is no longer available"));
        return;
    }
    auto nodes = selectedNodes_;
    if (!nodes.erase(nodeId)) {
        nodes.insert(nodeId);
        selectNodes(std::move(nodes), nodeId);
        return;
    }
    if (nodes.empty()) {
        clearSelection();
        return;
    }
    const auto* primaryNode = selectedNode();
    if (primaryNode && nodes.contains(primaryNode->id)) {
        selectedNodes_ = std::move(nodes);
        emit selectionChanged();
    } else {
        const auto primary = *nodes.begin();
        selectNodes(std::move(nodes), primary);
    }
}

std::set<document::NodeId>
CompositionSession::dataBlockReaders(const document::DataBlockRecordId blockId) const {
    std::set<document::NodeId> readers;
    const auto* current = composition();
    if (current == nullptr || !snapshot_.project().findDataBlock(blockId))
        return readers;
    for (const auto& node : current->graph().nodes()) {
        const auto binding = std::ranges::find_if(node.parameters, [](const auto& item) {
            return item.role == document::kDataBlockParameterRole;
        });
        if (binding == node.parameters.end())
            continue;
        const auto* parameter = current->parameters().find(binding->parameterId);
        const auto* constant =
            parameter ? std::get_if<document::ConstantValueSource>(&parameter->source) : nullptr;
        const auto* stored = constant ? std::get_if<std::int64_t>(&constant->value) : nullptr;
        if (stored != nullptr && *stored > 0 &&
            static_cast<std::uint64_t>(*stored) == blockId.value())
            readers.insert(node.id);
    }
    return readers;
}

void CompositionSession::selectDataBlock(const document::DataBlockRecordId blockId) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!blockId.isValid() || snapshot_.project().findDataBlock(blockId) == nullptr) {
        reportUnavailable(QStringLiteral("The selected data block is no longer available"));
        return;
    }
    const auto readers = dataBlockReaders(blockId);
    CompositionSelection next{.primary = blockId, .contextualLayer = std::nullopt};
    if (selection_ == next && selectedNodes_ == readers)
        return;
    selection_ = next;
    selectedNodes_ = readers;
    emit selectionChanged();
}

void CompositionSession::selectParameter(const document::ParameterId parameterId) {
    Q_ASSERT(QThread::currentThread() == thread());
    const auto* current = composition();
    if (current == nullptr || current->parameters().find(parameterId) == nullptr) {
        reportUnavailable(QStringLiteral("The selected parameter is no longer available"));
        return;
    }

    CompositionSelection next{.primary = parameterId,
                              .contextualLayer = contextualLayerForParameter(parameterId)};
    if (selection_ != next || !selectedNodes_.empty()) {
        selection_ = next;
        selectedNodes_.clear();
        emit selectionChanged();
    }
}

const document::NodeRecord* CompositionSession::selectedNode() const noexcept {
    return nodeForSelection(selection_);
}

std::optional<document::LayerId>
CompositionSession::layerForNode(const document::NodeId nodeId) const {
    const auto* current = composition();
    if (current == nullptr) {
        return std::nullopt;
    }

    std::vector<document::NodeId> pending{nodeId};
    std::vector<document::NodeId> visited;
    std::optional<document::LayerId> resolvedLayer;
    while (!pending.empty()) {
        const auto currentNodeId = pending.back();
        pending.pop_back();
        if (std::ranges::find(visited, currentNodeId) != visited.end()) {
            continue;
        }
        visited.push_back(currentNodeId);

        for (const auto& boundary : current->graph().layerOutputs()) {
            if (boundary.nodeId != currentNodeId) {
                continue;
            }
            if (resolvedLayer.has_value() && *resolvedLayer != boundary.layerId) {
                return std::nullopt;
            }
            resolvedLayer = boundary.layerId;
        }

        for (const auto& edge : current->graph().edges()) {
            if (edge.source.nodeId != currentNodeId) {
                continue;
            }
            if (const auto* destination = std::get_if<document::NodeInputRef>(&edge.destination)) {
                pending.push_back(destination->nodeId);
            }
        }
    }
    return resolvedLayer;
}

std::optional<document::NodeId>
CompositionSession::boundaryNodeForLayer(const document::LayerId layerId) const noexcept {
    const auto* current = composition();
    if (current == nullptr) {
        return std::nullopt;
    }
    const auto boundary =
        std::ranges::find_if(current->graph().layerOutputs(), [layerId](const auto& candidate) {
            return candidate.layerId == layerId;
        });
    if (boundary == current->graph().layerOutputs().end()) {
        return std::nullopt;
    }
    return boundary->nodeId;
}

std::optional<document::NodeId>
CompositionSession::directSourceNodeForLayer(const document::LayerId layerId) const noexcept {
    const auto* current = composition();
    const auto boundaryNodeId = boundaryNodeForLayer(layerId);
    if (current == nullptr || !boundaryNodeId.has_value()) {
        return std::nullopt;
    }

    std::optional<document::NodeId> sourceNodeId;
    for (const auto& edge : current->graph().edges()) {
        const auto* destination = std::get_if<document::NodeInputRef>(&edge.destination);
        if (destination == nullptr || destination->nodeId != *boundaryNodeId ||
            destination->port != document::kLayerOutputContentInputPort) {
            continue;
        }
        const auto* sourceNode = current->graph().findNode(edge.source.nodeId);
        if (sourceNodeId.has_value() || sourceNode == nullptr) {
            return std::nullopt;
        }
        if ((sourceNode->typeId == document::kSolidSourceNodeType &&
             sourceNode->schemaVersion == document::kSolidSourceNodeSchemaVersion &&
             edge.source.port != document::kSolidSourceOutputPort) ||
            (sourceNode->typeId == document::kTextSourceNodeType &&
             sourceNode->schemaVersion == document::kTextSourceNodeSchemaVersion &&
             edge.source.port != document::kTextSourceOutputPort)) {
            return std::nullopt;
        }
        sourceNodeId = edge.source.nodeId;
    }
    return sourceNodeId;
}

const document::ParameterRecord*
CompositionSession::parameterForSelection(const std::string_view role) const noexcept {
    const auto* current = composition();
    const auto* node = nodeForSelection(selection_);
    if (current == nullptr) {
        return nullptr;
    }
    if (node != nullptr) {
        if (const auto* parameter = parameterForNode(*node, role)) {
            return parameter;
        }
        if (const auto* layerId = std::get_if<document::LayerId>(&selection_.primary)) {
            const auto sourceNodeId = directSourceNodeForLayer(*layerId);
            const auto* sourceNode =
                sourceNodeId.has_value() ? current->graph().findNode(*sourceNodeId) : nullptr;
            return sourceNode == nullptr ? nullptr : parameterForNode(*sourceNode, role);
        }
    }
    if (const auto* parameterId = std::get_if<document::ParameterId>(&selection_.primary)) {
        for (const auto& candidate : current->graph().nodes()) {
            const auto binding =
                std::ranges::find_if(candidate.parameters, [parameterId, role](const auto& item) {
                    return item.parameterId == *parameterId && item.role == role;
                });
            if (binding != candidate.parameters.end()) {
                return current->parameters().find(*parameterId);
            }
        }
    }
    return nullptr;
}

std::optional<double>
CompositionSession::constantValue(const document::ParameterId parameterId) const {
    const auto* current = composition();
    if (current == nullptr) {
        return std::nullopt;
    }
    const auto* parameter = current->parameters().find(parameterId);
    if (parameter == nullptr) {
        return std::nullopt;
    }
    const auto* source = std::get_if<document::ConstantValueSource>(&parameter->source);
    if (source == nullptr) {
        return std::nullopt;
    }
    const auto* value = std::get_if<double>(&source->value);
    return value == nullptr ? std::nullopt : std::optional<double>(*value);
}

std::optional<document::Vec2d>
CompositionSession::constantVec2Value(const document::ParameterId parameterId) const {
    const auto* current = composition();
    if (current == nullptr) {
        return std::nullopt;
    }
    const auto* parameter = current->parameters().find(parameterId);
    if (parameter == nullptr) {
        return std::nullopt;
    }
    const auto* source = std::get_if<document::ConstantValueSource>(&parameter->source);
    if (source == nullptr) {
        return std::nullopt;
    }
    const auto* value = std::get_if<document::Vec2d>(&source->value);
    return value == nullptr ? std::nullopt : std::optional<document::Vec2d>(*value);
}

std::optional<core::Color4d>
CompositionSession::constantColorValue(const document::ParameterId parameterId) const {
    const auto* current = composition();
    if (current == nullptr) {
        return std::nullopt;
    }
    const auto* parameter = current->parameters().find(parameterId);
    if (parameter == nullptr) {
        return std::nullopt;
    }
    const auto* source = std::get_if<document::ConstantValueSource>(&parameter->source);
    if (source == nullptr) {
        return std::nullopt;
    }
    const auto* value = std::get_if<core::Color4d>(&source->value);
    return value == nullptr ? std::nullopt : std::optional<core::Color4d>(*value);
}

std::optional<QString>
CompositionSession::constantStringValue(const document::ParameterId parameterId) const {
    const auto* current = composition();
    if (current == nullptr) {
        return std::nullopt;
    }
    const auto* parameter = current->parameters().find(parameterId);
    if (parameter == nullptr) {
        return std::nullopt;
    }
    const auto* source = std::get_if<document::ConstantValueSource>(&parameter->source);
    if (source == nullptr) {
        return std::nullopt;
    }
    const auto* value = std::get_if<std::string>(&source->value);
    return value == nullptr ? std::nullopt : std::optional<QString>(QString::fromStdString(*value));
}

bool CompositionSession::addShapeLayer(const document::ShapeKind kind,
                                       commands::ShapeLayerGeometry geometry) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!composition()) {
        reportUnavailable(tr("No composition is available for the new shape"));
        return false;
    }
    commands::Transaction transaction("Add Shape Layer", snapshot_.revision());
    transaction.emplace<commands::AddShapeLayer>(compositionId_, kind, std::move(geometry));
    const auto result = commandStack_->execute(std::move(transaction));
    if (!handleResult(result))
        return false;
    if (const auto layer = result.outputId<document::LayerId>("layer"))
        selectLayer(*layer);
    return true;
}

bool CompositionSession::addSolidLayer(const QString& name, const core::Color4d color) {
    Q_ASSERT(QThread::currentThread() == thread());
    const auto* current = composition();
    if (current == nullptr) {
        reportUnavailable(QStringLiteral("No composition is available for the new solid layer"));
        return false;
    }
    commands::Transaction transaction("Add Solid Layer", snapshot_.revision());
    transaction.emplace<commands::AddSolidLayer>(compositionId_, name.toStdString(), color,
                                                 compositionCenter(*current));
    const auto result = commandStack_->execute(std::move(transaction));
    const auto layerId = result.outputId<document::LayerId>(commands::kAddSolidLayerLayerOutput);
    if (!handleResult(result)) {
        return false;
    }
    if (layerId.has_value()) {
        selectLayer(*layerId);
    }
    return true;
}

bool CompositionSession::addTextLayer(const QString& name, const QString& text, const double size,
                                      const core::Color4d color,
                                      const std::optional<document::Vec2d> position) {
    Q_ASSERT(QThread::currentThread() == thread());
    const auto* current = composition();
    if (current == nullptr) {
        reportUnavailable(QStringLiteral("No composition is available for the new text layer"));
        return false;
    }
    commands::Transaction transaction("Add Text Layer", snapshot_.revision());
    transaction.emplace<commands::AddTextLayer>(
        compositionId_, name.toStdString(), text.toStdString(),
        position.value_or(compositionCenter(*current)), 1.0, size, color);
    const auto result = commandStack_->execute(std::move(transaction));
    const auto layerId = result.outputId<document::LayerId>(commands::kAddTextLayerLayerOutput);
    if (!handleResult(result)) {
        return false;
    }
    if (layerId.has_value()) {
        selectLayer(*layerId);
    }
    return true;
}

const document::ParameterRecord*
CompositionSession::parameterForTarget(const std::string_view role,
                                       const AuthoringTarget target) const noexcept {
    if (!target.has_value()) {
        return parameterForSelection(role);
    }
    const auto* current = composition();
    const auto* node = current == nullptr ? nullptr : current->graph().findNode(*target);
    return node == nullptr ? nullptr : parameterForNode(*node, role);
}

bool CompositionSession::setSelectionScalarParameter(const std::string_view role,
                                                     const double value,
                                                     const QString& commandLabel,
                                                     const AuthoringTarget target) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!std::isfinite(value)) {
        reportUnavailable(QStringLiteral("The parameter value must be finite"));
        return false;
    }

    const auto* parameter = parameterForTarget(role, target);
    if (parameter == nullptr) {
        reportUnavailable(QStringLiteral("The selected object does not expose this parameter"));
        return false;
    }
    if (isValueEditing(parameter->id))
        return updateValueEdit(value);
    commands::Transaction transaction(commandLabel.toStdString(), snapshot_.revision());
    if (const auto* constantSource =
            std::get_if<document::ConstantValueSource>(&parameter->source)) {
        if (std::get_if<double>(&constantSource->value) == nullptr) {
            reportUnavailable(QStringLiteral("The parameter value does not match its schema"));
            return false;
        }
        transaction.emplace<commands::SetParameterSource>(compositionId_, parameter->id,
                                                          document::ConstantValueSource{value});
    } else if (const auto* animationSource =
                   std::get_if<document::AnimationCurveSource>(&parameter->source)) {
        transaction.emplace<commands::SetKeyframeAtTime>(compositionId_, animationSource->curveId,
                                                         currentTime_, value);
    } else {
        reportUnavailable(
            QStringLiteral("Disconnect the driven parameter before editing its value"));
        return false;
    }
    return execute(std::move(transaction));
}

bool CompositionSession::setSelectedPosition(const double x, const double y,
                                             const AuthoringTarget target) {
    Q_ASSERT(QThread::currentThread() == thread());
    const auto* position = parameterForTarget(document::kPositionParameterRole, target);
    if (position == nullptr) {
        reportUnavailable(QStringLiteral("The selected object does not expose a position"));
        return false;
    }
    if (!std::isfinite(x) || !std::isfinite(y)) {
        reportUnavailable(QStringLiteral("Position values must be finite"));
        return false;
    }
    return executePositionCommand(position->id, currentTime_, document::Vec2d{x, y},
                                  QStringLiteral("Set Position"));
}

bool CompositionSession::executePositionCommand(const document::ParameterId parameterId,
                                                const core::RationalTime time,
                                                const document::Vec2d value,
                                                const QString& commandLabel) {
    if (isValueEditing(parameterId))
        return updateValueEdit(value);
    commands::Transaction transaction(commandLabel.toStdString(), snapshot_.revision());
    return appendParameterEdit(transaction, parameterId, time, value) &&
           executeTransaction(std::move(transaction)).succeeded();
}

bool CompositionSession::setSelectionVec2Parameter(const std::string_view role, const double x,
                                                   const double y, const QString& commandLabel,
                                                   const AuthoringTarget target) {
    Q_ASSERT(QThread::currentThread() == thread());
    const auto* parameter = parameterForTarget(role, target);
    if (parameter == nullptr) {
        reportUnavailable(QStringLiteral("The selected object does not expose this parameter"));
        return false;
    }
    if (!std::isfinite(x) || !std::isfinite(y)) {
        reportUnavailable(QStringLiteral("The parameter value must be finite"));
        return false;
    }
    // Deliberately the same write path position uses -- one transaction carrying either a constant
    // rewrite or a keyframe at the session time -- so every Vec2d transform row is one undo step
    // and behaves identically whether it is static or animated.
    return executePositionCommand(parameter->id, currentTime_, document::Vec2d{x, y}, commandLabel);
}

bool CompositionSession::setParameterComponentValue(const document::ParameterId parameterId,
                                                    const document::AnimationComponent component,
                                                    const double value) {
    Q_ASSERT(QThread::currentThread() == thread());
    const auto* current = composition();
    const auto* parameter = current ? current->parameters().find(parameterId) : nullptr;
    if (!parameter || !std::isfinite(value)) {
        reportUnavailable(QStringLiteral("The component value is not available or finite"));
        return false;
    }
    if (valueEdit_ && isValueEditing(parameterId)) {
        auto next = valueEdit_->value;
        if (auto* target = editedComponent(next, component)) {
            *target = value;
            return updateValueEdit(std::move(next));
        }
        return false;
    }
    if (const auto* constant = std::get_if<document::ConstantValueSource>(&parameter->source)) {
        auto updated = constant->value;
        const bool valid = std::visit(
            [&](auto& held) {
                using Value = std::decay_t<decltype(held)>;
                if constexpr (std::is_same_v<Value, document::Vec2d> ||
                              std::is_same_v<Value, document::Vec3d>) {
                    if (component == document::AnimationComponent::X) {
                        held.x = value;
                        return true;
                    }
                    if (component == document::AnimationComponent::Y) {
                        held.y = value;
                        return true;
                    }
                    if constexpr (std::is_same_v<Value, document::Vec3d>)
                        if (component == document::AnimationComponent::Z) {
                            held.z = value;
                            return true;
                        }
                } else if constexpr (std::is_same_v<Value, core::Color4d>) {
                    if (component == document::AnimationComponent::Red) {
                        held.red = value;
                        return true;
                    }
                    if (component == document::AnimationComponent::Green) {
                        held.green = value;
                        return true;
                    }
                    if (component == document::AnimationComponent::Blue) {
                        held.blue = value;
                        return true;
                    }
                    if (component == document::AnimationComponent::Alpha) {
                        held.alpha = value;
                        return true;
                    }
                }
                return false;
            },
            updated);
        if (valid)
            return setParameterValue(parameterId, std::move(updated),
                                     QStringLiteral("Set Component"));
        reportUnavailable(QStringLiteral("This component does not belong to the parameter"));
        return false;
    }
    commands::Transaction transaction("Set Component", snapshot_.revision());
    transaction.emplace<commands::SetKeyframeAtTimeForParameterComponent>(
        compositionId_, parameterId, component, currentTime_, value);
    return execute(std::move(transaction));
}

bool CompositionSession::appendParameterEdit(commands::Transaction& transaction,
                                             const document::ParameterId parameterId,
                                             const core::RationalTime time,
                                             document::ParameterValue value) {
    Q_ASSERT(QThread::currentThread() == thread());
    const auto* current = composition();
    const auto* parameter = current == nullptr ? nullptr : current->parameters().find(parameterId);
    if (parameter == nullptr) {
        reportUnavailable(QStringLiteral("The selected object does not expose this parameter"));
        return false;
    }
    if (std::holds_alternative<document::ConstantValueSource>(parameter->source)) {
        transaction.emplace<commands::SetParameterSource>(
            compositionId_, parameterId, document::ConstantValueSource{std::move(value)});
        return true;
    }
    const auto* animated = std::get_if<document::AnimationCurveSource>(&parameter->source);
    if (animated == nullptr) {
        reportUnavailable(
            QStringLiteral("Disconnect the driven parameter before editing its value"));
        return false;
    }
    // Only the three kinds a curve can carry reach a key; anything else on an animated parameter is
    // a document inconsistency this command did not create, refused rather than silently flattened.
    const bool queued = std::visit(
        [&](const auto& held) {
            using Held = std::decay_t<decltype(held)>;
            if constexpr (std::is_same_v<Held, double> || std::is_same_v<Held, document::Vec2d> ||
                          std::is_same_v<Held, document::Vec3d> ||
                          std::is_same_v<Held, core::Color4d>) {
                transaction.emplace<commands::SetKeyframeAtTime>(compositionId_, animated->curveId,
                                                                 time, held);
                return true;
            } else {
                return false;
            }
        },
        value);
    if (!queued) {
        reportUnavailable(QStringLiteral("This parameter's kind cannot carry a keyframe"));
        return false;
    }
    return true;
}

bool CompositionSession::setParameterValue(const document::ParameterId parameterId,
                                           document::ParameterValue value,
                                           const QString& commandLabel) {
    if (isValueEditing(parameterId))
        return updateValueEdit(std::move(value));
    commands::Transaction transaction(commandLabel.toStdString(), snapshot_.revision());
    return appendParameterEdit(transaction, parameterId, currentTime_, std::move(value)) &&
           executeTransaction(std::move(transaction)).succeeded();
}

bool CompositionSession::setSelectedAnchor(const double x, const double y,
                                           const AuthoringTarget target) {
    return setSelectionVec2Parameter(document::kAnchorParameterRole, x, y,
                                     QStringLiteral("Set Anchor"), target);
}

bool CompositionSession::setSelectedScale(const double x, const double y,
                                          const AuthoringTarget target) {
    return setSelectionVec2Parameter(document::kScaleParameterRole, x, y,
                                     QStringLiteral("Set Scale"), target);
}

bool CompositionSession::setSelectedRotation(const double degrees, const AuthoringTarget target) {
    // No domain clamp: rotation is authored in degrees and may wind past a full turn in either
    // direction. Only finiteness is required, which setSelectionScalarParameter already enforces.
    return setSelectionScalarParameter(document::kRotationParameterRole, degrees,
                                       QStringLiteral("Set Rotation"), target);
}

bool CompositionSession::setSelectedOpacity(const double opacity, const AuthoringTarget target) {
    if (opacity < 0.0 || opacity > 1.0) {
        reportUnavailable(QStringLiteral("Opacity must be between zero and one"));
        return false;
    }
    return setSelectionScalarParameter(document::kOpacityParameterRole, opacity,
                                       QStringLiteral("Set Opacity"), target);
}

bool CompositionSession::setSelectedTextContent(const QString& content,
                                                const AuthoringTarget target) {
    Q_ASSERT(QThread::currentThread() == thread());
    const auto utf8 = content.toStdString();
    if (!core::isValidUtf8(utf8)) {
        reportUnavailable(QStringLiteral("The text content must be valid UTF-8"));
        return false;
    }
    const auto* parameter = parameterForTarget(document::kTextParameterRole, target);
    if (parameter == nullptr) {
        reportUnavailable(QStringLiteral("The selected object does not expose text content"));
        return false;
    }
    const auto* constantSource = std::get_if<document::ConstantValueSource>(&parameter->source);
    if (constantSource == nullptr || std::get_if<std::string>(&constantSource->value) == nullptr) {
        // The content schema is String, the one parameter schema that is still constant-only:
        // CreateAnimationForParameter refuses it and SetKeyframeAtTime has no string overload, so a
        // non-constant source here is a pre-existing document inconsistency rather than anything
        // this command created -- refused the same way the driven-parameter branches above are.
        reportUnavailable(QStringLiteral("Disconnect the driven text content before editing it"));
        return false;
    }
    if (isValueEditing(parameter->id))
        return updateValueEdit(utf8);
    if (*std::get_if<std::string>(&constantSource->value) == utf8) {
        return true;
    }
    commands::Transaction transaction("Set Text Content", snapshot_.revision());
    transaction.emplace<commands::SetParameterSource>(compositionId_, parameter->id,
                                                      document::ConstantValueSource{utf8});
    return execute(std::move(transaction));
}

bool CompositionSession::setSelectedTextSize(const double size, const AuthoringTarget target) {
    if (!std::isfinite(size) || size <= 0.0 || size > document::kMaximumTextSizePixels) {
        reportUnavailable(QStringLiteral("The text size must be between zero and %1 pixels")
                              .arg(document::kMaximumTextSizePixels));
        return false;
    }
    return setSelectionScalarParameter(document::kTextSizeParameterRole, size,
                                       QStringLiteral("Set Text Size"), target);
}

bool CompositionSession::setSelectedTextColor(const core::Color4d color,
                                              const AuthoringTarget target) {
    return setSelectionColorParameter(document::kTextColorParameterRole, color,
                                      QStringLiteral("Set Text Color"), target);
}

std::optional<core::BlendMode>
CompositionSession::blendModeForLayer(const document::LayerId layerId) const noexcept {
    const auto* current = composition();
    const auto boundaryNodeId = boundaryNodeForLayer(layerId);
    const auto* node = current == nullptr || !boundaryNodeId.has_value()
                           ? nullptr
                           : current->graph().findNode(*boundaryNodeId);
    const auto* parameter =
        node == nullptr ? nullptr : parameterForNode(*node, document::kBlendModeParameterRole);
    const auto* constantSource =
        parameter == nullptr ? nullptr
                             : std::get_if<document::ConstantValueSource>(&parameter->source);
    const auto* stored =
        constantSource == nullptr ? nullptr : std::get_if<std::int64_t>(&constantSource->value);
    return stored == nullptr ? std::nullopt : core::blendModeFromStoredValue(*stored);
}

std::optional<document::LayerId> CompositionSession::parentOf(document::LayerId layer) const {
    const auto* current = composition();
    const auto* boundary = current ? current->graph().findLayer(layer) : nullptr;
    return boundary ? boundary->parent : std::nullopt;
}

std::vector<document::LayerId> CompositionSession::childrenOf(document::LayerId layer) const {
    std::vector<document::LayerId> children;
    if (const auto* current = composition()) {
        for (const auto& boundary : current->graph().layerOutputs())
            if (boundary.parent == layer)
                children.push_back(boundary.layerId);
    }
    return children;
}

std::vector<document::LayerId> CompositionSession::candidateParents(document::LayerId layer) const {
    const auto* current = composition();
    if (!current || !current->graph().findLayer(layer))
        return {};
    std::set<document::LayerId> excluded{layer};
    std::vector<document::LayerId> pending{layer};
    while (!pending.empty()) {
        const auto parent = pending.back();
        pending.pop_back();
        for (const auto child : childrenOf(parent))
            if (excluded.insert(child).second)
                pending.push_back(child);
    }
    std::vector<document::LayerId> candidates;
    for (const auto& boundary : current->graph().layerOutputs())
        if (!excluded.contains(boundary.layerId))
            candidates.push_back(boundary.layerId);
    return candidates;
}

bool CompositionSession::setLayerParent(document::LayerId layer,
                                        std::optional<document::LayerId> parent) {
    Q_ASSERT(QThread::currentThread() == thread());
    commands::Transaction transaction("Set Layer Parent", snapshot_.revision());
    transaction.emplace<commands::SetLayerParent>(compositionId_, layer, parent);
    return execute(std::move(transaction));
}

bool CompositionSession::setLayerBlendMode(const document::LayerId layerId,
                                           const core::BlendMode mode) {
    Q_ASSERT(QThread::currentThread() == thread());
    const auto* current = composition();
    const auto boundaryNodeId = boundaryNodeForLayer(layerId);
    const auto* node = current == nullptr || !boundaryNodeId.has_value()
                           ? nullptr
                           : current->graph().findNode(*boundaryNodeId);
    const auto* parameter =
        node == nullptr ? nullptr : parameterForNode(*node, document::kBlendModeParameterRole);
    if (parameter == nullptr) {
        reportUnavailable(QStringLiteral("That layer does not expose a blend mode"));
        return false;
    }
    const auto* constantSource = std::get_if<document::ConstantValueSource>(&parameter->source);
    if (constantSource == nullptr || std::get_if<std::int64_t>(&constantSource->value) == nullptr) {
        // The blend-mode schema is constant-only -- CreateAnimationForParameter accepts only the
        // animatable transform and opacity schemas, and SetKeyframeAtTime has no integer overload
        // -- so a non-constant source here is a pre-existing document inconsistency rather than
        // anything this command created. Refused exactly as the colour path refuses a driven
        // colour.
        reportUnavailable(QStringLiteral("Disconnect the driven blend mode before editing it"));
        return false;
    }
    const auto stored = core::blendModeStoredValue(mode);
    if (*std::get_if<std::int64_t>(&constantSource->value) == stored) {
        return true;
    }
    commands::Transaction transaction("Set Blend Mode", snapshot_.revision());
    transaction.emplace<commands::SetParameterSource>(compositionId_, parameter->id,
                                                      document::ConstantValueSource{stored});
    return execute(std::move(transaction));
}

bool CompositionSession::setSelectedBlendMode(const core::BlendMode mode) {
    Q_ASSERT(QThread::currentThread() == thread());
    // Same resolution rule every other selection-driven writer uses: a directly selected layer, or
    // the contextual layer a node/parameter/keyframe selection resolved to.
    const auto* directLayer = std::get_if<document::LayerId>(&selection_.primary);
    const auto layerId =
        directLayer != nullptr ? std::optional(*directLayer) : selection_.contextualLayer;
    if (!layerId.has_value()) {
        reportUnavailable(QStringLiteral("The selected object does not expose a blend mode"));
        return false;
    }
    return setLayerBlendMode(*layerId, mode);
}

bool CompositionSession::setSelectedSolidColor(const core::Color4d color,
                                               const AuthoringTarget target) {
    return setSelectionColorParameter(document::kSolidColorParameterRole, color,
                                      QStringLiteral("Set Solid Color"), target);
}

bool CompositionSession::setSelectionColorParameter(const std::string_view role,
                                                    const core::Color4d color,
                                                    const QString& commandLabel,
                                                    const AuthoringTarget target) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!std::isfinite(color.red) || !std::isfinite(color.green) || !std::isfinite(color.blue) ||
        !std::isfinite(color.alpha)) {
        reportUnavailable(QStringLiteral("Color values must be finite"));
        return false;
    }
    const auto* parameter = parameterForTarget(role, target);
    if (parameter == nullptr) {
        reportUnavailable(QStringLiteral("The selected object does not expose a color"));
        return false;
    }
    if (isValueEditing(parameter->id))
        return updateValueEdit(color);
    commands::Transaction transaction(commandLabel.toStdString(), snapshot_.revision());
    if (const auto* constantSource =
            std::get_if<document::ConstantValueSource>(&parameter->source)) {
        if (std::get_if<core::Color4d>(&constantSource->value) == nullptr) {
            reportUnavailable(QStringLiteral("The color value does not match its schema"));
            return false;
        }
        transaction.emplace<commands::SetParameterSource>(compositionId_, parameter->id,
                                                          document::ConstantValueSource{color});
    } else if (const auto* animationSource =
                   std::get_if<document::AnimationCurveSource>(&parameter->source)) {
        // Task S5, item 1: a colour parameter can be animated now, so editing one writes a key at
        // the session time through exactly the branch position and opacity already take. Before
        // this task no command could put a colour on a curve at all, and this method refused every
        // non-constant source on that basis.
        transaction.emplace<commands::SetKeyframeAtTime>(compositionId_, animationSource->curveId,
                                                         currentTime_, color);
    } else {
        reportUnavailable(QStringLiteral("Disconnect the driven color before editing its value"));
        return false;
    }
    return execute(std::move(transaction));
}

bool CompositionSession::moveLayerBefore(const document::LayerSlotId slotId,
                                         const std::optional<document::LayerSlotId> beforeSlotId) {
    Q_ASSERT(QThread::currentThread() == thread());
    commands::Transaction transaction("Reorder Layer", snapshot_.revision());
    transaction.emplace<commands::MoveLayerBefore>(compositionId_, slotId, beforeSlotId);
    return execute(std::move(transaction));
}

bool CompositionSession::deleteSelectedKeyframe() {
    Q_ASSERT(QThread::currentThread() == thread());
    const auto* keySelection = std::get_if<KeyframeSelection>(&selection_.primary);
    if (keySelection == nullptr) {
        return false;
    }
    commands::Transaction transaction("Delete Keyframe", snapshot_.revision());
    transaction.emplace<commands::DeleteKeyframe>(compositionId_, keySelection->curveId,
                                                  keySelection->keyframeId);
    return execute(std::move(transaction));
}

bool CompositionSession::moveSelectedKeyframe(const core::RationalTime newTime) {
    Q_ASSERT(QThread::currentThread() == thread());
    const auto* keySelection = std::get_if<KeyframeSelection>(&selection_.primary);
    if (keySelection == nullptr) {
        return false;
    }
    const auto* current = composition();
    const auto* record =
        current == nullptr ? nullptr : current->animationCurves().find(keySelection->curveId);
    if (record == nullptr) {
        return false;
    }

    commands::Transaction transaction("Move Keyframe", snapshot_.revision());
    if (const auto* scalar = std::get_if<document::ScalarAnimationCurve>(record)) {
        const auto key = std::ranges::find(scalar->keyframes, keySelection->keyframeId,
                                           &document::ScalarKeyframe::id);
        if (key == scalar->keyframes.end()) {
            return false;
        }
        if (key->time == newTime) {
            // Zero effective change commits nothing (docs/architecture/animation-and-time.md's
            // zero-move precedent -- mirrors commitTransformInteraction()'s zero-move branch).
            return true;
        }
        transaction.emplace<commands::UpdateScalarKeyframe>(compositionId_, keySelection->curveId,
                                                            keySelection->keyframeId, newTime,
                                                            key->value, key->outgoingInterpolation);
    } else if (!std::holds_alternative<document::ScalarAnimationCurve>(*record)) {
        // A vector or colour key lives on a component curve, so the move is addressed as
        // KeyframeAddress{curve, key, component}: a component-tagged selection moves that lane's
        // key, and a component-less one moves every component key sharing its exact time.
        const auto time = keyframeTimeForSelection(*record, *keySelection);
        if (!time.has_value()) {
            return false;
        }
        if (*time == newTime) {
            // Zero effective change commits nothing, exactly as the scalar arm above.
            return true;
        }
        transaction.emplace<commands::MoveKeyframes>(
            compositionId_,
            std::vector<commands::KeyframeMove>{
                {{keySelection->curveId, keySelection->keyframeId, keySelection->component},
                 newTime}});
    } else {
        return false;
    }
    return execute(std::move(transaction));
}

bool CompositionSession::insertKeyframeAtTime(const document::AnimationCurveId curveId,
                                              const core::RationalTime time) {
    Q_ASSERT(QThread::currentThread() == thread());
    const auto* current = composition();
    if (current == nullptr) {
        return false;
    }
    const auto parameterId = parameterForCurve(curveId);
    const auto* parameter =
        parameterId.has_value() ? current->parameters().find(*parameterId) : nullptr;
    if (parameter == nullptr) {
        return false;
    }
    const auto sample = sampleParameterValue(*parameter, time);
    if (!sample.has_value()) {
        return false;
    }

    commands::Transaction transaction("Insert Keyframe", snapshot_.revision());
    if (const auto* scalar = std::get_if<double>(&*sample)) {
        transaction.emplace<commands::InsertScalarKeyframe>(compositionId_, curveId, time, *scalar);
    } else if (const auto* vector = std::get_if<document::Vec2d>(&*sample)) {
        transaction.emplace<commands::InsertVec2Keyframe>(compositionId_, curveId, time, *vector);
    } else {
        return false;
    }

    const auto result = commandStack_->execute(std::move(transaction));
    const auto keyframeId = result.outputId<document::KeyframeId>(commands::kKeyframeOutput);
    if (!handleResult(result)) {
        return false;
    }
    if (keyframeId.has_value()) {
        // One-truth selection swap, same as every other select* method (K1's precedent).
        selectKeyframe(curveId, *keyframeId);
    }
    return true;
}

#include "composition_session_transform.ipp"
#include "composition_session_value_edit.ipp"

bool CompositionSession::canUndo() const noexcept { return commandStack_->canUndo(); }

bool CompositionSession::canRedo() const noexcept { return commandStack_->canRedo(); }

QString CompositionSession::undoLabel() const {
    const auto label = commandStack_->undoLabel();
    return label.has_value()
               ? QString::fromUtf8(label->data(), static_cast<qsizetype>(label->size()))
               : QString{};
}

QString CompositionSession::redoLabel() const {
    const auto label = commandStack_->redoLabel();
    return label.has_value()
               ? QString::fromUtf8(label->data(), static_cast<qsizetype>(label->size()))
               : QString{};
}

bool CompositionSession::undo() {
    Q_ASSERT(QThread::currentThread() == thread());
    return handleResult(commandStack_->undo());
}

bool CompositionSession::redo() {
    Q_ASSERT(QThread::currentThread() == thread());
    return handleResult(commandStack_->redo());
}

commands::CommandResult
CompositionSession::executeNodeTransaction(commands::Transaction&& transaction) {
    return executeTransaction(std::move(transaction));
}

commands::CommandResult
CompositionSession::executeTransaction(commands::Transaction&& transaction) {
    Q_ASSERT(QThread::currentThread() == thread());
    auto result = commandStack_->execute(std::move(transaction));
    (void)handleResult(result);
    return result;
}

bool CompositionSession::execute(commands::Transaction&& transaction) {
    return handleResult(commandStack_->execute(std::move(transaction)));
}

void CompositionSession::handleCommandEvent(const commands::CommandEvent& event) {
    if (event.kind == commands::CommandEventKind::Rejected) {
        // The direct CompositionSession command methods retain ownership of the user-facing
        // rejection signal in handleResult(). External adapters (for example the node-editor
        // fixture and future editor panels) may already publish that signal after calling the
        // command stack, so the seam only publishes the non-duplicating history transition here.
        emit historyChanged();
        return;
    }
    if (event.kind == commands::CommandEventKind::RevisionChanged) {
        const auto previousRevision = snapshot_.revision();
        snapshot_ = document_->snapshot();
        const bool revisionAdvanced = snapshot_.revision() != previousRevision;
        // Retain the last genuine evaluation snapshot only when this command proved, from its own
        // published metadata, that it changed no rendered pixel and sat exactly on the live
        // revision chain of the same project. Every other case -- an unclassified or
        // render-affecting command, a stale/external revision, a different project -- advances the
        // evaluation snapshot to live, which is the conservative choice.
        const bool retainEvaluationSnapshot =
            revisionAdvanced && !event.result.renderAffecting && event.result.succeeded() &&
            event.result.beforeRevision == previousRevision &&
            event.result.afterRevision == snapshot_.revision() &&
            evaluationSnapshot_.project().id() == snapshot_.project().id();
        // Adopt the new evaluation snapshot BEFORE invalidating interactions or cancelling value
        // edits: both emit signals whose handlers build a preview request, and a request built
        // during that window must already see the revision it is allowed to evaluate. This is the
        // same reason rebind() resets it before publishing.
        if (revisionAdvanced && !retainEvaluationSnapshot) {
            evaluationSnapshot_ = snapshot_;
        }
        invalidateTransformInteractionOnStaleRevision();
        if (valueEdit_ && valueEdit_->revision != snapshot_.revision()) {
            cancelValueEdit();
        }
        if (revisionAdvanced) {
            normalizeSelection();
            emit snapshotChanged();
            if (!retainEvaluationSnapshot)
                emit evaluationChanged();
        }
        return;
    }
    emit historyChanged();
}

bool CompositionSession::handleResult(const commands::CommandResult& result) {
    if (!result.succeeded()) {
        const auto message = statusMessage(result);
        if (!message.isEmpty()) {
            emit commandRejected(message);
        }
    }
    return result.succeeded();
}

const document::NodeRecord*
CompositionSession::nodeForSelection(const CompositionSelection& selection) const noexcept {
    const auto* current = composition();
    if (current == nullptr) {
        return nullptr;
    }
    if (const auto* nodeId = std::get_if<document::NodeId>(&selection.primary)) {
        return current->graph().findNode(*nodeId);
    }
    if (const auto* layerId = std::get_if<document::LayerId>(&selection.primary)) {
        const auto nodeId = boundaryNodeForLayer(*layerId);
        return nodeId.has_value() ? current->graph().findNode(*nodeId) : nullptr;
    }
    return nullptr;
}

const document::ParameterRecord*
CompositionSession::parameterForNode(const document::NodeRecord& node,
                                     const std::string_view role) const noexcept {
    const auto* current = composition();
    if (current == nullptr) {
        return nullptr;
    }
    const auto binding = std::ranges::find_if(
        node.parameters, [role](const auto& item) { return item.role == role; });
    return binding == node.parameters.end() ? nullptr
                                            : current->parameters().find(binding->parameterId);
}

bool CompositionSession::selectionExists(const CompositionSelection& selection) const {
    const auto* current = composition();
    if (current == nullptr) {
        return false;
    }
    if (std::holds_alternative<std::monostate>(selection.primary)) {
        return true;
    }
    if (const auto* layerId = std::get_if<document::LayerId>(&selection.primary)) {
        return boundaryNodeForLayer(*layerId).has_value();
    }
    if (const auto* nodeId = std::get_if<document::NodeId>(&selection.primary)) {
        return current->graph().findNode(*nodeId) != nullptr;
    }
    if (const auto* keySelection = std::get_if<KeyframeSelection>(&selection.primary)) {
        return keyframeSelectionExists(*keySelection);
    }
    if (const auto* blockId = std::get_if<document::DataBlockRecordId>(&selection.primary)) {
        return snapshot_.project().findDataBlock(*blockId) != nullptr;
    }
    const auto* parameterId = std::get_if<document::ParameterId>(&selection.primary);
    return parameterId != nullptr && current->parameters().find(*parameterId) != nullptr;
}

bool CompositionSession::keyframeSelectionExists(const KeyframeSelection& selection) const {
    const auto* current = composition();
    if (current == nullptr) {
        return false;
    }
    const auto* record = current->animationCurves().find(selection.curveId);
    if (record == nullptr) {
        return false;
    }
    return std::visit(
        [&](const auto& curve) {
            using Curve = std::decay_t<decltype(curve)>;
            const auto holdsKey = [&](const auto& keys) {
                return std::ranges::any_of(
                    keys, [&](const auto& key) { return key.id == selection.keyframeId; });
            };
            if constexpr (std::is_same_v<Curve, document::ScalarAnimationCurve>) {
                return !selection.component.has_value() && holdsKey(curve.keyframes);
            } else if (selection.component.has_value()) {
                const auto* component = curve.component(*selection.component);
                return component != nullptr && holdsKey(component->keyframes);
            } else {
                return std::ranges::any_of(curve.components, [&](const auto& component) {
                    return holdsKey(component.keyframes);
                });
            }
        },
        *record);
}

std::optional<document::AnimationComponent>
CompositionSession::componentForKeyframe(const document::AnimationCurveId curveId,
                                         const document::KeyframeId keyframeId) const {
    const auto* current = composition();
    const auto* record = current == nullptr ? nullptr : current->animationCurves().find(curveId);
    if (record == nullptr) {
        return std::nullopt;
    }
    // A vector or colour key belongs to exactly one component curve, and the component is half of
    // its address now that the whole-value projection is gone. Resolving it once, here, is what
    // lets the component-less selectKeyframe() overload stay the caller-facing convenience it was.
    return std::visit(
        [keyframeId](const auto& curve) -> std::optional<document::AnimationComponent> {
            using Curve = std::decay_t<decltype(curve)>;
            if constexpr (!std::is_same_v<Curve, document::ScalarAnimationCurve>) {
                for (const auto component : animationComponentsOf<Curve>()) {
                    const auto* values = curve.component(component);
                    if (values != nullptr &&
                        std::ranges::any_of(values->keyframes, [keyframeId](const auto& key) {
                            return key.id == keyframeId;
                        }))
                        return component;
                }
            }
            return std::nullopt;
        },
        *record);
}

std::optional<core::RationalTime>
CompositionSession::keyframeTimeForSelection(const document::AnimationCurveRecord& record,
                                             const KeyframeSelection& selection) {
    return std::visit(
        [&](const auto& curve) -> std::optional<core::RationalTime> {
            using Curve = std::decay_t<decltype(curve)>;
            const auto timeOf = [&](const auto& keys) -> std::optional<core::RationalTime> {
                const auto key =
                    std::ranges::find(keys, selection.keyframeId, &document::ScalarKeyframe::id);
                return key == keys.end() ? std::nullopt : std::optional{key->time};
            };
            if constexpr (std::is_same_v<Curve, document::ScalarAnimationCurve>) {
                return selection.component.has_value() ? std::nullopt : timeOf(curve.keyframes);
            } else if (selection.component.has_value()) {
                const auto* component = curve.component(*selection.component);
                return component == nullptr ? std::nullopt : timeOf(component->keyframes);
            } else {
                for (const auto& component : curve.components)
                    if (const auto time = timeOf(component.keyframes))
                        return time;
                return std::nullopt;
            }
        },
        record);
}

std::optional<document::ParameterId>
CompositionSession::parameterForCurve(const document::AnimationCurveId curveId) const noexcept {
    const auto* current = composition();
    if (current == nullptr) {
        return std::nullopt;
    }
    for (const auto& parameter : current->parameters().records()) {
        const auto* source = std::get_if<document::AnimationCurveSource>(&parameter.source);
        if (source != nullptr && source->curveId == curveId) {
            return parameter.id;
        }
    }
    return std::nullopt;
}

std::optional<ParameterSample>
CompositionSession::sampleParameterValue(const document::ParameterRecord& parameter,
                                         const core::RationalTime time) const {
    const auto* current = composition();
    if (current == nullptr) {
        return std::nullopt;
    }
    const auto* source = std::get_if<document::AnimationCurveSource>(&parameter.source);
    if (source == nullptr) {
        // Only for animation-sourced parameters (decision 2); constant/driven callers keep their
        // existing paths (constantValue()/constantVec2Value(), the explicit driven-refusal
        // branches).
        return std::nullopt;
    }
    if (const auto* scalarCurve = current->animationCurves().findScalar(source->curveId)) {
        const auto sample =
            runtime::sampleAnimationCurve(runtime::compileAnimationCurve(*scalarCurve), time);
        if (!sample || !sample.value.has_value()) {
            return std::nullopt;
        }
        return ParameterSample(*sample.value);
    }
    if (const auto* vec2Curve = current->animationCurves().findVec2(source->curveId)) {
        auto compiled = runtime::compileAnimationCurve(*vec2Curve);
        if (source->defaultValue.has_value()) {
            if (const auto* value = std::get_if<document::Vec2d>(&*source->defaultValue))
                compiled.defaultValue = *value;
        }
        const auto sample = runtime::sampleAnimationCurve(compiled, time);
        if (!sample || !sample.value.has_value()) {
            return std::nullopt;
        }
        return ParameterSample(*sample.value);
    }
    if (const auto* vec3Curve = current->animationCurves().findVec3(source->curveId)) {
        auto compiled = runtime::compileAnimationCurve(*vec3Curve);
        if (source->defaultValue.has_value()) {
            if (const auto* value = std::get_if<document::Vec3d>(&*source->defaultValue))
                compiled.defaultValue = *value;
        }
        const auto sample = runtime::sampleAnimationCurve(compiled, time);
        if (!sample || !sample.value.has_value()) {
            return std::nullopt;
        }
        return ParameterSample(*sample.value);
    }
    if (const auto* colorCurve = current->animationCurves().findColor4(source->curveId)) {
        auto compiled = runtime::compileAnimationCurve(*colorCurve);
        if (source->defaultValue.has_value()) {
            if (const auto* value = std::get_if<core::Color4d>(&*source->defaultValue))
                compiled.defaultValue = *value;
        }
        const auto sample = runtime::sampleAnimationCurve(compiled, time);
        if (!sample || !sample.value.has_value()) {
            return std::nullopt;
        }
        return ParameterSample(*sample.value);
    }
    return std::nullopt;
}

std::optional<ParameterSample>
CompositionSession::effectiveParameterValue(const document::ParameterRecord* parameter) const {
    if (parameter == nullptr) {
        return std::nullopt;
    }
    if (transformInteraction_) {
        const auto& overrides = transformInteraction_->overrides;
        const auto found = std::ranges::find(overrides, parameter->id,
                                             &runtime::SnapshotParameterOverride::parameterId);
        if (found != overrides.end())
            return std::visit(
                [](const auto& value) -> std::optional<ParameterSample> {
                    if constexpr (std::is_constructible_v<ParameterSample,
                                                          const std::decay_t<decltype(value)>&>)
                        return ParameterSample(value);
                    else
                        return std::nullopt;
                },
                found->value);
    }
    if (valueEdit_ && valueEdit_->anchor && valueEdit_->anchor->parameter == parameter->id)
        return ParameterSample(valueEdit_->anchor->value);
    if (valueEdit_ && isValueEditing(parameter->id))
        return std::visit(
            [](const auto& value) -> std::optional<ParameterSample> {
                // A live edit can only sample the kinds a ParameterSample carries; a rational
                // time or a shape path has no scalar/vector/colour/string projection.
                if constexpr (std::is_constructible_v<ParameterSample,
                                                      const std::decay_t<decltype(value)>&>)
                    return ParameterSample(value);
                else
                    return std::nullopt;
            },
            valueEdit_->value);
    if (const auto* constant = std::get_if<document::ConstantValueSource>(&parameter->source)) {
        if (const auto* scalar = std::get_if<double>(&constant->value)) {
            return ParameterSample(*scalar);
        }
        if (const auto* vector = std::get_if<document::Vec2d>(&constant->value)) {
            return ParameterSample(*vector);
        }
        if (const auto* vector = std::get_if<document::Vec3d>(&constant->value))
            return ParameterSample(*vector);
        if (const auto* color = std::get_if<core::Color4d>(&constant->value)) {
            return ParameterSample(*color);
        }
        // Task DRIVE-1: the three kinds that have no curve. They are still VALUES a row has to
        // show, and since a driver can now vary any of them, "what is this parameter right now" is
        // one question with one answer for every kind rather than four kinds with a reader and
        // three without. A Boolean is matched before an Integer because the document stores each
        // in its own alternative, and neither is a projection of the other.
        if (const auto* text = std::get_if<std::string>(&constant->value)) {
            return ParameterSample(*text);
        }
        if (const auto* flag = std::get_if<bool>(&constant->value)) {
            return ParameterSample(*flag);
        }
        if (const auto* integer = std::get_if<std::int64_t>(&constant->value)) {
            return ParameterSample(*integer);
        }
        // A RationalTime is an instant rather than a quantity, and no row reads one through here.
        return std::nullopt;
    }
    // An animated source is sampled at the CURRENT session time; a driven one is produced by the
    // value graph rather than read off the record, so it answers nothing here and
    // drivenValueText() answers instead -- exactly as every write path refuses a driven source.
    return sampleParameterValue(*parameter, currentTime_);
}

namespace {

template <typename Value>
[[nodiscard]] std::optional<Value> typedSample(const std::optional<ParameterSample>& sample) {
    if (!sample.has_value()) {
        return std::nullopt;
    }
    const auto* value = std::get_if<Value>(&*sample);
    return value == nullptr ? std::nullopt : std::optional<Value>(*value);
}

} // namespace

std::optional<double> CompositionSession::effectiveScalarValue(const std::string_view role) const {
    return typedSample<double>(effectiveParameterValue(parameterForSelection(role)));
}

std::optional<document::Vec2d>
CompositionSession::effectiveVec2Value(const std::string_view role) const {
    return typedSample<document::Vec2d>(effectiveParameterValue(parameterForSelection(role)));
}

std::optional<core::Color4d>
CompositionSession::effectiveColorValue(const std::string_view role) const {
    return typedSample<core::Color4d>(effectiveParameterValue(parameterForSelection(role)));
}

std::optional<double>
CompositionSession::effectiveScalarValue(const document::ParameterId parameterId) const {
    const auto* current = composition();
    return typedSample<double>(effectiveParameterValue(
        current == nullptr ? nullptr : current->parameters().find(parameterId)));
}

std::optional<document::Vec2d>
CompositionSession::effectiveVec2Value(const document::ParameterId parameterId) const {
    const auto* current = composition();
    return typedSample<document::Vec2d>(effectiveParameterValue(
        current == nullptr ? nullptr : current->parameters().find(parameterId)));
}

std::optional<core::Color4d>
CompositionSession::effectiveColorValue(const document::ParameterId parameterId) const {
    const auto* current = composition();
    return typedSample<core::Color4d>(effectiveParameterValue(
        current == nullptr ? nullptr : current->parameters().find(parameterId)));
}

std::optional<document::KeyframeId>
CompositionSession::keyframeAtExactTime(const document::ParameterRecord& parameter,
                                        const core::RationalTime time) const {
    const auto* current = composition();
    const auto* source = std::get_if<document::AnimationCurveSource>(&parameter.source);
    const auto* record = current == nullptr || source == nullptr
                             ? nullptr
                             : current->animationCurves().find(source->curveId);
    if (record == nullptr) {
        return std::nullopt;
    }
    return std::visit(
        [time](const auto& curve) -> std::optional<document::KeyframeId> {
            using Curve = std::decay_t<decltype(curve)>;
            if constexpr (std::is_same_v<Curve, document::ScalarAnimationCurve>) {
                for (const auto& key : curve.keyframes)
                    if (key.time == time)
                        return key.id;
            } else {
                for (const auto& component : curve.components)
                    for (const auto& key : component.keyframes)
                        if (key.time == time)
                            return key.id;
            }
            return std::nullopt;
        },
        *record);
}

std::size_t CompositionSession::keyframeCount(const document::AnimationCurveId curveId) const {
    const auto* current = composition();
    const auto* record = current == nullptr ? nullptr : current->animationCurves().find(curveId);
    if (record == nullptr) {
        return 0;
    }
    return std::visit(
        [](const auto& curve) {
            using Curve = std::decay_t<decltype(curve)>;
            if constexpr (std::is_same_v<Curve, document::ScalarAnimationCurve>) {
                return curve.keyframes.size();
            } else {
                std::size_t count = 0;
                for (const auto& component : curve.components)
                    count += component.keyframes.size();
                return count;
            }
        },
        *record);
}

KeyframeDiamondState CompositionSession::keyframeDiamondState(const std::string_view role) const {
    return diamondStateFor(parameterForSelection(role));
}

KeyframeParameterState
CompositionSession::keyframeParameterState(const std::string_view role) const {
    const auto* parameter = parameterForSelection(role);
    return parameter ? keyframeParameterState(parameter->id, currentTime_)
                     : KeyframeParameterState::Unsupported;
}

KeyframeDiamondState
CompositionSession::keyframeDiamondState(const std::string_view role,
                                         const document::AnimationComponent component) const {
    const auto* parameter = parameterForSelection(role);
    return parameter ? keyframeDiamondState(parameter->id, component, currentTime_)
                     : KeyframeDiamondState::Unsupported;
}

bool CompositionSession::toggleKeyframe(const std::string_view role,
                                        const document::AnimationComponent component) {
    const auto* parameter = parameterForSelection(role);
    return parameter && toggleKeyframe(parameter->id, component, currentTime_);
}

KeyframeDiamondState CompositionSession::keyframeDiamondStateForParameter(
    const document::ParameterId parameterId) const {
    const auto* current = composition();
    return diamondStateFor(current == nullptr ? nullptr : current->parameters().find(parameterId));
}

KeyframeDiamondState
CompositionSession::keyframeDiamondState(const document::ParameterId parameterId,
                                         const document::AnimationComponent component,
                                         const core::RationalTime time) const {
    const auto* current = composition();
    const auto* parameter = current == nullptr ? nullptr : current->parameters().find(parameterId);
    if (parameter == nullptr || !document::isAnimatableSchemaKey(parameter->schemaKey))
        return KeyframeDiamondState::Unsupported;
    if (std::holds_alternative<document::ConstantValueSource>(parameter->source))
        return KeyframeDiamondState::Constant;
    const auto* source = std::get_if<document::AnimationCurveSource>(&parameter->source);
    if (source == nullptr || current == nullptr)
        return KeyframeDiamondState::Unsupported;
    const auto* values = current->animationCurves().findComponent(source->curveId, component);
    if (values == nullptr)
        return KeyframeDiamondState::Unsupported;
    return std::ranges::any_of(values->keyframes,
                               [time](const auto& key) { return key.time == time; })
               ? KeyframeDiamondState::AnimatedWithKey
               : KeyframeDiamondState::AnimatedWithoutKey;
}

KeyframeParameterState
CompositionSession::keyframeParameterState(const document::ParameterId parameterId,
                                           const core::RationalTime time) const {
    const auto* current = composition();
    const auto* parameter = current == nullptr ? nullptr : current->parameters().find(parameterId);
    if (parameter == nullptr || !document::isAnimatableSchemaKey(parameter->schemaKey))
        return KeyframeParameterState::Unsupported;
    if (std::holds_alternative<document::ConstantValueSource>(parameter->source))
        return KeyframeParameterState::None;
    const auto* source = std::get_if<document::AnimationCurveSource>(&parameter->source);
    if (source == nullptr || current == nullptr)
        return KeyframeParameterState::Unsupported;
    const auto* record = current->animationCurves().find(source->curveId);
    if (record == nullptr)
        return KeyframeParameterState::Unsupported;
    return std::visit(
        [time](const auto& curve) {
            using Curve = std::decay_t<decltype(curve)>;
            if constexpr (std::is_same_v<Curve, document::ScalarAnimationCurve>) {
                return std::ranges::any_of(curve.keyframes,
                                           [time](const auto& key) { return key.time == time; })
                           ? KeyframeParameterState::All
                           : KeyframeParameterState::None;
            } else {
                std::size_t keyed = 0;
                for (const auto& component : curve.components)
                    if (std::ranges::any_of(component.keyframes,
                                            [time](const auto& key) { return key.time == time; }))
                        ++keyed;
                return keyed == 0                         ? KeyframeParameterState::None
                       : keyed == curve.components.size() ? KeyframeParameterState::All
                                                          : KeyframeParameterState::Some;
            }
        },
        *record);
}

KeyframeDiamondState
CompositionSession::diamondStateFor(const document::ParameterRecord* parameter) const {
    if (parameter == nullptr || !document::isAnimatableSchemaKey(parameter->schemaKey)) {
        return KeyframeDiamondState::Unsupported;
    }
    if (std::holds_alternative<document::ConstantValueSource>(parameter->source)) {
        return KeyframeDiamondState::Constant;
    }
    if (!std::holds_alternative<document::AnimationCurveSource>(parameter->source)) {
        // A driven parameter is not something this gesture can key, and painting it as "animated"
        // would promise a click that does nothing.
        return KeyframeDiamondState::Unsupported;
    }
    return keyframeAtExactTime(*parameter, currentTime_).has_value()
               ? KeyframeDiamondState::AnimatedWithKey
               : KeyframeDiamondState::AnimatedWithoutKey;
}

bool CompositionSession::toggleKeyframe(const std::string_view role) {
    return toggleKeyframeFor(parameterForSelection(role));
}

bool CompositionSession::toggleKeyframeForParameter(const document::ParameterId parameterId) {
    const auto* current = composition();
    return toggleKeyframeFor(current == nullptr ? nullptr
                                                : current->parameters().find(parameterId));
}

bool CompositionSession::toggleKeyframe(const document::ParameterId parameterId,
                                        const document::AnimationComponent component,
                                        const core::RationalTime time) {
    Q_ASSERT(QThread::currentThread() == thread());
    const auto* current = composition();
    const auto* parameter = current == nullptr ? nullptr : current->parameters().find(parameterId);
    if (parameter == nullptr) {
        reportUnavailable(QStringLiteral("The selected object does not expose this parameter"));
        return false;
    }
    if (!document::isAnimatableSchemaKey(parameter->schemaKey)) {
        reportUnavailable(QStringLiteral("This parameter cannot be animated"));
        return false;
    }
    const auto label = keyframeCommandLabel(parameter->schemaKey);
    if (const auto* constant = std::get_if<document::ConstantValueSource>(&parameter->source)) {
        const bool supported = std::visit(
            [](const auto& value) {
                using Value = std::decay_t<decltype(value)>;
                return std::is_same_v<Value, document::Vec2d> ||
                       std::is_same_v<Value, document::Vec3d> ||
                       std::is_same_v<Value, core::Color4d>;
            },
            constant->value);
        if (!supported) {
            reportUnavailable(QStringLiteral("The parameter value does not match its schema"));
            return false;
        }
        commands::Transaction transaction(label.addKey.toStdString(), snapshot_.revision());
        transaction.emplace<commands::CreateAnimationForParameter>(compositionId_, parameterId,
                                                                   time, component);
        return execute(std::move(transaction));
    }
    const auto* source = std::get_if<document::AnimationCurveSource>(&parameter->source);
    if (source == nullptr) {
        reportUnavailable(
            QStringLiteral("Disconnect the driven parameter before keying its value"));
        return false;
    }
    const auto* componentCurve =
        current->animationCurves().findComponent(source->curveId, component);
    if (componentCurve == nullptr) {
        reportUnavailable(QStringLiteral("This component is not available on the parameter"));
        return false;
    }
    const auto existing =
        std::ranges::find(componentCurve->keyframes, time, &document::ScalarKeyframe::time);
    commands::Transaction transaction(
        (existing == componentCurve->keyframes.end() ? label.addKey : label.removeKey)
            .toStdString(),
        snapshot_.revision());
    if (existing != componentCurve->keyframes.end()) {
        transaction.emplace<commands::DeleteKeyframe>(compositionId_, source->curveId, existing->id,
                                                      component);
        return execute(std::move(transaction));
    }
    const auto sample = sampleParameterValue(*parameter, time);
    if (!sample.has_value()) {
        reportUnavailable(QStringLiteral("The animation curve could not be sampled here"));
        return false;
    }
    std::optional<double> value;
    std::visit(
        [&](const auto& held) {
            using Value = std::decay_t<decltype(held)>;
            if constexpr (std::is_same_v<Value, document::Vec2d>) {
                if (component == document::AnimationComponent::X)
                    value = held.x;
                else if (component == document::AnimationComponent::Y)
                    value = held.y;
            } else if constexpr (std::is_same_v<Value, document::Vec3d>) {
                if (component == document::AnimationComponent::X)
                    value = held.x;
                else if (component == document::AnimationComponent::Y)
                    value = held.y;
                else if (component == document::AnimationComponent::Z)
                    value = held.z;
            } else if constexpr (std::is_same_v<Value, core::Color4d>) {
                if (component == document::AnimationComponent::Red)
                    value = held.red;
                else if (component == document::AnimationComponent::Green)
                    value = held.green;
                else if (component == document::AnimationComponent::Blue)
                    value = held.blue;
                else if (component == document::AnimationComponent::Alpha)
                    value = held.alpha;
            }
        },
        *sample);
    if (!value.has_value()) {
        reportUnavailable(QStringLiteral("This component does not match the parameter value"));
        return false;
    }
    transaction.emplace<commands::SetKeyframeAtTimeForParameterComponent>(
        compositionId_, parameterId, component, time, *value);
    return execute(std::move(transaction));
}

bool CompositionSession::toggleKeyframeFor(const document::ParameterRecord* parameter) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (parameter == nullptr) {
        reportUnavailable(QStringLiteral("The selected object does not expose this parameter"));
        return false;
    }
    if (!document::isAnimatableSchemaKey(parameter->schemaKey)) {
        reportUnavailable(QStringLiteral("This parameter cannot be animated"));
        return false;
    }
    const auto parameterId = parameter->id;
    const auto label = keyframeCommandLabel(parameter->schemaKey);

    // --- constant -> animated, with one key at the current value and time ----------------------
    if (const auto* constant = std::get_if<document::ConstantValueSource>(&parameter->source)) {
        commands::Transaction transaction(label.addKey.toStdString(), snapshot_.revision());
        transaction.emplace<commands::CreateAnimationForParameter>(compositionId_, parameterId,
                                                                   currentTime_);
        // Parameter-keyed, not curve-keyed: the curve this writes into does not exist until the
        // operation above runs against the same draft. Both operations land in ONE transaction, so
        // the whole gesture is one undo step (docs/architecture/animation-and-time.md).
        const bool queued = std::visit(
            [&](const auto& value) {
                using Value = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<Value, double> ||
                              std::is_same_v<Value, document::Vec2d> ||
                              std::is_same_v<Value, document::Vec3d> ||
                              std::is_same_v<Value, core::Color4d>) {
                    transaction.emplace<commands::SetKeyframeAtTimeForParameter>(
                        compositionId_, parameterId, currentTime_, value);
                    return true;
                } else {
                    return false;
                }
            },
            constant->value);
        if (!queued) {
            reportUnavailable(QStringLiteral("The parameter value does not match its schema"));
            return false;
        }
        return execute(std::move(transaction));
    }

    const auto* source = std::get_if<document::AnimationCurveSource>(&parameter->source);
    if (source == nullptr) {
        reportUnavailable(
            QStringLiteral("Disconnect the driven parameter before keying its value"));
        return false;
    }
    const auto curveId = source->curveId;
    const auto existing = keyframeAtExactTime(*parameter, currentTime_);

    // --- animated, no key here -> insert one at the exactly sampled value ----------------------
    if (!existing.has_value() ||
        keyframeParameterState(parameterId, currentTime_) == KeyframeParameterState::Some) {
        const auto sample = sampleParameterValue(*parameter, currentTime_);
        if (!sample.has_value()) {
            reportUnavailable(QStringLiteral("The animation curve could not be sampled here"));
            return false;
        }
        commands::Transaction transaction(label.addKey.toStdString(), snapshot_.revision());
        std::visit(
            [&](const auto& value) {
                if constexpr (interpolable<std::decay_t<decltype(value)>>) {
                    transaction.emplace<commands::SetKeyframeAtTime>(compositionId_, curveId,
                                                                     currentTime_, value);
                }
            },
            *sample);
        return execute(std::move(transaction));
    }

    // --- animated, key here -> delete it; the LAST key converts back to a constant -------------
    commands::Transaction transaction(label.removeKey.toStdString(), snapshot_.revision());
    if (keyframeCount(curveId) > 1) {
        transaction.emplace<commands::DeleteKeyframe>(compositionId_, curveId, *existing);
        return execute(std::move(transaction));
    }
    // The curve's final key IS the parameter's whole animation, so removing it is the
    // animation-to-constant transition, holding exactly the value that key carried.
    const auto sample = sampleParameterValue(*parameter, currentTime_);
    if (!sample.has_value()) {
        reportUnavailable(QStringLiteral("The animation curve could not be sampled here"));
        return false;
    }
    std::visit(
        [&](const auto& value) {
            if constexpr (interpolable<std::decay_t<decltype(value)>>) {
                transaction.emplace<commands::ConvertAnimationToConstant>(compositionId_,
                                                                          parameterId, value);
            }
        },
        *sample);
    return execute(std::move(transaction));
}

std::optional<document::KeyframeInterpolation>
CompositionSession::selectedKeyframeInterpolation() const {
    const auto* keySelection = std::get_if<KeyframeSelection>(&selection_.primary);
    const auto* current = composition();
    const auto* record = keySelection == nullptr || current == nullptr
                             ? nullptr
                             : current->animationCurves().find(keySelection->curveId);
    if (record == nullptr) {
        return std::nullopt;
    }
    const auto keyframeId = keySelection->keyframeId;
    return std::visit(
        [keyframeId, component = keySelection->component](
            const auto& curve) -> std::optional<document::KeyframeInterpolation> {
            using Curve = std::decay_t<decltype(curve)>;
            const auto interpolationOf =
                [keyframeId](const auto& keys) -> std::optional<document::KeyframeInterpolation> {
                const auto key = std::ranges::find(keys, keyframeId, &document::ScalarKeyframe::id);
                return key == keys.end() ? std::nullopt : std::optional{key->outgoingInterpolation};
            };
            if constexpr (std::is_same_v<Curve, document::ScalarAnimationCurve>) {
                return component.has_value() ? std::nullopt : interpolationOf(curve.keyframes);
            } else if (component.has_value()) {
                const auto* values = curve.component(*component);
                return values == nullptr ? std::nullopt : interpolationOf(values->keyframes);
            } else {
                for (const auto& values : curve.components)
                    if (const auto found = interpolationOf(values.keyframes))
                        return found;
                return std::nullopt;
            }
        },
        *record);
}

bool CompositionSession::selectedKeyframeIsFinal() const {
    const auto* keySelection = std::get_if<KeyframeSelection>(&selection_.primary);
    const auto* current = composition();
    const auto* record = keySelection == nullptr || current == nullptr
                             ? nullptr
                             : current->animationCurves().find(keySelection->curveId);
    if (record == nullptr) {
        return false;
    }
    const auto keyframeId = keySelection->keyframeId;
    return std::visit(
        [keyframeId, component = keySelection->component](const auto& curve) {
            using Curve = std::decay_t<decltype(curve)>;
            const auto endsWithKey = [keyframeId](const auto& keys) {
                return !keys.empty() && keys.back().id == keyframeId;
            };
            if constexpr (std::is_same_v<Curve, document::ScalarAnimationCurve>) {
                return !component.has_value() && endsWithKey(curve.keyframes);
            } else if (component.has_value()) {
                const auto* values = curve.component(*component);
                return values != nullptr && endsWithKey(values->keyframes);
            } else {
                return std::ranges::any_of(curve.components, [&](const auto& values) {
                    return endsWithKey(values.keyframes);
                });
            }
        },
        *record);
}

bool CompositionSession::setSelectedKeyframeInterpolation(
    const document::KeyframeInterpolation interpolation) {
    Q_ASSERT(QThread::currentThread() == thread());
    const auto* keySelection = std::get_if<KeyframeSelection>(&selection_.primary);
    if (keySelection == nullptr) {
        return false;
    }
    commands::Transaction transaction("Set Keyframe Interpolation", snapshot_.revision());
    transaction.emplace<commands::SetKeyframeInterpolation>(compositionId_, keySelection->curveId,
                                                            keySelection->keyframeId, interpolation,
                                                            keySelection->component);
    return execute(std::move(transaction));
}

std::optional<document::LayerId>
CompositionSession::contextualLayerForParameter(const document::ParameterId parameterId) const {
    const auto* current = composition();
    if (current == nullptr) {
        return std::nullopt;
    }
    for (const auto& node : current->graph().nodes()) {
        const auto binding = std::ranges::find_if(node.parameters, [parameterId](const auto& item) {
            return item.parameterId == parameterId;
        });
        if (binding != node.parameters.end()) {
            return layerForNode(node.id);
        }
    }
    return std::nullopt;
}

void CompositionSession::normalizeSelection() {
    const auto before = selectedNodes_;
    const auto oldSelection = selection_;
    std::erase_if(selection_.keyframes,
                  [this](const auto& key) { return !keyframeSelectionExists(key); });
    if (std::holds_alternative<KeyframeSelection>(selection_.primary) &&
        !selectionExists(selection_) && !selection_.keyframes.empty()) {
        const auto key = selection_.keyframes.back();
        selection_.primary = key;
        const auto parameter = parameterForCurve(key.curveId);
        selection_.contextualLayer =
            parameter ? contextualLayerForParameter(*parameter) : std::nullopt;
    }
    const auto* current = composition();
    std::erase_if(selectedNodes_,
                  [&](const auto id) { return !current || !current->graph().findNode(id); });
    if (!selectionExists(selection_)) {
        if (std::holds_alternative<document::NodeId>(selection_.primary) &&
            !selectedNodes_.empty()) {
            const auto primary = *selectedNodes_.begin();
            selection_ = {.primary = primary, .contextualLayer = layerForNode(primary)};
        } else {
            selection_ = {};
            selectedNodes_.clear();
        }
    } else if (const auto* nodeId = std::get_if<document::NodeId>(&selection_.primary)) {
        selectedNodes_.insert(*nodeId);
        selection_.contextualLayer = layerForNode(*nodeId);
    } else if (const auto* blockId =
                   std::get_if<document::DataBlockRecordId>(&selection_.primary)) {
        selectedNodes_ = dataBlockReaders(*blockId);
    }
    if (before != selectedNodes_ || oldSelection != selection_)
        emit selectionChanged();
}

void CompositionSession::reportUnavailable(const QString& message) {
    emit commandRejected(message);
}

void CompositionSession::invalidateTransformInteractionOnStaleRevision() {
    if (transformInteraction_.has_value() &&
        transformInteraction_->baseRevision != snapshot_.revision()) {
        cancelTransformInteraction();
    }
}

} // namespace bloom::ui
