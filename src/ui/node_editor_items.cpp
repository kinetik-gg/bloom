#include "node_editor_items.hpp"
#include <QCoreApplication>
#include <QPainterPathStroker>
#include <QShortcut>
#include <bloom/commands/node_operations.hpp>

namespace bloom::ui {
kit::Color socketColorToken(const runtime::SocketValueKind kind) noexcept {
    switch (kind) {
    case runtime::SocketValueKind::Image:
        return kit::Color::DataImage;
    case runtime::SocketValueKind::Color:
        return kit::Color::DataSequence;
    case runtime::SocketValueKind::Scalar:
        return kit::Color::Muted;
    case runtime::SocketValueKind::Vector2:
        return kit::Color::DataComposition;
    case runtime::SocketValueKind::String:
        return kit::Color::DataAudio;
    }
    return kit::Color::DataImage;
}

namespace node_editor {
QString nodeCategoryName(const document::NodeCategory category) {
    switch (category) {
    case document::NodeCategory::Sources:
        return QCoreApplication::translate("node_editor", "Sources");
    case document::NodeCategory::Layers:
        return QCoreApplication::translate("node_editor", "Layers");
    case document::NodeCategory::Compositing:
        return QCoreApplication::translate("node_editor", "Compositing");
    case document::NodeCategory::Values:
        return QCoreApplication::translate("node_editor", "Values");
    case document::NodeCategory::Output:
        return QCoreApplication::translate("node_editor", "Output");
    case document::NodeCategory::Utilities:
        return QCoreApplication::translate("node_editor", "Utilities");
    }
    return {};
}

std::span<const document::NodeCategory> nodeCategoryOrder() {
    static constexpr std::array kOrder{
        document::NodeCategory::Sources,     document::NodeCategory::Layers,
        document::NodeCategory::Compositing, document::NodeCategory::Values,
        document::NodeCategory::Output,      document::NodeCategory::Utilities};
    return kOrder;
}

QString displayTypeName(const std::string_view typeId) {
    QString name = QString::fromUtf8(typeId.data(), static_cast<qsizetype>(typeId.size()));
    if (name.startsWith(QStringLiteral("bloom."))) {
        name.remove(0, 6);
    }
    name.replace('-', ' ');
    if (!name.isEmpty()) {
        name[0] = name[0].toUpper();
    }
    return name;
}

// A layer boundary node's display name is the layer's own durable name (the exact string the
// timeline row shows); every other node is named by its type. Both are the node's real name, read
// from document truth -- neither is invented here.
QString nodeDisplayName(const document::Composition& composition,
                        const document::NodeRecord& node) {
    for (const auto& boundary : composition.graph().layerOutputs()) {
        if (boundary.nodeId == node.id && !boundary.name.empty()) {
            return QString::fromStdString(boundary.name);
        }
    }
    return displayTypeName(node.typeId);
}

const document::ParameterRecord* parameterForRole(const document::NodeRecord& node,
                                                  const document::Composition& composition,
                                                  const std::string_view role) {
    const auto binding = std::ranges::find_if(
        node.parameters, [role](const auto& candidate) { return candidate.role == role; });
    return binding == node.parameters.end() ? nullptr
                                            : composition.parameters().find(binding->parameterId);
}

// The read-only rendering for a bound parameter that has no editable kit primitive behind it (see
// NodeItem::ensureFields()). Unchanged from the pre-U4 projection.
QString parameterText(const document::ParameterRecord& parameter) {
    const auto* constant = std::get_if<document::ConstantValueSource>(&parameter.source);
    if (constant == nullptr) {
        return parameterSourceDescription(parameter);
    }

    return std::visit(
        [](const auto& value) -> QString {
            using Value = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Value, bool>) {
                return value ? QStringLiteral("On") : QStringLiteral("Off");
            } else if constexpr (std::is_same_v<Value, std::int64_t>) {
                return QString::number(value);
            } else if constexpr (std::is_same_v<Value, double>) {
                return QString::number(value, 'f', 2);
            } else if constexpr (std::is_same_v<Value, document::Vec2d>) {
                return QStringLiteral("%1, %2").arg(value.x, 0, 'f', 1).arg(value.y, 0, 'f', 1);
            } else if constexpr (std::is_same_v<Value, core::Color4d>) {
                return exactColorText(value);
            } else if constexpr (std::is_same_v<Value, std::string>) {
                return QString::fromStdString(value).left(24);
            } else {
                return QStringLiteral("%1/%2").arg(value.numerator()).arg(value.denominator());
            }
        },
        constant->value);
}

document::NodeId destinationNodeId(const document::InputPortRef& input) {
    return std::visit(
        [](const auto& destination) {
            using Destination = std::decay_t<decltype(destination)>;
            if constexpr (std::is_same_v<Destination, document::NodeInputRef>) {
                return destination.nodeId;
            } else {
                return destination.stackNodeId;
            }
        },
        input);
}

// A kit numeric cell sized for a node card: no internal label column (KValueField's own is a fixed
// 72px meant for a Properties-style row), because the card paints the row's name itself in a column
// shared by every row.
kit::KValueField* makeCardField(const QString& objectName, const QString& accessibleName,
                                const double minimum, const double maximum, const int decimals,
                                const QString& unit) {
    auto* field = new kit::KValueField;
    field->setObjectName(objectName);
    field->setAccessibleName(accessibleName);
    field->setRange(minimum, maximum);
    field->setDecimals(decimals);
    field->setSingleStep(1.0);
    field->setUnit(unit);
    field->resize(field->sizeHint());
    return field;
}

void NodeItem::paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget*) {
    const QRectF bounds = cardRect();
    const auto radiusToken = layout_.collapsed ? kit::Radius::Full : kCardRadius;
    const bool selected = option->state.testFlag(QStyle::State_Selected);
    painter->setRenderHint(QPainter::Antialiasing, true);

    // SurfaceRaised card + hairline Border, then the Accent inset edge when selected.
    painter->setOpacity(layout_.muted ? 0.5 : 1.0);
    kit::fillRoundedSurface(*painter, bounds, kit::color(kit::Color::SurfaceRaised),
                            kit::color(kit::Color::Border), radiusToken);
    painter->setOpacity(1.0);
    const auto selectionOutline = [&] {
        if (!selected)
            return;
        painter->setOpacity(1.0);
        const qreal inset = kSelectionEdgeWidth / 2.0;
        painter->setBrush(Qt::NoBrush);
        painter->setPen(QPen(kit::color(primary_ ? kit::Color::Foreground : kit::Color::Accent),
                             kSelectionEdgeWidth));
        const auto radius = static_cast<qreal>(kit::radiusPx(
            radiusToken, static_cast<int>(std::min(bounds.width(), bounds.height()))));
        painter->drawRoundedRect(bounds.adjusted(inset, inset, -inset, -inset), radius, radius);
    };

    // The header strip: one step down the surface ladder from the card so it reads as chrome, with
    // its own hairline foot rather than a second rounded rectangle.
    QPainterPath header;
    // Winding, not QPainterPath's default odd-even rule: the header is a rounded rectangle UNION a
    // square strip that squares off its bottom corners, and the two subpaths overlap. Under
    // odd-even the overlap cancels and the strip is left unpainted -- a dark band across the bottom
    // of every header, which is exactly what the pre-U4 projection drew.
    header.setFillRule(Qt::WindingFill);
    const auto headerRadius = static_cast<qreal>(
        kit::radiusPx(radiusToken, static_cast<int>(std::min(bounds.width(), kCardHeaderHeight))));
    header.addRoundedRect(QRectF(0.0, 0.0, bounds.width(), kCardHeaderHeight), headerRadius,
                          headerRadius);
    if (!layout_.collapsed)
        header.addRect(QRectF(0.0, kCardHeaderHeight - headerRadius, bounds.width(), headerRadius));
    painter->setPen(Qt::NoPen);
    painter->fillPath(header, kit::color(kit::Color::Surface));
    kit::applyHairlinePen(*painter, kit::color(kit::Color::Border));
    if (!layout_.collapsed)
        painter->drawLine(QPointF(0.0, kCardHeaderHeight),
                          QPointF(bounds.width(), kCardHeaderHeight));

    painter->setFont(kit::font(kit::TypeRole::UiSmall));
    painter->setPen(kit::color(kit::Color::Foreground));
    const QRectF titleRect(kCardPadding, 0.0,
                           bounds.width() - 2.0 * kCardPadding -
                               (layout_.muted ? kit::px(kit::Size::IconSmall) + kCardPadding : 0.0),
                           kCardHeaderHeight);
    painter->drawText(
        titleRect, Qt::AlignVCenter | Qt::AlignLeft,
        QFontMetricsF(painter->font()).elidedText(title_, Qt::ElideRight, titleRect.width()));

    if (layout_.muted) {
        const auto badge =
            kit::iconPixmap(kit::IconId::Hidden, kit::Size::IconSmall, kit::Color::Foreground);
        painter->drawPixmap(
            QPointF(bounds.width() - kCardPadding - badge.width() / badge.devicePixelRatio(),
                    (kCardHeaderHeight - badge.height() / badge.devicePixelRatio()) / 2.0),
            badge);
    }
    if (layout_.collapsed) {
        selectionOutline();
        return;
    }
    painter->setOpacity(layout_.muted ? 0.5 : 1.0);
    painter->setFont(kit::font(kit::TypeRole::UiSmall));
    painter->setPen(kit::color(kit::Color::Muted));
    for (const auto* socket : sockets_) {
        const qreal rowExtent = socket->rowHeight();
        const QRectF row(kCardPadding, socket->pos().y() - rowExtent / 2,
                         std::max(0.0, width_ - 2 * kCardPadding), rowExtent);
        painter->drawText(
            row,
            static_cast<int>(Qt::AlignVCenter | (socket->input ? Qt::AlignLeft : Qt::AlignRight)),
            painter->fontMetrics().elidedText(socket->name, Qt::ElideRight,
                                              static_cast<int>(row.width())));
    }

    // Row labels. The controls themselves are real kit widgets in proxies; only their names are
    // painted here, in the shared right-aligned label column.
    painter->setFont(kit::font(kit::TypeRole::UiSmall));
    qreal y = kCardHeaderHeight + static_cast<qreal>(sockets_.size()) * kSocketRowHeight;
    const QRectF labelColumn(kCardPadding, 0.0, labelColumnWidth_, rowHeight_);
    const auto drawLabel = [&](const QString& text, const qreal top) {
        painter->setPen(kit::color(kit::Color::Muted));
        painter->drawText(labelColumn.translated(0.0, top), Qt::AlignVCenter | Qt::AlignRight,
                          text);
    };
    for (const auto& row : valueRows_) {
        drawLabel(row.label, y);
        y += rowHeight_ + kCardRowGap;
    }
    if (colorChip_ != nullptr) {
        drawLabel(colorRowLabel_, y);
        y += rowHeight_ + kCardRowGap;
    }
    for (const auto& [label, value] : readOnlyRows_) {
        drawLabel(label, y);
        painter->setFont(kit::font(kit::TypeRole::Value));
        painter->setPen(kit::color(kit::Color::Foreground));
        const QRectF valueRect(
            kCardPadding + labelColumnWidth_ + kCardLabelGap, y,
            bounds.width() - kCardPadding * 2.0 - labelColumnWidth_ - kCardLabelGap, rowHeight_);
        painter->drawText(
            valueRect, Qt::AlignVCenter | Qt::AlignLeft,
            QFontMetricsF(painter->font()).elidedText(value, Qt::ElideRight, valueRect.width()));
        painter->setFont(kit::font(kit::TypeRole::UiSmall));
        y += rowHeight_ + kCardRowGap;
    }
    selectionOutline();
}

