#include "node_editor_items.hpp"
#include "properties_driven_values.hpp"
#include "properties_registry_row.hpp"
#include "properties_sections.hpp"
#include <QAction>
#include <QLabel>
#include <QMenu>
#include <QVBoxLayout>
#include <algorithm>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/properties_editor.hpp>

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
         {"solidColorRedEditor", document::kSolidColorParameterRole},
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
    if (!drivenValues_) {
        drivenValues_ = new PropertiesDrivenValues(session_, this);
        drivenValues_->ready = [this](const PropertiesDrivenValues::Values& values) {
            for (auto* label : findChildren<QLabel*>("propertiesDrivenValue")) {
                const auto id =
                    document::ParameterId::fromRaw(label->property("parameterId").toULongLong());
                if (const auto found = values.find(id); found != values.end()) {
                    label->setText(found->second);
                    label->setToolTip(found->second);
                }
            }
        };
    }
    auto rows = findChildren<QWidget*>("propertiesRow");
    rows.append(findChildren<QWidget*>("propertiesRegistryRow"));
    rows.append(findChildren<QWidget*>("mergeInputRow"));
    std::vector<document::ParameterId> driven;
    for (auto* row : rows) {
        // The inner presentation rows of a generic row inherit its outer context menu.
        if (row->parentWidget() && row->parentWidget()->objectName() == "propertiesRegistryRow")
            continue;
        if (!row->property("resetMenuInstalled").toBool()) {
            row->setProperty("resetMenuInstalled", true);
            row->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(
                row, &QWidget::customContextMenuRequested, row, [this, row](const QPoint& point) {
                    auto* menu = new QMenu(row);
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
            if (row->objectName() == "propertiesRegistryRow") {
                auto* label =
                    properties::makeRowLabel(row->property("rowLabel").toString(), display);
                label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
                layout->addWidget(label);
            }
            auto* jump = new kit::KButton(display);
            jump->setObjectName("propertiesDriverLink");
            jump->setVariant(kit::KButton::Variant::Ghost);
            jump->setIconId(kit::IconId::Link);
            layout->addWidget(jump);
            auto* value = new QLabel(display);
            value->setObjectName("propertiesDrivenValue");
            value->setTextFormat(Qt::PlainText);
            value->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
            layout->addWidget(value);
            row->layout()->addWidget(display);
            connect(jump, &kit::KButton::clicked, this, [this, jump] {
                jumpToPropertiesNode(
                    session_, document::NodeId::fromRaw(jump->property("nodeId").toULongLong()),
                    this);
            });
        }
        // Restore only controls this decoration hid. Their enabled state remains session-owned.
        for (auto* child : row->findChildren<QWidget*>(QString{}, Qt::FindDirectChildrenOnly)) {
            if (child == display || child->objectName() == "propertiesRowLabel")
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
            driven.push_back(id);
            display->setEnabled(true);
            auto* jump = display->findChild<kit::KButton*>();
            const auto* node = composition->graph().findNode(driver->sourceNodeId);
            jump->setText(node ? node_editor::nodeDisplayName(*composition, *node)
                               : tr("Missing driver"));
            jump->setToolTip(jump->text());
            jump->setProperty("nodeId", QVariant::fromValue(
                                            static_cast<qulonglong>(driver->sourceNodeId.value())));
            auto* value = display->findChild<QLabel*>("propertiesDrivenValue");
            value->setProperty("parameterId",
                               QVariant::fromValue(static_cast<qulonglong>(id.value())));
            value->setText(tr("Resolving…"));
        }
    }
    drivenValues_->request(std::move(driven));
}
} // namespace bloom::ui
