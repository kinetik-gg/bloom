#include "node_editor_items.hpp"
#include <QCoreApplication>
#include <QPainterPathStroker>
#include <QShortcut>
#include <bloom/commands/node_operations.hpp>
#include <bloom/ui/asset_controller.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <memory>

namespace bloom::ui {
kit::Color socketColorToken(const runtime::SocketValueKind kind) noexcept {
    switch (kind) {
    case runtime::SocketValueKind::Image:
        return kit::Color::SocketImage;
    case runtime::SocketValueKind::Audio:
        return kit::Color::SocketAudio;
    case runtime::SocketValueKind::Color:
        return kit::Color::SocketColor;
    case runtime::SocketValueKind::Scalar:
        return kit::Color::SocketScalar;
    case runtime::SocketValueKind::Vector2:
    // Task S7: both vector widths share one token so they read as a family. The socket's NAME and
    // tooltip are what distinguish them, and a cross-width link is refused by the kind check
    // regardless -- two adjacent violets would have said "these connect" when they do not.
    case runtime::SocketValueKind::Vector3:
        return kit::Color::SocketVector;
    case runtime::SocketValueKind::String:
        return kit::Color::SocketString;
    case runtime::SocketValueKind::Integer:
        return kit::Color::SocketInteger;
    case runtime::SocketValueKind::Boolean:
        return kit::Color::SocketBoolean;
    }
    return kit::Color::SocketImage;
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
    case document::NodeCategory::Math:
        return QCoreApplication::translate("node_editor", "Math");
    case document::NodeCategory::Output:
        return QCoreApplication::translate("node_editor", "Output");
    case document::NodeCategory::Compatibility:
        return QCoreApplication::translate("node_editor", "Compatibility");
    case document::NodeCategory::Utilities:
        return QCoreApplication::translate("node_editor", "Utilities");
    }
    return {};
}

QString nodeCategoryName(const QString& category) { return category; }
QString nodeCategoryName(const document::NodeDefinition& definition) {
    const auto& type = definition.key.typeId;
    if (type == document::kTimeValueNodeType || type == document::kFrameNumberNodeType ||
        type == document::kFrameRateNodeType || type == document::kSecondsToFramesNodeType ||
        type == document::kFramesToSecondsNodeType ||
        type == document::kSecondsToTimecodeNodeType ||
        type == document::kTimecodeToSecondsNodeType)
        return QStringLiteral("Time");
    if (type == document::kSeparateHsvNodeType || type == document::kCombineHsvNodeType ||
        type == document::kHueShiftNodeType || type == document::kLuminanceNodeType ||
        type == document::kColorMixNodeType)
        return QStringLiteral("Color");
    if (definition.category == document::NodeCategory::Values)
        return QStringLiteral("Values");
    if (type == document::kBooleanLogicNodeType || type == document::kBooleanNotNodeType ||
        type == document::kInRangeNodeType || type == document::kCompareNodeType ||
        type.starts_with("bloom.switch-"))
        return QStringLiteral("Logic");
    if (type == document::kVector2MathNodeType || type == document::kVector3MathNodeType ||
        type == document::kVector2ReduceNodeType || type == document::kVector3ReduceNodeType ||
        type == document::kRotate2dNodeType || type == document::kPolarToCartesianNodeType ||
        type == document::kCartesianToPolarNodeType)
        return QStringLiteral("Vector");
    if (definition.category == document::NodeCategory::Utilities &&
        type.find("-to-") != std::string::npos)
        return QStringLiteral("Convert");
    if (type.starts_with("bloom.string-"))
        return QStringLiteral("String");
    return nodeCategoryName(definition.category);
}
std::span<const QString> nodeCategoryOrder() {
    static const std::array order{
        QStringLiteral("Sources"), QStringLiteral("Layers"), QStringLiteral("Compositing"),
        QStringLiteral("Values"),  QStringLiteral("Math"),   QStringLiteral("Convert"),
        QStringLiteral("String"),  QStringLiteral("Logic"),  QStringLiteral("Time"),
        QStringLiteral("Color"),   QStringLiteral("Vector"), QStringLiteral("Utilities"),
        QStringLiteral("Output")};
    return order;
}

QString displayTypeName(const std::string_view typeId) {
    if (typeId == "startFrame")
        return QCoreApplication::translate("node_editor", "Start Frame");
    if (typeId == "loopMode")
        return QCoreApplication::translate("node_editor", "Loop Mode");
    if (typeId == "colorSpace")
        return QCoreApplication::translate("node_editor", "Color Space");
    QString name = QString::fromUtf8(typeId.data(), static_cast<qsizetype>(typeId.size()));
    if (name.startsWith(QStringLiteral("bloom."))) {
        name.remove(0, 6);
    }
    name.replace('-', ' ');
    bool first = true;
    for (auto& character : name) {
        if (first)
            character = character.toUpper();
        first = character.isSpace();
    }
    return name;
}

