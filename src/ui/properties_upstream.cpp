#include "node_editor_items.hpp"
#include "properties_registry_row.hpp"
#include "properties_sections.hpp"
#include <QLabel>
#include <QVBoxLayout>
#include <algorithm>
#include <array>
#include <bloom/document/project.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/section.hpp>
#include <bloom/ui/node_editor.hpp>
#include <bloom/ui/properties_editor.hpp>
#include <memory>

namespace bloom::ui {
void jumpToPropertiesNode(CompositionSession& session, document::NodeId node, QWidget* panel) {
    session.selectNode(node);
    for (auto* editor : panel->window()->findChildren<NodeGraphEditor*>()) {
        QRectF bounds;
        for (auto* item : editor->graphScene()->selectedItems())
            bounds = bounds.united(item->sceneBoundingRect());
        if (!bounds.isEmpty())
            editor->graphView()->frameRect(bounds);
    }
}

void PropertiesEditor::configureUpstream() {
    const auto* composition = session_.composition();
    const auto* selected = session_.selectedNode();
    // A layer's direct source already has its own section, but its dependencies still count.
    std::optional<document::NodeId> shownSource;
    if (const auto* layer = std::get_if<document::LayerId>(&session_.selection().primary))
        shownSource = session_.directSourceNodeForLayer(*layer);
    // Task DRIVE-1: the walk itself is the session's now, because the timeline makes the same walk
    // from a layer and two surfaces disagreeing about a node's dependencies would be a bug nobody
    // could see. Properties follows input EDGES as well as driver links -- everything the selected
    // node depends on, pixels included.
    std::vector<std::pair<document::NodeId, int>> nodes;
    if (selected && composition) {
        const std::array seeds{selected->id};
        for (const auto& upstream :
             session_.upstreamNodes(seeds, UpstreamTraversal::DriverLinksAndInputEdges))
            if (upstream.id != shownSource)
                nodes.emplace_back(upstream.id, upstream.depth);
    }
    QString signature;
    for (const auto& [id, depth] : nodes) {
        const auto* node = composition->graph().findNode(id);
        signature += QString("%1/%2/%3/%4/%5;")
                         .arg(id.value())
                         .arg(depth)
                         .arg(node_editor::nodeDisplayName(*composition, *node))
                         .arg(QString::fromStdString(node->typeId))
                         .arg(node->schemaVersion);
    }
    if (signature != upstreamSignature_) {
        if (upstreamPanel_) {
            for (auto* section : upstreamPanel_->findChildren<kit::KSection*>())
                std::erase(sections_, section);
            // CRASH-2: cancel any in-flight font-catalogue poll on the rows going down with this
            // panel before it is orphaned for deferred deletion -- see
            // PropertiesRegistryRow::detachFromSession().
            for (auto* row : upstreamRows_)
                row->detachFromSession();
            upstreamPanel_->hide();
            upstreamPanel_->setParent(nullptr);
            upstreamPanel_->deleteLater();
        }
        upstreamRows_.clear();
        upstreamSignature_ = signature;
        upstreamPanel_ = new QWidget(selectionSection_);
        upstreamPanel_->setObjectName("propertiesUpstreamPanel");
        auto* layout = new QVBoxLayout(upstreamPanel_);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(kit::px(kit::Spacing::S));
        auto* selectionLayout = qobject_cast<QVBoxLayout*>(selectionSection_->layout());
        selectionLayout->insertWidget(selectionLayout->count() - 1, upstreamPanel_);
        int more = 0;
        for (const auto& [id, depth] : nodes) {
            if (depth > 3) {
                ++more;
                continue;
            }
            const auto* node = composition->graph().findNode(id);
            auto* section = properties::addSection(
                layout, upstreamPanel_, QString("upstream-%1").arg(id.value()),
                node_editor::nodeDisplayName(*composition, *node));
            section->setProperty("nodeId",
                                 QVariant::fromValue(static_cast<qulonglong>(id.value())));
            adoptSection(section, {});
            auto* jump = new kit::KButton(section);
            jump->setObjectName("propertiesJumpToNode");
            jump->setIconId(kit::IconId::Jump);
            jump->setVariant(kit::KButton::Variant::Ghost);
            jump->setFixedSize(kit::px(kit::Size::ControlCompact),
                               kit::px(kit::Size::ControlCompact));
            jump->setToolTip(tr("Jump to node"));
            jump->setAccessibleName(tr("Jump to node"));
            jump->setProperty("rowLabel", section->title());
            section->addHeaderAction(jump);
            connect(jump, &kit::KButton::clicked, this,
                    [this, id] { jumpToPropertiesNode(session_, id, this); });
            const auto* definition =
                document::builtInNodeDefinitions().find(node->typeId, node->schemaVersion);
            if (!definition)
                continue;
            for (const auto& declared : definition->parameters) {
                if (propertiesRowVisibility(declared.role, declared.schemaKey) ==
                    PropertiesRowVisibility::Hidden)
                    continue;
                const auto found = std::ranges::find(node->parameters, declared.role,
                                                     &document::ParameterBinding::role);
                if (found == node->parameters.end())
                    continue;
                auto* row = new PropertiesRegistryRow(session_, id, found->parameterId, declared,
                                                      section->body());
                section->bodyLayout()->addWidget(row);
                upstreamRows_.push_back(row);
                connect(section, &kit::KSection::resetRequested, row, [row] { row->reset(); });
            }
            if (definition->lowering == document::NodeLoweringKind::Text) {
                auto* font = new kit::KDropdown(section->body());
                font->setObjectName("propertiesUpstreamFont");
                font->addItem(tr("DejaVu Sans"));
                font->setEnabled(false);
                font->setToolTip(tr("The embedded DejaVu Sans face is the only supported font"));
                auto* row = properties::addRow(
                    section->bodyLayout(), section->body(),
                    properties::makeRowLabel(tr("Font"), section->body()), nullptr, font);
                section->bodyLayout()->removeWidget(row);
                section->bodyLayout()->insertWidget(1, row);
            }
        }
        if (more) {
            auto* label = new kit::KLabel(tr("and %1 more upstream").arg(more), upstreamPanel_);
            label->setObjectName("propertiesMoreUpstream");
            layout->addWidget(label);
        }
    }
    for (auto* row : upstreamRows_)
        row->refresh();
}
} // namespace bloom::ui
