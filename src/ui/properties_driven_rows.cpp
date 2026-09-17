#include "node_editor_items.hpp"
#include "properties_registry_row.hpp"
#include "properties_sections.hpp"
#include <QAction>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QVBoxLayout>
#include <algorithm>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/properties_editor.hpp>
#include <memory>

namespace bloom::ui {
namespace {
struct RowTarget {
    document::NodeId node;
    document::ParameterId parameter;
    document::ParameterDefinition definition;
};
std::optional<RowTarget> targetFor(const CompositionSession& session, document::ParameterId id) {
    const auto* composition = session.composition();
    if (!composition)
        return {};
    for (const auto& node : composition->graph().nodes()) {
        for (const auto& binding : node.parameters) {
            if (binding.parameterId != id)
                continue;
            const auto* definition =
                document::builtInNodeDefinitions().find(node.typeId, node.schemaVersion);
            if (!definition)
                continue;
            const auto declared = std::ranges::find(definition->parameters, binding.role,
                                                    &document::ParameterDefinition::role);
            if (declared != definition->parameters.end())
                return RowTarget{node.id, id, *declared};
        }
    }
    return {};
}
void resetRow(CompositionSession& session, const RowTarget& target) {
    const auto* parameter = session.composition()->parameters().find(target.parameter);
    if (parameter && std::holds_alternative<document::DriverBindingSource>(parameter->source)) {
        // The existing disconnect command restores the registry default in one undo step.
        commands::Transaction transaction("Reset Parameter", session.snapshot().revision());
        transaction.emplace<commands::DisconnectInput>(
            session.compositionId(), document::NodeInputRef{target.node, target.definition.role});
        (void)session.executeNodeTransaction(std::move(transaction));
    } else
        (void)session.setParameterValue(target.parameter, target.definition.defaultValue,
                                        QObject::tr("Reset Parameter"));
}
} // namespace
void resetPropertiesParameter(CompositionSession& session, document::ParameterId parameter) {
    if (const auto target = targetFor(session, parameter))
        resetRow(session, *target);
}
void PropertiesEditor::configureDrivenRows() {
    // Give hand-crafted rows the same parameter identity carried by registry rows.
    const std::array<std::pair<const char*, std::string_view>, 10> handcrafted{
        {{"positionXEditor", document::kPositionParameterRole},
         {"anchorXEditor", document::kAnchorParameterRole},
         {"scaleXEditor", document::kScaleParameterRole},
         {"rotationEditor", document::kRotationParameterRole},
         {"opacityEditor", document::kOpacityParameterRole},
         {"propertiesSolidColorChip", document::kSolidColorParameterRole},
         {"textContentEditor", document::kTextParameterRole},
         {"textSizeEditor", document::kTextSizeParameterRole},
         {"textColorChip", document::kTextColorParameterRole},
         {"blendModeEditor", document::kBlendModeParameterRole}}};
    for (const auto& [name, role] : handcrafted) {
        auto* field = findChild<QWidget*>(name);
        auto* row = field ? field->parentWidget() : nullptr;
        while (row && row->objectName() != "propertiesRow")
            row = row->parentWidget();
        const auto* parameter = session_.parameterForSelection(role);
        if (row)
            row->setProperty("parameterId", QVariant::fromValue(static_cast<qulonglong>(
                                                parameter ? parameter->id.value() : 0)));
    }
    // Task DRIVE-1: the resolution is the SESSION's. This panel used to own an evaluator of its
    // own, which meant the Properties row and the timeline row for one driven parameter were two
    // answers to one question that could differ; now there is one answer and both read it.
    if (!drivenValuesConnected_) {
        drivenValuesConnected_ = true;
        connect(&session_, &CompositionSession::drivenValuesChanged, this, [this] {
            for (auto* label : findChildren<QLabel*>("propertiesDrivenValue")) {
                const auto id =
                    document::ParameterId::fromRaw(label->property("parameterId").toULongLong());
                if (const auto text = session_.drivenValueText(id); !text.isEmpty()) {
                    label->setText(text);
                    label->setToolTip(text);
                }
            }
            for (auto* label : findChildren<QLabel*>()) {
                const auto port = label->property("valueOutputPort").toString();
                if (port.isEmpty())
                    continue;
                const auto node =
                    document::NodeId::fromRaw(label->property("valueOutputNodeId").toULongLong());
                const auto text = session_.valueOutputText(node, port.toStdString());
                label->setText(text.isEmpty() ? tr("Resolving…") : text);
                label->setToolTip(label->text());
            }
        });
    }
    auto rows = findChildren<QWidget*>("propertiesRow");
    rows.append(findChildren<QWidget*>("propertiesRegistryRow"));
    rows.append(findChildren<QWidget*>("mergeInputRow"));
    for (auto* row : rows) {
        // The inner presentation rows of a generic row inherit its outer context menu.
        if (row->parentWidget() && row->parentWidget()->objectName() == "propertiesRegistryRow")
            continue;
        if (!row->property("resetMenuInstalled").toBool()) {
            row->setProperty("resetMenuInstalled", true);
            row->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(
                row, &QWidget::customContextMenuRequested, row, [this, row](const QPoint& point) {
                    auto* menu = kit::makeMenu(row);
                    menu->setObjectName("propertiesRowContextMenu");
                    menu->setAttribute(Qt::WA_DeleteOnClose);
                    auto* action = menu->addAction(tr("Reset to default"));
                    action->setObjectName("propertiesResetToDefault");
                    const auto contextId = row->property("contextParameterId").toULongLong();
                    const auto target = targetFor(
                        session_,
                        document::ParameterId::fromRaw(
                            contextId ? contextId : row->property("parameterId").toULongLong()));
                    row->setProperty("contextParameterId", QVariant{});
                    action->setEnabled(target && !session_.composition()->nodeLocked(target->node));
                    if (target)
                        connect(action, &QAction::triggered, this,
                                [this, target] { resetRow(session_, *target); });
                    menu->popup(row->mapToGlobal(point));
                });
        }
        for (auto* child : row->findChildren<QWidget*>()) {
            if (child->property("rowMenuForwarded").toBool())
                continue;
            child->setProperty("rowMenuForwarded", true);
            child->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(child, &QWidget::customContextMenuRequested, row,
                    [row, child](const QPoint& point) {
                        row->setProperty("contextParameterId", child->property("parameterId"));
                        Q_EMIT row->customContextMenuRequested(
                            row->mapFromGlobal(child->mapToGlobal(point)));
                    });
        }
        const auto id = document::ParameterId::fromRaw(row->property("parameterId").toULongLong());
        const auto* composition = session_.composition();
        const auto* parameter = composition ? composition->parameters().find(id) : nullptr;
        const auto* driver =
            parameter ? std::get_if<document::DriverBindingSource>(&parameter->source) : nullptr;
        auto* display =
            row->findChild<QWidget*>("propertiesDrivenDisplay", Qt::FindDirectChildrenOnly);
        if (!driver && !display)
            continue;
        if (!display) {
            display = new QWidget(row);
            display->setObjectName("propertiesDrivenDisplay");
            auto* layout = new QVBoxLayout(display);
            layout->setContentsMargins(0, 0, 0, 0);
            auto* jump = new kit::KButton(display);
            jump->setObjectName("propertiesDriverLink");
            jump->setVariant(kit::KButton::Variant::Ghost);
            jump->setIconId(kit::IconId::Link);
            jump->setFixedHeight(kit::px(kit::Size::ControlCompact));
            layout->addWidget(jump);
            auto* value = new kit::KLabel(display);
            value->setObjectName("propertiesDrivenValue");
            value->setFont(kit::font(kit::TypeRole::Value));
            value->setTextFormat(Qt::PlainText);
            value->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
            layout->addWidget(value);
            if (auto* horizontal = qobject_cast<QHBoxLayout*>(row->layout()))
                horizontal->insertWidget(horizontal->count() - 1, display, 1);
            else {
                auto* originalDisplay = display;
                display = properties::addRow(
                    qobject_cast<QVBoxLayout*>(row->layout()), row,
                    properties::makeRowLabel(row->property("rowLabel").toString(), row), nullptr,
                    originalDisplay);
                originalDisplay->setObjectName("propertiesDrivenControls");
                display->setObjectName("propertiesDrivenDisplay");
            }
            connect(jump, &kit::KButton::clicked, this, [this, jump] {
                jumpToPropertiesNode(
                    session_, document::NodeId::fromRaw(jump->property("nodeId").toULongLong()),
                    this);
            });
        }
        // Restore only controls this decoration hid. Their enabled state remains session-owned.
        for (auto* child : row->findChildren<QWidget*>(QString{}, Qt::FindDirectChildrenOnly)) {
            if (child == display || child->objectName() == "propertiesRowLabel" ||
                (child->sizePolicy().retainSizeWhenHidden() &&
                 child->width() == kit::px(kit::Size::PropertiesDiamondColumn)))
                continue;
            if (driver) {
                if (!child->isHidden()) {
                    child->setProperty("hiddenByDriver", true);
                    child->hide();
                }
            } else if (child->property("hiddenByDriver").toBool()) {
                child->setProperty("hiddenByDriver", false);
                child->show();
            }
        }
        display->setVisible(driver != nullptr);
        if (driver) {
            display->setEnabled(true);
            auto* jump = display->findChild<kit::KButton*>();
            jump->setText(session_.driverDisplayName(id));
            jump->setToolTip(jump->text());
            jump->setProperty("nodeId", QVariant::fromValue(
                                            static_cast<qulonglong>(driver->sourceNodeId.value())));
            auto* value = display->findChild<QLabel*>("propertiesDrivenValue");
            value->setProperty("parameterId",
                               QVariant::fromValue(static_cast<qulonglong>(id.value())));
            // Whatever the session already knows, so a rebuilt row does not flash back to
            // "Resolving…" for a value that was resolved long ago. A String shows the resolved
            // text exactly as every other kind shows its resolved value: read-only, because the
            // graph owns it.
            const auto resolved = session_.drivenValueText(id);
            value->setText(resolved.isEmpty() ? tr("Resolving…") : resolved);
            value->setToolTip(value->text());
        }
    }
    session_.refreshDrivenValues();
}
} // namespace bloom::ui
