#include "node_editor_items.hpp"
#include "properties_registry_row.hpp"
#include "properties_sections.hpp"
#include <QLabel>
#include <QVBoxLayout>
#include <algorithm>
#include <bloom/document/project.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/section.hpp>
#include <bloom/ui/node_editor.hpp>
#include <bloom/ui/properties_editor.hpp>
#include <deque>
#include <set>

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
    std::vector<std::pair<document::NodeId, int>> nodes;
    std::set<document::NodeId> visited;
    std::deque<std::pair<document::NodeId, int>> queue;
    if (selected && composition) {
        visited.insert(selected->id);
        queue.emplace_back(selected->id, 0);
    }
    // A layer's direct source already has its own section, but its dependencies still count.
    std::optional<document::NodeId> shownSource;
    if (const auto* layer = std::get_if<document::LayerId>(&session_.selection().primary))
        shownSource = session_.directSourceNodeForLayer(*layer);
    while (!queue.empty()) {
        const auto [id, depth] = queue.front();
        queue.pop_front();
        const auto* node = composition->graph().findNode(id);
        if (!node)
            continue;
        if (depth > 0 && id != shownSource)
            nodes.emplace_back(id, depth);
        const auto* definition =
            document::builtInNodeDefinitions().find(node->typeId, node->schemaVersion);
        if (definition && (definition->lowering == document::NodeLoweringKind::LayerStack ||
                           definition->lowering == document::NodeLoweringKind::CompositionOutput))
            continue;
        const auto enqueue = [&](document::NodeId upstream) {
            if (visited.insert(upstream).second)
                queue.emplace_back(upstream, depth + 1);
        };
        for (const auto& edge : composition->graph().edges()) {
            const auto* input = std::get_if<document::NodeInputRef>(&edge.destination);
            if (input && input->nodeId == id)
                enqueue(edge.source.nodeId);
        }
        for (const auto& binding : node->parameters) {
            const auto* parameter = composition->parameters().find(binding.parameterId);
            const auto* driver =
                parameter ? std::get_if<document::DriverBindingSource>(&parameter->source)
                          : nullptr;
            if (driver)
                enqueue(driver->sourceNodeId);
        }
    }
    QString signature;
    for (const auto& [id, depth] : nodes) {
        const auto* node = composition->graph().findNode(id);
        signature += QString("%1/%2/%3;")
                         .arg(id.value())
                         .arg(depth)
                         .arg(node_editor::nodeDisplayName(*composition, *node));
    }
    if (signature != upstreamSignature_) {
        if (upstreamPanel_) {
            for (auto* section : upstreamPanel_->findChildren<kit::KSection*>())
                std::erase(sections_, section);
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
            auto* jump = new kit::KButton(section->body());
            jump->setObjectName("propertiesJumpToNode");
            jump->setText(tr("Jump to node"));
            jump->setProperty("rowLabel", section->title());
            section->bodyLayout()->addWidget(jump);
            connect(jump, &kit::KButton::clicked, this,
                    [this, id] { jumpToPropertiesNode(session_, id, this); });
            const auto* definition =
                document::builtInNodeDefinitions().find(node->typeId, node->schemaVersion);
            if (!definition)
                continue;
            for (const auto& declared : definition->parameters) {
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
        }
        if (more) {
            auto* label = new QLabel(tr("and %1 more upstream").arg(more), upstreamPanel_);
            label->setObjectName("propertiesMoreUpstream");
            layout->addWidget(label);
        }
    }
    for (auto* row : upstreamRows_)
        row->refresh();
}
} // namespace bloom::ui
