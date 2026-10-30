#include <bloom/ui/layer_alignment_actions.hpp>

#include <bloom/commands/transaction.hpp>
#include <bloom/host/layer_alignment.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/icons.hpp>
#include <bloom/ui/kit/painting.hpp>
#include <bloom/ui/tool_controller.hpp>

#include <QAction>
#include <QActionGroup>
#include <QMenu>
#include <QStringList>

#include <algorithm>

namespace bloom::ui {

namespace {

using commands::AlignmentTarget;
using commands::LayerArrangement;

constexpr std::array kArrangements{
    LayerArrangement::AlignLeft,           LayerArrangement::AlignHorizontalCentre,
    LayerArrangement::AlignRight,          LayerArrangement::AlignTop,
    LayerArrangement::AlignVerticalMiddle, LayerArrangement::AlignBottom,
    LayerArrangement::DistributeHorizontalCentres, LayerArrangement::DistributeVerticalCentres};
constexpr std::array kTargets{AlignmentTarget::Composition, AlignmentTarget::Selection,
                              AlignmentTarget::KeyObject};

kit::IconId arrangementIcon(const LayerArrangement arrangement) {
    switch (arrangement) {
    case LayerArrangement::AlignLeft:
        return kit::IconId::AlignLayersLeft;
    case LayerArrangement::AlignHorizontalCentre:
        return kit::IconId::AlignLayersHorizontalCentre;
    case LayerArrangement::AlignRight:
        return kit::IconId::AlignLayersRight;
    case LayerArrangement::AlignTop:
        return kit::IconId::AlignLayersTop;
    case LayerArrangement::AlignVerticalMiddle:
        return kit::IconId::AlignLayersVerticalMiddle;
    case LayerArrangement::AlignBottom:
        return kit::IconId::AlignLayersBottom;
    case LayerArrangement::DistributeHorizontalCentres:
        return kit::IconId::DistributeHorizontal;
    case LayerArrangement::DistributeVerticalCentres:
        return kit::IconId::DistributeVertical;
    }
    return kit::IconId::AlignLayersLeft;
}

QString targetName(const AlignmentTarget target) {
    switch (target) {
    case AlignmentTarget::Composition:
        return LayerAlignmentActions::tr("Composition");
    case AlignmentTarget::Selection:
        return LayerAlignmentActions::tr("Selection");
    case AlignmentTarget::KeyObject:
        return LayerAlignmentActions::tr("Key Object");
    }
    return {};
}

QString objectStem(const LayerArrangement arrangement) {
    return QString::fromLatin1(commands::layerArrangementId(arrangement).data(),
                               static_cast<qsizetype>(commands::layerArrangementId(arrangement).size()));
}

} // namespace

QString layerArrangementName(const LayerArrangement arrangement) {
    switch (arrangement) {
    case LayerArrangement::AlignLeft:
        return LayerAlignmentActions::tr("Align Left Edges");
    case LayerArrangement::AlignHorizontalCentre:
        return LayerAlignmentActions::tr("Align Horizontal Centers");
    case LayerArrangement::AlignRight:
        return LayerAlignmentActions::tr("Align Right Edges");
    case LayerArrangement::AlignTop:
        return LayerAlignmentActions::tr("Align Top Edges");
    case LayerArrangement::AlignVerticalMiddle:
        return LayerAlignmentActions::tr("Align Vertical Middles");
    case LayerArrangement::AlignBottom:
        return LayerAlignmentActions::tr("Align Bottom Edges");
    case LayerArrangement::DistributeHorizontalCentres:
        return LayerAlignmentActions::tr("Distribute Horizontally");
    case LayerArrangement::DistributeVerticalCentres:
        return LayerAlignmentActions::tr("Distribute Vertically");
    }
    return {};
}

LayerAlignmentActions::LayerAlignmentActions(CompositionSession& session,
                                             CompositionPreviewController* preview,
                                             QObject* parent)
    : QObject(parent), session_(session), preview_(preview) {
    for (std::size_t index = 0; index < kArrangements.size(); ++index) {
        const auto arrangement = kArrangements[index];
        auto* action = new QAction(kit::icon(arrangementIcon(arrangement), kit::IconRole::Chrome),
                                   layerArrangementName(arrangement), this);
        action->setObjectName(QStringLiteral("alignAction-") + objectStem(arrangement));
        connect(action, &QAction::triggered, this, [this, arrangement] { (void)run(arrangement); });
        actions_[index] = action;
    }
    auto* group = new QActionGroup(this);
    group->setExclusive(true);
    for (std::size_t index = 0; index < kTargets.size(); ++index) {
        const auto target = kTargets[index];
        auto* action = new QAction(targetName(target), this);
        action->setObjectName(QStringLiteral("alignTargetAction-") +
                              QString::fromLatin1(commands::alignmentTargetId(target).data(),
                                                  static_cast<qsizetype>(
                                                      commands::alignmentTargetId(target).size())));
        action->setCheckable(true);
        group->addAction(action);
        connect(action, &QAction::triggered, this,
                [this, target] { session_.tools().setAlignmentTarget(target); });
        targets_[index] = action;
    }
    targets_[2]->setToolTip(tr("Align to the primary selected layer, which stays where it is"));

    const auto refreshLater = [this] { refresh(); };
    connect(&session_, &CompositionSession::selectionChanged, this, refreshLater);
    connect(&session_, &CompositionSession::snapshotChanged, this, refreshLater);
    connect(&session_, &CompositionSession::compositionChanged, this, refreshLater);
    connect(&session_, &CompositionSession::currentTimeChanged, this, refreshLater);
    connect(&session_.tools(), &ToolController::alignmentTargetChanged, this, refreshLater);
    if (preview_ != nullptr)
        connect(preview_, &CompositionPreviewController::stateChanged, this, refreshLater);
    refresh();
}

QAction* LayerAlignmentActions::action(const LayerArrangement arrangement) const {
    return actions_[static_cast<std::size_t>(arrangement)];
}

QAction* LayerAlignmentActions::targetAction(const AlignmentTarget target) const {
    return targets_[static_cast<std::size_t>(target)];
}

QMenu* LayerAlignmentActions::createMenu(QWidget* parent) const {
    auto* menu = kit::makeMenu(tr("Align"), parent);
    menu->setObjectName(QStringLiteral("alignMenu"));
    for (std::size_t index = 0; index < actions_.size(); ++index) {
        if (index == 6)
            menu->addSeparator();
        menu->addAction(actions_[index]);
    }
    menu->addSeparator();
    auto* targets = menu->addMenu(tr("Align To"));
    targets->setObjectName(QStringLiteral("alignToMenu"));
    for (auto* target : targets_)
        targets->addAction(target);
    return menu;
}

std::vector<document::LayerId> LayerAlignmentActions::selectedLayers() const {
    std::vector<document::LayerId> layers;
    const auto* composition = session_.composition();
    if (composition == nullptr)
        return layers;
    std::set<document::LayerId> selected;
    for (const auto node : session_.selectedNodes())
        if (const auto layer = session_.layerForNode(node))
            selected.insert(*layer);
    // Top of the stack first: a stable order, so a distribution's ties always break the same way.
    const auto entries = composition->graph().layerStack().entries();
    for (auto entry = entries.rbegin(); entry != entries.rend(); ++entry)
        if (selected.contains(entry->layerId))
            layers.push_back(entry->layerId);
    return layers;
}

std::optional<document::LayerId> LayerAlignmentActions::keyLayer() const {
    const auto* node = session_.selectedNode();
    return node != nullptr ? session_.layerForNode(node->id) : std::nullopt;
}

QString LayerAlignmentActions::keyLayerName() const {
    const auto* composition = session_.composition();
    const auto key = keyLayer();
    if (composition == nullptr || !key)
        return {};
    const auto* boundary = composition->graph().findLayer(*key);
    return boundary != nullptr ? QString::fromStdString(boundary->name) : QString{};
}

QString LayerAlignmentActions::unavailableReason(const LayerArrangement arrangement) const {
    if (session_.composition() == nullptr)
        return tr("No composition is open");
    const auto layers = selectedLayers();
    if (layers.empty())
        return tr("Select the layers to arrange");
    const auto target = session_.tools().alignmentTarget();
    const bool distributing = commands::isDistribution(arrangement);
    if (distributing && layers.size() < commands::kMinimumDistributedLayers)
        return tr("Select three or more layers to distribute them");
    if (target == AlignmentTarget::KeyObject && (!keyLayer() || layers.size() < 2))
        return tr("Select a key layer and at least one more layer");
    if (target == AlignmentTarget::Selection && !distributing && layers.size() < 2)
        return tr("Select two or more layers, or align to the composition");
    if (preview_ == nullptr)
        return tr("Alignment measures the Viewer's frame, and no Viewer preview is running");
    const auto frame = preview_->state().frame;
    if (frame == nullptr)
        return tr("Waiting for the Viewer to show the current frame");
    const auto& identity = frame->desiredIdentity();
    if (identity.compositionId != session_.compositionId() ||
        identity.sourceRevision != session_.snapshot().revision() ||
        identity.time != session_.currentTime())
        return tr("Waiting for the Viewer to show the current frame");
    return {};
}

void LayerAlignmentActions::refresh() {
    const auto key = keyLayerName();
    for (std::size_t index = 0; index < actions_.size(); ++index) {
        const auto reason = unavailableReason(kArrangements[index]);
        auto* action = actions_[index];
        action->setEnabled(reason.isEmpty());
        // An unavailable action keeps its slot and says why.
        action->setToolTip(reason.isEmpty() ? action->text()
                                            : action->text() + QLatin1Char('\n') + reason);
    }
    const auto target = session_.tools().alignmentTarget();
    for (std::size_t index = 0; index < targets_.size(); ++index)
        targets_[index]->setChecked(kTargets[index] == target);
    targets_[2]->setText(key.isEmpty() ? tr("Key Object") : tr("Key Object (%1)").arg(key));
    emit changed();
}

bool LayerAlignmentActions::run(const LayerArrangement arrangement) {
    if (!unavailableReason(arrangement).isEmpty())
        return false;
    const auto* composition = session_.composition();
    const auto frame = preview_->state().frame;
    const auto time = frame->desiredIdentity().time;
    commands::LayerAlignmentRequest request{
        .composition = session_.compositionId(),
        .time = time,
        .layers = selectedLayers(),
        .arrangement = arrangement,
        .target = session_.tools().alignmentTarget(),
        .keyLayer = keyLayer(),
        .geometry =
            host::layerAlignmentGeometry(*composition, time, preview_->currentLayerBounds(frame))};
    commands::Transaction transaction(layerArrangementName(arrangement).toStdString(),
                                      session_.snapshot().revision());
    transaction.emplace<commands::AlignLayers>(std::move(request));
    const auto result = session_.executeTransaction(std::move(transaction));
    // Skipped layers are never silent: they reach the status line, and a refusal (every layer
    // skipped) already reaches it through the session's rejection signal.
    if (!result.warnings.empty()) {
        QStringList messages;
        for (const auto& warning : result.warnings)
            messages << QString::fromStdString(warning.issue.message);
        emit notice(messages.join(QStringLiteral("; ")));
    }
    return result.status == commands::CommandStatus::Succeeded;
}

} // namespace bloom::ui