QVariant NodeItem::itemChange(const GraphicsItemChange change, const QVariant& value) {
    if (change == ItemPositionHasChanged || change == ItemSelectedHasChanged) {
        for (auto* edge : edges_) {
            if (change == ItemPositionHasChanged) {
                edge->updatePath();
            }
            edge->update();
        }
    }
    return QGraphicsObject::itemChange(change, value);
}

NodeItem* nodeItemAncestor(QGraphicsItem* item) {
    for (auto* candidate = item; candidate != nullptr; candidate = candidate->parentItem()) {
        if (auto* node = dynamic_cast<NodeItem*>(candidate)) {
            return node;
        }
    }
    return nullptr;
}

NodeItem* firstNodeItem(const QList<QGraphicsItem*>& items) {
    for (auto* item : items) {
        if (auto* node = nodeItemAncestor(item)) {
            return node;
        }
    }
    return nullptr;
}

} // namespace node_editor
} // namespace bloom::ui

namespace bloom::ui::node_editor {
QString socketKindName(const document::SocketValueKind kind) {
    switch (kind) {
    case document::SocketValueKind::Image:
        return QStringLiteral("Image");
    case document::SocketValueKind::Color:
        return QStringLiteral("Color");
    case document::SocketValueKind::Scalar:
        return QStringLiteral("Scalar");
    case document::SocketValueKind::Vector2:
        return QStringLiteral("Vector2");
    case document::SocketValueKind::String:
        return QStringLiteral("String");
    }
    return {};
}

QPainterPath linkPath(const QPointF start, const QPointF end) {
    const qreal handle = std::max(64.0, std::abs(end.x() - start.x()) / 2.0);
    QPainterPath path(start);
    path.cubicTo(start + QPointF(handle, 0), end - QPointF(handle, 0), end);
    return path;
}

SocketItem::SocketItem(const document::NodeId node, QString portName,
                       const document::SocketValueKind valueKind,
                       std::optional<document::InputPortRef> inputRef,
                       std::optional<document::OutputPortRef> outputRef, const bool structural,
                       QGraphicsItem* parent)
    : QGraphicsItem(parent), name(std::move(portName)), kind(valueKind), input(std::move(inputRef)),
      output(std::move(outputRef)), structural_(structural) {
    setData(kNodeItemKindRole, QStringLiteral("socket"));
    setData(kNodeStableIdRole, QVariant::fromValue<qulonglong>(node.value()));
    setData(kNodeSocketNameRole, name);
    setData(kNodeSocketInputRole, input.has_value());
    setAcceptHoverEvents(true);
    setAcceptedMouseButtons(Qt::LeftButton);
    setZValue(2);
    setCursor(draggable() ? Qt::CrossCursor : Qt::ForbiddenCursor);
    QString tip = name + QStringLiteral(" · ") + socketKindName(kind);
    if (structural)
        tip += QStringLiteral("\nStructural Layer Output / stack-slot boundary; remove the layer "
                              "to remove this connection");
    else if (kind != document::SocketValueKind::Image)
        tip += QStringLiteral("\nOnly Image ports are linkable in this editor");
    description_ = tip;
    setAuthoringEnabled(true);
}

qreal SocketItem::rowHeight() const { return kSocketRowHeight; }

QPainterPath SocketItem::shape() const {
    QPainterPath hit;
    hit.addEllipse(boundingRect());
    return hit;
}

void SocketItem::paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*) {
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setPen(QPen(kit::color(kit::Color::Surface), kit::kHairlineWidth));
    painter->setBrush(kit::color(socketColorToken(kind)));
    const qreal radius = hovered_ ? 6.0 : kSocketDiameter / 2;
    painter->drawEllipse(QPointF(), radius, radius);
}
void SocketItem::hoverEnterEvent(QGraphicsSceneHoverEvent* event) {
    hovered_ = true;
    setData(kNodeHoveredRole, true);
    update();
    QGraphicsItem::hoverEnterEvent(event);
}
void SocketItem::hoverLeaveEvent(QGraphicsSceneHoverEvent* event) {
    hovered_ = false;
    setData(kNodeHoveredRole, false);
    update();
    QGraphicsItem::hoverLeaveEvent(event);
}