// Task S1, item 7. displayTypeName() above spells a name out of an identifier, which is the right
// answer for a parameter role and the wrong one for a node the artist reads on a card: "Solid
// source" names the implementation, "Solid" names the thing. Four built-ins are therefore named
// here. Type ids are untouched -- this is vocabulary, not identity.
QString nodeTypeDisplayName(const std::string_view typeId) {
    if (typeId == "bloom.image-source")
        return QCoreApplication::translate("node_editor", "Image");
    if (typeId == "bloom.audio-source")
        return QCoreApplication::translate("node_editor", "Audio");
    if (typeId == document::kSolidSourceNodeType)
        return QCoreApplication::translate("node_editor", "Solid");
    if (typeId == document::kLayerOutputNodeType)
        return QCoreApplication::translate("node_editor", "Layer");
    if (typeId == document::kLayerStackNodeType)
        return QCoreApplication::translate("node_editor", "Merge");
    if (typeId == document::kCompositionOutputNodeType)
        return QCoreApplication::translate("node_editor", "Output");
    // Task S7's library. Spelled out for the same reason the four above are: displayTypeName()
    // reads a name out of an identifier, which gives "Value scalar" and "Separate xy" -- the
    // implementation's spelling rather than the artist's. Type ids are untouched; this is
    // vocabulary, not identity.
    struct LibraryName final {
        std::string_view typeId;
        const char* name;
    };
    static const std::array kLibraryNames{
        LibraryName{document::kIntegerValueNodeType, "Integer"},
        LibraryName{document::kScalarValueNodeType, "Scalar"},
        LibraryName{document::kVector2ValueNodeType, "Vector 2"},
        LibraryName{document::kVector3ValueNodeType, "Vector 3"},
        LibraryName{document::kStringValueNodeType, "String"},
        LibraryName{document::kColorValueNodeType, "Color"},
        LibraryName{document::kBooleanValueNodeType, "Boolean"},
        LibraryName{document::kTimeValueNodeType, "Time"},
        LibraryName{document::kScalarMathNodeType, "Math"},
        LibraryName{document::kVector2MathNodeType, "Vector 2 Math"},
        LibraryName{document::kVector3MathNodeType, "Vector 3 Math"},
        LibraryName{document::kVector2ReduceNodeType, "Vector 2 Measure"},
        LibraryName{document::kVector3ReduceNodeType, "Vector 3 Measure"},
        LibraryName{document::kMapRangeNodeType, "Map Range"},
        LibraryName{document::kClampNodeType, "Clamp"},
        LibraryName{document::kMixNodeType, "Mix"},
        LibraryName{document::kColorMixNodeType, "Mix Color"},
        LibraryName{document::kCompareNodeType, "Compare"},
        LibraryName{document::kScalarSwitchNodeType, "Switch Scalar"},
        LibraryName{document::kIntegerSwitchNodeType, "Switch Integer"},
        LibraryName{document::kBooleanSwitchNodeType, "Switch Boolean"},
        LibraryName{document::kVector2SwitchNodeType, "Switch Vector 2"},
        LibraryName{document::kVector3SwitchNodeType, "Switch Vector 3"},
        LibraryName{document::kColorSwitchNodeType, "Switch Color"},
        LibraryName{document::kStringSwitchNodeType, "Switch String"},
        LibraryName{document::kSeparateXyNodeType, "Separate XY"},
        LibraryName{document::kCombineXyNodeType, "Combine XY"},
        LibraryName{document::kSeparateXyzNodeType, "Separate XYZ"},
        LibraryName{document::kCombineXyzNodeType, "Combine XYZ"},
        LibraryName{document::kSeparateRgbaNodeType, "Separate RGBA"},
        LibraryName{document::kCombineRgbaNodeType, "Combine RGBA"},
        LibraryName{document::kRandomNodeType, "Random"},
        LibraryName{document::kRerouteNodeType, "Reroute"},
    };
    const auto* const match = std::ranges::find(kLibraryNames, typeId, &LibraryName::typeId);
    if (match != kLibraryNames.end())
        return QCoreApplication::translate("node_editor", match->name);
    // Task UTIL-1's library carries its own name in its descriptor, beside the shape that name
    // belongs to, rather than in a second table here that could fall out of step with the first.
    if (const auto* descriptor = document::findValueUtilityDescriptor(typeId))
        return QString::fromUtf8(descriptor->displayName.data(),
                                 static_cast<qsizetype>(descriptor->displayName.size()));
    return displayTypeName(typeId);
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
    return nodeTypeDisplayName(node.typeId);
}