void NodeItem::buildSockets(const document::NodeRecord& node,
                            const document::Composition& composition,
                            const document::NodeDefinitionRegistry& registry) {
    for (auto* socket : sockets_)
        delete socket;
    sockets_.clear();
    linkedInputs_.clear();
    const auto* definition = registry.find(node.typeId, node.schemaVersion);
    if (!definition)
        return;
    const bool boundary =
        std::ranges::any_of(composition.graph().layerOutputs(),
                            [&](const auto& layer) { return layer.nodeId == node.id; });
    for (const auto& port : definition->inputs) {
        document::InputPortRef input = document::NodeInputRef{node.id, port.name};
        if (std::ranges::any_of(composition.graph().edges(),
                                [&](const auto& edge) { return edge.destination == input; }))
            linkedInputs_.insert(QString::fromStdString(port.name));
        sockets_.push_back(new SocketItem(node.id, QString::fromStdString(port.name),
                                          port.valueKind, input, std::nullopt, false, this));
    }
    if (definition->layerSlotInput && node.id == composition.graph().layerStack().nodeId()) {
        for (const auto& slot : composition.graph().layerStack().entries()) {
            const auto& port = *definition->layerSlotInput;
            sockets_.push_back(
                new SocketItem(node.id, QString::fromStdString(port.role), port.valueKind,
                               document::LayerStackInputRef{node.id, slot.slotId, port.role},
                               std::nullopt, true, this));
        }
    }
    for (const auto& port : definition->outputs)
        sockets_.push_back(
            new SocketItem(node.id, QString::fromStdString(port.name), port.valueKind, std::nullopt,
                           document::OutputPortRef{node.id, port.name}, boundary, this));
}

NodeEdgeItem::NodeEdgeItem(NodeItem& source, NodeItem& destination, SocketItem& output,
                           SocketItem& input, document::EdgeRecord record, const bool isStructural)
    : edge(std::move(record)), structural(isStructural), source_(source), destination_(destination),
      output_(output), input_(input) {
    setData(kNodeItemKindRole, QStringLiteral("edge"));
    setData(kNodeStableIdRole, QVariant::fromValue<qulonglong>(edge.id.value()));
    setData(kNodeStructuralRole, structural);
    setAcceptHoverEvents(true);
    setAcceptedMouseButtons(Qt::NoButton);
    setZValue(-1);
    if (structural)
        setToolTip(output.draggable() ? input.toolTip() : output.toolTip());
    source_.addEdge(*this);
    destination_.addEdge(*this);
    updatePath();
}
void NodeEdgeItem::updatePath() { setPath(linkPath(output_.scenePos(), input_.scenePos())); }
QPainterPath NodeEdgeItem::shape() const {
    QPainterPathStroker stroke;
    stroke.setWidth(12);
    return stroke.createStroke(path());
}
void NodeEdgeItem::paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*) {
    const bool emphasized = hovered_ || source_.isSelected() || destination_.isSelected();
    const QColor base = kit::color(socketColorToken(output_.kind));
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setBrush(Qt::NoBrush);
    painter->setPen(QPen(emphasized ? kit::hoverFillFor(base) : base,
                         emphasized ? 2.0 : kit::kHairlineWidth, Qt::SolidLine, Qt::RoundCap));
    painter->drawPath(path());
}
void NodeEdgeItem::hoverEnterEvent(QGraphicsSceneHoverEvent* event) {
    emphasize(true);
    setData(kNodeHoveredRole, true);
    QGraphicsPathItem::hoverEnterEvent(event);
}
void NodeEdgeItem::hoverLeaveEvent(QGraphicsSceneHoverEvent* event) {
    emphasize(false);
    setData(kNodeHoveredRole, false);
    QGraphicsPathItem::hoverLeaveEvent(event);
}
} // namespace bloom::ui::node_editor