// A layer boundary card carries its LAYER's name, so the card alone would no longer say what kind
// of node it is. The eyebrow is what still says it: one small line above the name, nothing else.
QString nodeEyebrow(const document::Composition&, const document::NodeRecord& node) {
    const auto* definition =
        document::builtInNodeDefinitions().find(node.typeId, node.schemaVersion);
    return definition ? nodeCategoryName(*definition) : QString{};
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
            } else if constexpr (std::is_same_v<Value, document::Vec3d>) {
                return QStringLiteral("%1, %2, %3")
                    .arg(value.x, 0, 'f', 1)
                    .arg(value.y, 0, 'f', 1)
                    .arg(value.z, 0, 'f', 1);
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
    field->setCompact(true);
    field->setSingleStep(1.0);
    field->setUnit(unit);
    field->resize(field->sizeHint());
    return field;
}

void NodeItem::paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget*) {
    const QRectF bounds = cardRect();
    if (reroute_) {
        // A dot in the kind's own colour, ringed like a socket so it reads as part of the wire,
        // with the selection outline the cards use so selecting one is the same gesture and the
        // same ink.
        painter->setRenderHint(QPainter::Antialiasing, true);
        const auto kind =
            sockets_.empty() ? document::SocketValueKind::Image : sockets_.front()->kind;
        painter->setPen(
            QPen(kit::color(option->state.testFlag(QStyle::State_Selected) ? kit::Color::Accent
                                                                           : kit::Color::Surface),
                 kSelectionEdgeWidth));
        painter->setBrush(kit::color(socketColorToken(kind)));
        painter->drawEllipse(bounds.center(), kRerouteDiameter / 2.0, kRerouteDiameter / 2.0);
        return;
    }
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
        painter->setPen(QPen(kit::color(kit::Color::Accent), kSelectionEdgeWidth));
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

    const auto [titleRect, categoryRect] = titleBandRects();
    painter->setFont(kit::font(kit::TypeRole::UiSmall));
    painter->setPen(kit::color(kit::Color::Muted));
    painter->drawText(categoryRect, Qt::AlignRight | Qt::AlignVCenter, eyebrow_);
    painter->setFont(kit::font(kit::TypeRole::Ui));
    painter->setPen(kit::color(kit::Color::Foreground));
    painter->drawText(
        titleRect, Qt::AlignLeft | Qt::AlignVCenter,
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
    if (imageSource_ || audioSource_) {
        const QRectF cell(kCardPadding, kCardHeaderHeight + kCardPadding, width_ - 2 * kCardPadding,
                          kit::px(kit::Size::ImageThumbnail) - 2 * kCardPadding);
        painter->fillRect(cell, kit::color(kit::Color::SurfaceSunken));
        const auto* controller = session_ ? session_->assetController() : nullptr;
        const auto thumbnail =
            imageSource_ && controller ? controller->nodeThumbnail(id_) : QImage{};
        const auto waveform =
            audioSource_ && controller ? controller->waveform(imageAsset_) : nullptr;
        if (!thumbnail.isNull()) {
            auto size = QSizeF(thumbnail.size());
            size.scale(cell.size(), Qt::KeepAspectRatio);
            painter->drawImage(
                QRectF(cell.center() - QPointF(size.width() / 2, size.height() / 2), size),
                thumbnail);
        } else if (waveform && !waveform->buckets.empty()) {
            painter->setPen(QPen(kit::color(kit::Color::DataAudio), 1.0));
            const auto bucketCount = waveform->buckets.size();
            for (std::size_t index = 0; index < bucketCount; ++index) {
                float minimum = 0.0F;
                float maximum = 0.0F;
                for (const auto& channel : waveform->buckets[index]) {
                    minimum = std::min(minimum, channel.minimum);
                    maximum = std::max(maximum, channel.maximum);
                }
                const auto x = cell.left() + cell.width() * (static_cast<qreal>(index) + 0.5) /
                                                 static_cast<qreal>(bucketCount);
                const auto top =
                    cell.center().y() -
                    cell.height() * std::clamp(static_cast<qreal>(maximum), 0.0, 1.0) / 2.0;
                const auto bottom =
                    cell.center().y() -
                    cell.height() * std::clamp(static_cast<qreal>(minimum), -1.0, 0.0) / 2.0;
                painter->drawLine(QPointF(x, top), QPointF(x, bottom));
            }
        } else {
            const auto glyph =
                kit::iconPixmap(audioSource_ ? kit::IconId::Audio : kit::IconId::Warning,
                                kit::Size::IconSmall, kit::Color::Muted);
            painter->drawPixmap(cell.center() -
                                    QPointF(glyph.width() / glyph.devicePixelRatio() / 2,
                                            glyph.height() / glyph.devicePixelRatio() / 2),
                                glyph);
        }
    }

    painter->setFont(kit::font(kit::TypeRole::UiSmall));
    painter->setPen(kit::color(kit::Color::Muted));
    if ((imageSource_ || audioSource_) && session_) {
        const auto* asset = session_->snapshot().project().findAsset(imageAsset_);
        const auto* controller = session_->assetController();
        if (!asset || !asset->manifest.gaps.empty() ||
            (controller && controller->missing(imageAsset_))) {
            const auto warning =
                kit::iconPixmap(kit::IconId::Warning, kit::Size::IconSmall, kit::Color::Warn);
            painter->drawPixmap(
                QPointF(width_ - kCardPadding - warning.width() / warning.devicePixelRatio(),
                        kCardHeaderHeight + kCardPadding),
                warning);
        }
    }
    for (const auto* socket : sockets_) {
        if (parameterSocketY_.contains(socket->name))
            continue;
        const qreal rowExtent = socket->rowHeight();
        const QRectF row(kCardPadding, socket->pos().y() - rowExtent / 2,
                         std::max(0.0, width_ - (kCardPadding + kCardPadding)), rowExtent);
        painter->drawText(
            row,
            static_cast<int>(Qt::AlignVCenter | (socket->input ? Qt::AlignLeft : Qt::AlignRight)),
            painter->fontMetrics().elidedText(
                socket->multiInput() ? QStringLiteral("%1 (%2)")
                                           .arg(displayTypeName(socket->name.toStdString()))
                                           .arg(socket->orderedInputs().size())
                                     : displayTypeName(socket->name.toStdString()),
                Qt::ElideRight, static_cast<int>(row.width())));
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

NodeGroupItem* groupItemAncestor(QGraphicsItem* item) {
    for (auto* candidate = item; candidate != nullptr; candidate = candidate->parentItem()) {
        if (auto* group = dynamic_cast<NodeGroupItem*>(candidate)) {
            return group;
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
    case document::SocketValueKind::Audio:
        return QStringLiteral("Audio");
    case document::SocketValueKind::Color:
        return QStringLiteral("Color");
    case document::SocketValueKind::Scalar:
        return QStringLiteral("Scalar");
    case document::SocketValueKind::Vector2:
        return QStringLiteral("Vector2");
    case document::SocketValueKind::String:
        return QStringLiteral("String");
    case document::SocketValueKind::Integer:
        return QStringLiteral("Integer");
    case document::SocketValueKind::Boolean:
        return QStringLiteral("Boolean");
    case document::SocketValueKind::Vector3:
        return QStringLiteral("Vector3");
    }
    return {};
}

QPainterPath linkPath(const QPointF start, const QPointF end, const LinkStyle style) {
    switch (style) {
    case LinkStyle::Straight: {
        QPainterPath path(start);
        path.lineTo(end);
        return path;
    }
    case LinkStyle::Angled: {
        // Orthogonal horizontal-vertical-horizontal: out of the source horizontally to the
        // midpoint, straight down (or up) to the destination's row, then horizontally into it.
        QPainterPath path(start);
        const qreal midX = (start.x() + end.x()) / 2.0;
        path.lineTo(midX, start.y());
        path.lineTo(midX, end.y());
        path.lineTo(end);
        return path;
    }
    case LinkStyle::Spline:
        break;
    }
    const qreal handle = std::max(static_cast<qreal>(kit::px(kit::Size::NodeLinkHandleMin)),
                                  std::abs(end.x() - start.x()) / 2.0);
    QPainterPath path(start);
    path.cubicTo(start + QPointF(handle, 0), end - QPointF(handle, 0), end);
    return path;
}

SocketItem::SocketItem(const document::NodeId node, QString portName,
                       const document::SocketValueKind valueKind,
                       std::optional<document::InputPortRef> inputRef,
                       std::optional<document::OutputPortRef> outputRef, QGraphicsItem* parent)
    : QGraphicsItem(parent), name(std::move(portName)), kind(valueKind), input(std::move(inputRef)),
      output(std::move(outputRef)) {
    setData(kNodeItemKindRole, QStringLiteral("socket"));
    setData(kNodeStableIdRole, QVariant::fromValue<qulonglong>(node.value()));
    setData(kNodeSocketNameRole, name);
    setData(kNodeSocketInputRole, input.has_value());
    setAcceptHoverEvents(true);
    setAcceptedMouseButtons(Qt::LeftButton);
    setZValue(2);
    setCursor(draggable() ? Qt::CrossCursor : Qt::ForbiddenCursor);
    description_ = name + QStringLiteral(" · ") + socketKindName(kind);
    setAuthoringEnabled(true);
}

qreal SocketItem::pillLength() const {
    if (!multiInput())
        return kSocketDiameter;
    // At least one ordinary row tall, then one pitch per ordered slot: the pill's length is the
    // stack's depth, read straight off the slot model.
    return std::max(kSocketRowHeight, static_cast<qreal>(orderedInputs_.size()) * kStackSlotPitch);
}

qreal SocketItem::rowHeight() const { return std::max(kSocketRowHeight, pillLength()); }

QRectF SocketItem::boundingRect() const {
    const qreal half = pillLength() / 2.0 + kSocketDiameter / 2.0 + kSocketHitSlop;
    return {-16.0, -half, 32.0, half * 2.0};
}

QPainterPath SocketItem::shape() const {
    QPainterPath hit;
    if (!multiInput()) {
        hit.addEllipse(boundingRect());
        return hit;
    }
    // A pill's hit shape is a pill: the same 12px of slop the round socket gets, around a longer
    // body, rather than one ellipse stretched over the whole of it.
    const QRectF bounds = boundingRect();
    hit.addRoundedRect(bounds, bounds.width() / 2.0, bounds.width() / 2.0);
    return hit;
}

void SocketItem::setOrderedInputs(std::vector<document::InputPortRef> inputs) {
    prepareGeometryChange();
    orderedInputs_ = std::move(inputs);
    stackPill_ = true;
    description_ =
        orderedInputs_.empty()
            ? name + QStringLiteral(" · ") + socketKindName(kind) +
                  QCoreApplication::translate(
                      "node_editor", "\nOrdered multi-input: empty; drop an image output here")
            : name + QStringLiteral(" · ") + socketKindName(kind) +
                  QCoreApplication::translate(
                      "node_editor", "\nOrdered multi-input: %1 in stack order, topmost first")
                      .arg(orderedInputs_.size());
    setToolTip(description_);
    update();
}

bool SocketItem::accepts(const document::InputPortRef& ref) const {
    if ((input.has_value() && *input == ref) ||
        std::ranges::find(orderedInputs_, ref) != orderedInputs_.end())
        return true;
    // The stack pill stands for every role of every slot: a Layer's audio edge into a slot lands
    // on the same pill as its content edge, so the artist can see that the audio is already routed.
    const auto* slot = std::get_if<document::LayerStackInputRef>(&ref);
    return slot != nullptr &&
           std::ranges::any_of(orderedInputs_, [&](const document::InputPortRef& candidate) {
               const auto* ordered = std::get_if<document::LayerStackInputRef>(&candidate);
               return ordered != nullptr && ordered->stackNodeId == slot->stackNodeId &&
                      ordered->slotId == slot->slotId;
           });
}

void SocketItem::setDropIndicator(const std::optional<std::size_t> slotIndex) {
    if (dropIndicator_ == slotIndex)
        return;
    dropIndicator_ = slotIndex;
    update();
}

std::optional<document::LayerSlotId> SocketItem::slotInsertionAt(const QPointF localPoint) const {
    const auto index = slotIndexAt(localPoint);
    if (!index.has_value())
        return std::nullopt;
    // The caret sits ON a slot; a pointer in that slot's upper half means "above it", and in its
    // lower half "below it" -- which for the last slot is an append.
    const qreal pitch = pillLength() / static_cast<qreal>(orderedInputs_.size());
    const qreal top = -pillLength() / 2.0 + pitch * static_cast<qreal>(*index);
    const bool below = localPoint.y() > top + pitch / 2.0;
    const auto target = below ? *index + 1 : *index;
    if (target >= orderedInputs_.size())
        return std::nullopt;
    const auto* slot = std::get_if<document::LayerStackInputRef>(&orderedInputs_[target]);
    return slot == nullptr ? std::nullopt : std::optional(slot->slotId);
}

std::optional<std::size_t> SocketItem::slotIndexAt(const QPointF localPoint) const {
    if (!multiInput() || orderedInputs_.empty() || !shape().contains(localPoint))
        return std::nullopt;
    const qreal length = pillLength();
    const qreal pitch = length / static_cast<qreal>(orderedInputs_.size());
    const auto index = static_cast<std::ptrdiff_t>(
        std::floor((localPoint.y() + length / 2.0) / std::max(pitch, 0.001)));
    return static_cast<std::size_t>(std::clamp(
        index, std::ptrdiff_t{0}, static_cast<std::ptrdiff_t>(orderedInputs_.size()) - 1));
}

QColor SocketItem::paintedInk() const {
    const QColor base = kit::color(socketColorToken(kind));
    switch (affinity_) {
    case DragAffinity::Compatible:
        return kit::hoverFillFor(base);
    case DragAffinity::Incompatible:
        return kit::withOpacity(base, kit::kDisabledOpacity);
    case DragAffinity::Idle:
        break;
    }
    return base;
}

void SocketItem::setDragAffinity(const DragAffinity affinity, const QString& refusal) {
    const QString tip = affinity == DragAffinity::Incompatible && !refusal.isEmpty()
                            ? description_ + QStringLiteral("\n") + refusal
                            : description_;
    if (affinity_ == affinity && toolTip() == tip)
        return;
    affinity_ = affinity;
    setToolTip(tip);
    update();
}

void SocketItem::paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*) {
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setPen(QPen(kit::color(kit::Color::Surface), kit::kHairlineWidth));
    painter->setBrush(paintedInk());
    const qreal radius = hovered_ ? 6.0 : kSocketDiameter / 2;
    if (!multiInput()) {
        painter->drawEllipse(QPointF(), radius, radius);
        return;
    }
    // The Merge node's one ordered multi-input: a vertical pill, divided into one segment per stack
    // slot so the port itself shows how many layers it carries and in what order. The segment
    // divisions are drawn in the card's own Surface ink, the same hairline that rings every socket.
    const qreal length = pillLength();
    const QRectF pill(-radius, -length / 2.0, radius * 2.0, length);
    painter->drawRoundedRect(pill, radius, radius);
    if (orderedInputs_.empty())
        return;
    // Not named `slots`: Qt's moc keyword macro takes that identifier.
    const auto slotCount = static_cast<qreal>(orderedInputs_.size());
    kit::applyHairlinePen(*painter, kit::color(kit::Color::Surface));
    for (std::size_t division = 1; division < orderedInputs_.size(); ++division) {
        const qreal y = pill.top() + length * static_cast<qreal>(division) / slotCount;
        painter->drawLine(QPointF(pill.left(), y), QPointF(pill.right(), y));
    }
    if (dropIndicator_.has_value()) {
        // The position the pointer is at in the order. Muted, not Accent: a stack slot is
        // structural, so this marks where the pointer IS and never promises that releasing there
        // lands a link -- the pill is dimmed as incompatible at the same moment.
        const qreal pitch = length / slotCount;
        const qreal y = pill.top() + pitch * (static_cast<qreal>(*dropIndicator_) + 0.5);
        painter->setPen(QPen(kit::color(kit::Color::Muted), 2.0, Qt::SolidLine, Qt::RoundCap));
        painter->drawLine(QPointF(pill.left() - radius, y), QPointF(pill.right() + radius, y));
    }
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
    // A reroute's sockets take the kind of the link it sits on (task FIX1, item I); the
    // definition's declared Image is only the placeholder a definition must name. An unconnected
    // reroute keeps the placeholder for painting and accepts anything, which is what its own
    // kind-less state means.
    const auto rerouteKind = document::isRerouteNodeType(node.typeId)
                                 ? composition.graph().rerouteKind(node.id, registry)
                                 : std::nullopt;
    const auto kindOf = [&rerouteKind](const document::SocketValueKind declared) {
        return rerouteKind.value_or(declared);
    };
    const bool kindless = document::isRerouteNodeType(node.typeId) && !rerouteKind.has_value();
    for (const auto& port : definition->inputs) {
        document::InputPortRef input = document::NodeInputRef{node.id, port.name};
        // Two kinds of port, one question each. An OPERAND socket is linked when its parameter
        // carries a driver binding; an image transport port is linked when an edge terminates on
        // it. That split is not an inconsistency -- each has exactly one durable record of where
        // its value comes from, which is why an edge and a binding can never disagree about a
        // socket.
        const auto binding =
            std::ranges::find(node.parameters, port.name, &document::ParameterBinding::role);
        const bool linked =
            binding != node.parameters.end()
                ? [&] {
                      const auto* parameter = composition.parameters().find(binding->parameterId);
                      return parameter != nullptr &&
                             std::holds_alternative<document::DriverBindingSource>(
                                 parameter->source);
                  }()
                : std::ranges::any_of(composition.graph().edges(), [&](const auto& edge) {
                      return edge.destination == input;
                  });
        if (linked)
            linkedInputs_.insert(QString::fromStdString(port.name));
        sockets_.push_back(new SocketItem(node.id, QString::fromStdString(port.name),
                                          kindOf(port.valueKind), input, std::nullopt, this));
        sockets_.back()->setAcceptsAnyKind(kindless);
    }
    if (definition->layerSlotInput && composition.graph().merge(node.id)) {
        // Task S1, item 7: ONE ordered multi-input for the whole stack, not one repeated row per
        // slot. The slot model underneath is exactly as it was -- these are its own slots, in its
        // own order -- and every edge that terminates on any of them terminates on this one socket.
        //
        // Task FIX1, item B: the pill exists even when the stack is EMPTY, and its own `input` is
        // the invalid-slot sentinel that ConnectPorts reads as "make a new slot here". Without it a
        // fresh composition's Merge node had no port at all and the artist had nothing to wire the
        // first layer into.
        const auto& port = *definition->layerSlotInput;
        const auto entries = composition.graph().merge(node.id)->entries();
        std::vector<document::InputPortRef> ordered;
        ordered.reserve(entries.size());
        for (const auto& slot : entries)
            ordered.push_back(document::LayerStackInputRef{node.id, slot.slotId, port.role});
        auto* pill = new SocketItem(
            node.id, QString::fromStdString(port.role), port.valueKind,
            document::LayerStackInputRef{node.id, document::LayerSlotId{}, port.role}, std::nullopt,
            this);
        pill->setOrderedInputs(std::move(ordered));
        sockets_.push_back(pill);
    }
    for (const auto& port : definition->outputs)
        sockets_.push_back(new SocketItem(node.id, QString::fromStdString(port.name),
                                          port.valueKind, std::nullopt,
                                          document::OutputPortRef{node.id, port.name}, this));
}

NodeEdgeItem::NodeEdgeItem(NodeItem& source, NodeItem& destination, SocketItem& output,
                           SocketItem& input, document::EdgeRecord record, const bool isStructural,
                           const LinkStyle style)
    : edge(std::move(record)), structural(isStructural), source_(source), destination_(destination),
      output_(output), input_(input), style_(style) {
    setData(kNodeItemKindRole, QStringLiteral("edge"));
    setData(kNodeStableIdRole, QVariant::fromValue<qulonglong>(edge.id.value()));
    setData(kNodeStructuralRole, structural);
    setAcceptHoverEvents(true);
    setAcceptedMouseButtons(Qt::NoButton);
    setZValue(-1);
    // Every link names both of its ends (task FIX1, item C), so hovering a wire in a dense graph
    // says what it connects instead of leaving the artist to trace it.
    setToolTip(QCoreApplication::translate("node_editor", "%1 · %2  →  %3 · %4")
                   .arg(source.title(), output.name, destination.title(), input.name));
    source_.addEdge(*this);
    destination_.addEdge(*this);
    updatePath();
}
void NodeEdgeItem::updatePath() {
    setPath(linkPath(output_.scenePos(), input_.scenePos(), style_));
}
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
    painter->setPen(QPen(emphasized ? base.lighter(kit::kLinkActiveLightness) : base,
                         kit::kNodeLinkWidth, Qt::SolidLine, Qt::RoundCap));
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
// Retires a rename field for good: detached from the card and taken out of the scene NOW, then
// deferred-deleted. Hiding it and waiting for the deferred delete left a stale, invisible editor
// among the card's children, and fieldWidget() answers with the first child that matches a name --
// so the next rename's own field could not be found at all.
void NodeItem::retireRenameProxy() {
    auto* retired = renameProxy_;
    if (retired == nullptr)
        return;
    renameProxy_ = nullptr;
    retired->hide();
    retired->setParentItem(nullptr);
    if (scene() != nullptr)
        scene()->removeItem(retired);
    retired->deleteLater();
}

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
    auto* field = new kit::KLineEdit(title_);
    field->setObjectName(QStringLiteral("nodeRenameEditor"));
    field->setAccessibleName(tr("Layer name"));
    field->setFont(kit::font(kit::TypeRole::UiSmall));
    field->resize(static_cast<int>(std::ceil(width_ - (kCardPadding + kCardPadding))),
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
                const auto name = field->text().toStdString();
                retireRenameProxy();
                commands::Transaction transaction("Rename Layer", revision);
                transaction.emplace<commands::RenameLayer>(composition, layer, name);
                (void)graphScene->submit(std::move(transaction));
            });
    auto* cancel = new QShortcut(QKeySequence(Qt::Key_Escape), field);
    cancel->setContext(Qt::WidgetShortcut);
    connect(cancel, &QShortcut::activated, this, [this] { retireRenameProxy(); });
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

namespace bloom::ui::node_editor {
NodeGroupItem::NodeGroupItem(const document::NodeGroupId id, CompositionSession* session)
    : id_(id), session_(session) {
    setData(kNodeItemKindRole, QStringLiteral("node-group"));
    setData(kNodeStableIdRole, QVariant::fromValue<qulonglong>(id.value()));
    setAcceptHoverEvents(true);
    // Behind its members AND behind their links: a frame is the ground they sit on, not a pane over
    // them. Cards rest at 0 and links at -1.
    setZValue(-2);
}

void NodeGroupItem::refresh(const document::NodeGroupRecord& record) {
    title_ = QString::fromStdString(record.name);
    members_ = record.members;
    padding_ = record.padding;
    setToolTip(QStringLiteral("%1\nNode group %2").arg(title_).arg(id_.value()));
    update();
}

void NodeGroupItem::setFrameRect(const QRectF rect) {
    setPos(rect.topLeft());
    if (size_ == rect.size())
        return;
    prepareGeometryChange();
    size_ = rect.size();
    if (renameProxy_ != nullptr && renameProxy_->widget() != nullptr)
        renameProxy_->widget()->resize(static_cast<int>(std::ceil(std::max(
                                           0.0, size_.width() - (kCardPadding + kCardPadding)))),
                                       static_cast<int>(kGroupTitleHeight));
    update();
}

void NodeGroupItem::setAuthoringEnabled(const bool enabled) {
    authoringEnabled_ = enabled;
    setCursor(enabled ? Qt::OpenHandCursor : Qt::ArrowCursor);
}

void NodeGroupItem::paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*) {
    const QRectF bounds = boundingRect();
    if (bounds.isEmpty())
        return;
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setOpacity(kGroupFillOpacity);
    kit::fillRoundedSurface(*painter, bounds, kit::color(kit::Color::SurfaceRaised), QColor(),
                            kGroupRadius);
    painter->setOpacity(1.0);
    // The border keeps full opacity: the fill is what recedes, while the hairline is what says
    // where the frame actually ends -- which is the line a drop is judged against.
    const auto radius = static_cast<qreal>(
        kit::radiusPx(kGroupRadius, static_cast<int>(std::min(bounds.width(), bounds.height()))));
    painter->setBrush(Qt::NoBrush);
    kit::applyHairlinePen(*painter, kit::color(kit::Color::Border));
    painter->drawRoundedRect(bounds, radius, radius);
    if (renameProxy_ != nullptr && renameProxy_->isVisible())
        return;
    painter->setFont(kit::font(kit::TypeRole::UiSmall));
    painter->setPen(kit::color(kit::Color::Muted));
    painter->drawText(
        titleRect().adjusted(kCardPadding, 0.0, -kCardPadding, 0.0),
        static_cast<int>(Qt::AlignVCenter | Qt::AlignLeft),
        QFontMetricsF(painter->font())
            .elidedText(title_, Qt::ElideRight,
                        std::max(0.0, bounds.width() - (kCardPadding + kCardPadding))));
}

void NodeGroupItem::retireRenameProxy() {
    auto* retired = renameProxy_;
    if (retired == nullptr)
        return;
    renameProxy_ = nullptr;
    retired->hide();
    retired->setParentItem(nullptr);
    if (scene() != nullptr)
        scene()->removeItem(retired);
    retired->deleteLater();
    update();
}

// The same inline-editor shape a layer card's rename uses, over the frame's own title strip: commit
// on editingFinished (which is what Enter in the field means), cancel on Escape, and one
// RenameGroup transaction for the commit.
void NodeGroupItem::startRename() {
    auto* graphScene = qobject_cast<NodeGraphicsScene*>(scene());
    if (session_ == nullptr || session_->composition() == nullptr || graphScene == nullptr ||
        !graphScene->canSubmit())
        return;
    if (renameProxy_ != nullptr) {
        renameProxy_->show();
        renameProxy_->widget()->setFocus();
        return;
    }
    auto* field = new kit::KLineEdit(title_);
    field->setObjectName(QStringLiteral("nodeGroupRenameEditor"));
    field->setAccessibleName(tr("Node group name"));
    field->setFont(kit::font(kit::TypeRole::UiSmall));
    field->resize(
        static_cast<int>(std::ceil(std::max(0.0, size_.width() - (kCardPadding + kCardPadding)))),
        static_cast<int>(kGroupTitleHeight));
    field->setAttribute(Qt::WA_TranslucentBackground, true);
    field->setAttribute(Qt::WA_NoSystemBackground, true);
    field->setAutoFillBackground(false);
    renameProxy_ = new QGraphicsProxyWidget(this);
    renameProxy_->setWidget(field);
    renameProxy_->setPos(kCardPadding, 0);
    renameProxy_->setZValue(5);
    const auto revision = session_->snapshot().revision();
    const auto composition = session_->compositionId();
    connect(field, &QLineEdit::editingFinished, this,
            [this, field, graphScene, revision, composition] {
                if (renameProxy_ == nullptr || !renameProxy_->isVisible())
                    return;
                const auto name = field->text().toStdString();
                retireRenameProxy();
                commands::Transaction transaction("Rename Node Group", revision);
                transaction.emplace<commands::RenameGroup>(composition, id_, name);
                (void)graphScene->submit(std::move(transaction));
            });
    auto* cancel = new QShortcut(QKeySequence(Qt::Key_Escape), field);
    cancel->setContext(Qt::WidgetShortcut);
    connect(cancel, &QShortcut::activated, this, [this] { retireRenameProxy(); });
    field->setFocus(Qt::OtherFocusReason);
    field->selectAll();
    update();
}
} // namespace bloom::ui::node_editor