namespace bloom::ui::node_editor {
void NodeItem::setPreviewWidth(const qreal width) {
    layout_.width = width;
    relayout();
    for (auto* edge : edges_)
        edge->updatePath();
}
} // namespace bloom::ui::node_editor

namespace bloom::ui::node_editor {
void NodeItem::startRename() {
    auto* graphScene = qobject_cast<NodeGraphicsScene*>(scene());
    if (!session_ || !session_->composition() || !graphScene || !graphScene->canSubmit())
        return;
    std::optional<document::LayerId> layer;
    for (const auto& boundary : session_->composition()->graph().layerOutputs())
        if (boundary.nodeId == id_)
            layer = boundary.layerId;
    if (!layer)
        return;
    if (renameProxy_) {
        renameProxy_->show();
        renameProxy_->widget()->setFocus();
        return;
    }
    auto* field = new QLineEdit(title_);
    field->setObjectName(QStringLiteral("nodeRenameEditor"));
    field->setAccessibleName(tr("Layer name"));
    field->setFont(kit::font(kit::TypeRole::UiSmall));
    field->resize(static_cast<int>(std::ceil(width_ - 2 * kCardPadding)),
                  static_cast<int>(kCardHeaderHeight));
    hostTranslucent(*field);
    renameProxy_ = new QGraphicsProxyWidget(this);
    renameProxy_->setWidget(field);
    renameProxy_->setPos(kCardPadding, 0);
    renameProxy_->setZValue(5);
    const auto revision = session_->snapshot().revision();
    const auto composition = session_->compositionId();
    connect(field, &QLineEdit::editingFinished, this,
            [this, field, graphScene, layer = *layer, revision, composition] {
                if (!renameProxy_ || !renameProxy_->isVisible())
                    return;
                renameProxy_->hide();
                commands::Transaction transaction("Rename Layer", revision);
                transaction.emplace<commands::RenameLayer>(composition, layer,
                                                           field->text().toStdString());
                (void)graphScene->submit(std::move(transaction));
                auto* retired = renameProxy_;
                renameProxy_ = nullptr;
                retired->deleteLater();
            });
    auto* cancel = new QShortcut(QKeySequence(Qt::Key_Escape), field);
    cancel->setContext(Qt::WidgetShortcut);
    connect(cancel, &QShortcut::activated, this, [this] {
        auto* retired = renameProxy_;
        retired->hide();
        renameProxy_ = nullptr;
        retired->deleteLater();
    });
    field->setFocus(Qt::OtherFocusReason);
    field->selectAll();
}
} // namespace bloom::ui::node_editor

namespace bloom::ui::node_editor {
void SocketItem::setAuthoringEnabled(const bool enabled) {
    setCursor(enabled && draggable() ? Qt::CrossCursor : Qt::ForbiddenCursor);
    setToolTip(enabled ? description_
                       : description_ + QStringLiteral("\nNode command submission is unavailable"));
}
} // namespace bloom::ui::node_editor
