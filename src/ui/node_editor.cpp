#include <bloom/ui/node_editor.hpp>

#include <bloom/ui/composition_editors.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/viewer_editor.hpp>

#include <bloom/ui/kit/color.hpp>
#include <bloom/ui/kit/color_chip.hpp>
#include <bloom/ui/kit/painting.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/kit/value_field.hpp>

#include <bloom/core/color.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/project.hpp>

#include <QAction>
#include <QBrush>
#include <QColor>
#include <QContextMenuEvent>
#include <QFontMetricsF>
#include <QGraphicsDropShadowEffect>
#include <QGraphicsObject>
#include <QGraphicsPathItem>
#include <QGraphicsProxyWidget>
#include <QGraphicsSceneHoverEvent>
#include <QGraphicsSceneMouseEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPointer>
#include <QResizeEvent>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QStyleOptionGraphicsItem>
#include <QVariant>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace bloom::ui {
namespace {

// --- Card geometry, entirely from tokens -------------------------------------------------------
//
// These are graph-space lengths in design pixels applied at 1x; the view's own zoom scales the
// whole card along with everything else, so a token spelled here reads exactly as it would in a
// widget at 100%.
constexpr qreal kCardPadding = kit::px(kit::Spacing::S);
constexpr qreal kCardLabelGap = kit::px(kit::Spacing::S);
constexpr qreal kCardRowGap = kit::px(kit::Spacing::XXS);
constexpr qreal kCardHeaderHeight = kit::px(kit::Size::PanelHeader);
constexpr auto kCardRadius = kit::Radius::Medium;
// The narrowest a card is ever drawn: four roomy controls' worth, so a parameterless node still
// reads as a card rather than as a label with a border.
constexpr qreal kCardMinimumWidth = kit::px(kit::Size::ControlRoomy) * 4;
// The port dot's diameter; its center sits on the header's own mid-line, at the card's left edge
// for an input and its right edge for an output.
constexpr qreal kSocketDiameter = kit::px(kit::Spacing::S);
// Dot-grid pitch, and the smallest on-screen pitch worth drawing at: below it the grid stops being
// a texture and starts being noise (and a pointless number of ellipses).
constexpr qreal kGridSpacing = kit::px(kit::Spacing::XXL);
constexpr qreal kGridMinimumDeviceSpacing = kit::px(kit::Spacing::M);
constexpr qreal kGridDotRadius = kit::kHairlineWidth;
// Selection is the 2px inset Accent edge docs/ux/visual-language.md's States table specifies for
// the case where a full Accent fill would hide content -- the same edge the timeline's track rows
// and the previous node projection already used.
constexpr qreal kSelectionEdgeWidth = 2.0;

// Default placement for a card the artist has never moved: four columns (source, layer output,
// layer stack, composition output), each pitch wide enough that a widest-case card still leaves a
// visible gutter, and a row pitch taller than the tallest card a layer boundary can produce. A card
// the artist HAS moved keeps its own position across every projection rebuild.
constexpr qreal kNodeColumnOrigin = kit::px(kit::Spacing::XXL);
constexpr qreal kNodeColumnPitch = kCardMinimumWidth + kit::px(kit::Spacing::XXL) * 4;
constexpr qreal kNodeRowPitch = kit::px(kit::Size::PanelHeader) * 6;
constexpr qreal kNodeSceneMargin = kit::px(kit::Spacing::XXL) * 2;

// The VIEW's own scrolling area, fixed and deliberately far larger than any graph or any display.
//
// This exists to make the canvas's mapping exact. QGraphicsView derives its viewport transform from
// the view transform PLUS a scroll offset, and that offset is not always zero: when the scene
// rectangle maps to something smaller than the viewport, QGraphicsView silently "indents" it to
// honor the view's alignment, which would quietly override a centered Fit and break the
// zoom-about-cursor invariant. Pinning the view's scene rectangle to a region that always maps
// LARGER than any viewport removes that branch entirely: the indents are always zero, the scroll
// bar values stay inside their range at 0, and viewportTransform() is exactly the view transform.
//
// "Always larger" is a real quantity, not a guess: at the smallest zoom the shared bounds allow
// (ViewTransform::kMinZoom), this half-extent still maps to kNodeColumnPitch * 1024 / 16 = more
// than twenty thousand device pixels on each side of the origin -- larger than any display, and
// larger than any graph the four-column default layout can produce.
constexpr qreal kCanvasHalfExtent = kNodeColumnPitch * 1024;

// A wheel detent, in QWheelEvent::angleDelta() eighth-of-a-degree units. Exactly the divisor
// ViewerEditor::wheelEvent() uses, so one notch means one step on both canvases.
constexpr int kWheelDetent = 120;

class NodeEdgeItem;
class NodeItem;

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

std::size_t layoutColumn(const document::NodeRecord& node) {
    if (node.typeId == document::kLayerStackNodeType) {
        return 2;
    }
    if (node.typeId == document::kLayerOutputNodeType) {
        return 1;
    }
    if (node.typeId == document::kCompositionOutputNodeType) {
        return 3;
    }
    return 0;
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

} // namespace

kit::Color socketColorToken(const runtime::SocketValueKind kind) noexcept {
    switch (kind) {
    case runtime::SocketValueKind::Image:
        return kit::Color::DataImage;
    }
    return kit::Color::DataImage;
}

namespace {

// The one transport kind any edge in a document::CanonicalGraph can carry today; see
// socketColorToken()'s own comment in node_editor.hpp for why there is exactly one.
constexpr auto kGraphTransportKind = runtime::SocketValueKind::Image;

class NodeItem final : public QGraphicsObject {
  public:
    NodeItem(const document::NodeId id, CompositionSession* session) : id_(id), session_(session) {
        setData(kNodeItemKindRole, QStringLiteral("node"));
        setData(kNodeStableIdRole, QVariant::fromValue<qulonglong>(id.value()));
        setFlags(ItemIsMovable | ItemIsSelectable | ItemSendsGeometryChanges);
        setCursor(Qt::OpenHandCursor);

        // Elevation on drag (decision 2). kit::applyElevation() takes a QWidget, and a node card is
        // a QGraphicsItem, so the token's own offset/blur/color are read here and handed to the
        // same QGraphicsDropShadowEffect that helper installs -- no literal shadow of its own. The
        // effect exists for the item's lifetime and is only ENABLED while a drag is in flight, so a
        // resting card pays nothing for it.
        const kit::Shadow token = kit::shadow(kit::Elevation::Drag);
        if (!token.isFlat()) {
            auto* shadow = new QGraphicsDropShadowEffect;
            shadow->setOffset(token.offsetX, token.offsetY);
            shadow->setBlurRadius(token.blurRadius);
            shadow->setColor(token.color);
            shadow->setEnabled(false);
            setGraphicsEffect(shadow);
            dragShadow_ = shadow;
        }
    }

    [[nodiscard]] document::NodeId id() const noexcept { return id_; }

    // Reconciles this card against the node record IN PLACE. Field widgets are created once, on the
    // first refresh that sees a given set of parameter roles, and afterwards only reconfigured --
    // exactly the discipline PropertiesEditor::rebuild() already follows, and what lets a card's
    // own field survive the snapshot change its edit produced.
    void refresh(const document::NodeRecord& node, const document::Composition& composition) {
        title_ = nodeDisplayName(composition, node);
        setToolTip(QStringLiteral("%1\n%2\nNode %3")
                       .arg(title_, displayTypeName(node.typeId))
                       .arg(id_.value()));
        ensureFields(node);
        refreshValues(node, composition);
        relayout();
    }

    // Sockets ACCUMULATE across the edges that touch this card, and are cleared once per
    // projection. A node in the middle of the chain -- a layer output, the layer stack -- is both
    // an edge source and an edge destination, so a setter that assigned both flags at once would
    // let whichever edge happened to be visited last erase the other end's dot.
    void clearSockets() {
        if (hasInputSocket_ || hasOutputSocket_) {
            hasInputSocket_ = false;
            hasOutputSocket_ = false;
            update();
        }
    }

    void markInputSocket() {
        if (!hasInputSocket_) {
            hasInputSocket_ = true;
            update();
        }
    }

    void markOutputSocket() {
        if (!hasOutputSocket_) {
            hasOutputSocket_ = true;
            update();
        }
    }

    [[nodiscard]] bool hasInputSocket() const noexcept { return hasInputSocket_; }
    [[nodiscard]] bool hasOutputSocket() const noexcept { return hasOutputSocket_; }

    [[nodiscard]] QPointF inputAnchor() const {
        return mapToScene(QPointF(0.0, kCardHeaderHeight / 2.0));
    }
    [[nodiscard]] QPointF outputAnchor() const {
        return mapToScene(QPointF(width_, kCardHeaderHeight / 2.0));
    }

    void addEdge(NodeEdgeItem& edge) { edges_.push_back(&edge); }
    void clearEdges() { edges_.clear(); }

    [[nodiscard]] QWidget* fieldWidget(const QString& objectName) const {
        for (const auto* child : childItems()) {
            const auto* proxy = qgraphicsitem_cast<const QGraphicsProxyWidget*>(child);
            if (proxy != nullptr && proxy->widget() != nullptr &&
                proxy->widget()->objectName() == objectName) {
                return proxy->widget();
            }
        }
        return nullptr;
    }

    // The card's own rectangle. The port dots are centered ON its left and right edges, so half of
    // each dot lies outside it -- boundingRect() below adds that overhang, because a QGraphicsItem
    // that paints outside its bounding rectangle leaves trails behind it and gets clipped out of
    // itemsBoundingRect() (which is what Fit frames against).
    [[nodiscard]] QRectF cardRect() const { return {0.0, 0.0, width_, height_}; }

    [[nodiscard]] QRectF boundingRect() const override {
        const qreal overhang = kSocketDiameter / 2.0 + kit::kHairlineWidth;
        return cardRect().adjusted(-overhang, 0.0, overhang, 0.0);
    }

    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget*) override;

  protected:
    QVariant itemChange(GraphicsItemChange change, const QVariant& value) override;

    // The elevation is raised on the first MOVE, not on the press: a click that only selects a card
    // is not a drag, and lifting the card for it would read as a flash rather than as depth.
    void mouseMoveEvent(QGraphicsSceneMouseEvent* event) override {
        if (dragShadow_ != nullptr) {
            dragShadow_->setEnabled(true);
        }
        QGraphicsObject::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QGraphicsSceneMouseEvent* event) override {
        if (dragShadow_ != nullptr) {
            dragShadow_->setEnabled(false);
        }
        QGraphicsObject::mouseReleaseEvent(event);
    }

  private:
    struct ValueRow final {
        QString label;
        kit::KValueField* field = nullptr;
    };

    // Selects THIS node through the session's one selection truth before any edit, because every
    // session write path this card uses (setSelectedPosition/setSelectedOpacity) targets the
    // session's current selection -- the same functions, on the same parameters, that
    // PropertiesEditor calls. Returns false if the node is no longer selectable, in which case no
    // command is issued at all.
    [[nodiscard]] bool selectSelf() {
        if (session_ == nullptr) {
            return false;
        }
        session_->selectNode(id_);
        const auto* selected = std::get_if<document::NodeId>(&session_->selection().primary);
        return selected != nullptr && *selected == id_;
    }

    void commitPosition() {
        if (refreshing_ || positionX_ == nullptr || positionY_ == nullptr || !selectSelf()) {
            return;
        }
        // Both components in one call, exactly as PropertiesEditor's own commitPosition lambda
        // does: one gesture is one SetParameterSource/SetKeyframeAtTime transaction, so it is one
        // undo step.
        (void)session_->setSelectedPosition(positionX_->value(), positionY_->value());
    }

    void commitOpacity() {
        if (refreshing_ || opacity_ == nullptr || !selectSelf()) {
            return;
        }
        (void)session_->setSelectedOpacity(opacity_->value() / 100.0);
    }

    void addProxy(QWidget* widget) {
        auto* proxy = new QGraphicsProxyWidget(this);
        proxy->setWidget(widget);
    }

    // Builds the card's rows once per role set. Editable rows exist only for roles that have BOTH a
    // kit primitive able to carry the value AND an existing session/command path able to write it
    // (decision 5's honesty rule):
    //
    //   position -> two KValueFields (X, Y), committed through
    //               CompositionSession::setSelectedPosition()
    //   opacity  -> one KValueField, committed through CompositionSession::setSelectedOpacity()
    //   color    -> a READ-ONLY KColorChip. There is no color write path anywhere: the whole
    //               command layer is AddSolidLayer, AddTextLayer, SetProjectName,
    //               SetCompositionName, SetCompositionDuration, SetCompositionFormat,
    //               SetParameterSource, MoveLayerBefore plus the animation operations, and
    //               CompositionSession exposes no color mutator at all -- PropertiesEditor's own
    //               Solid Source row is read-only text for exactly this reason. An enabled chip
    //               would open a picker whose result nothing could commit.
    //   anything else (text today) -> a painted read-only value row, because KValueField cannot
    //               carry a string and no command sets one after layer creation.
    void ensureFields(const document::NodeRecord& node) {
        std::vector<std::string> roles;
        roles.reserve(node.parameters.size());
        for (const auto& binding : node.parameters) {
            roles.push_back(binding.role);
        }
        if (fieldsBuilt_ && roles == builtRoles_) {
            return;
        }
        // Detached from the card, taken out of the scene (an item whose parent is cleared stays in
        // the scene as a top-level item and would keep painting), then deferred-deleted: this can
        // in principle run while one of these very widgets is emitting, so nothing is destroyed
        // under a live stack frame.
        for (auto* child : childItems()) {
            child->setParentItem(nullptr);
            if (scene() != nullptr) {
                scene()->removeItem(child);
            }
            if (auto* object = child->toGraphicsObject()) {
                object->deleteLater();
            } else {
                delete child;
            }
        }
        valueRows_.clear();
        readOnlyRows_.clear();
        positionX_ = nullptr;
        positionY_ = nullptr;
        opacity_ = nullptr;
        colorChip_ = nullptr;
        colorRowLabel_.clear();

        for (const auto& role : roles) {
            if (role == document::kPositionParameterRole) {
                // Range/decimals/step/unit mirror PropertiesEditor's Position editors verbatim, so
                // the same gesture in either surface produces the same value.
                positionX_ = makeCardField(QStringLiteral("nodePositionXEditor"), tr("Position X"),
                                           -1'000'000.0, 1'000'000.0, 2, QStringLiteral("px"));
                positionY_ = makeCardField(QStringLiteral("nodePositionYEditor"), tr("Position Y"),
                                           -1'000'000.0, 1'000'000.0, 2, QStringLiteral("px"));
                addProxy(positionX_);
                addProxy(positionY_);
                connect(positionX_, &kit::KValueField::valueChanged, this,
                        [this] { commitPosition(); });
                connect(positionY_, &kit::KValueField::valueChanged, this,
                        [this] { commitPosition(); });
                valueRows_.push_back({QStringLiteral("X"), positionX_});
                valueRows_.push_back({QStringLiteral("Y"), positionY_});
            } else if (role == document::kOpacityParameterRole) {
                opacity_ = makeCardField(QStringLiteral("nodeOpacityEditor"), tr("Opacity"), 0.0,
                                         100.0, 1, QStringLiteral("%"));
                addProxy(opacity_);
                connect(opacity_, &kit::KValueField::valueChanged, this,
                        [this] { commitOpacity(); });
                valueRows_.push_back({tr("Opacity"), opacity_});
            } else if (role == document::kSolidColorParameterRole) {
                colorChip_ = new kit::KColorChip;
                colorChip_->setObjectName(QStringLiteral("nodeColorChip"));
                colorChip_->setAccessibleName(tr("Solid color"));
                colorChip_->setControlSize(kit::KColorChip::ControlSize::Compact);
                colorChip_->setEnabled(false);
                colorChip_->resize(colorChip_->sizeHint());
                addProxy(colorChip_);
                colorRowLabel_ = tr("Color");
            } else {
                readOnlyRows_.push_back({displayTypeName(role), QString{}});
            }
        }
        builtRoles_ = std::move(roles);
        fieldsBuilt_ = true;
    }

    void refreshValues(const document::NodeRecord& node, const document::Composition& composition) {
        refreshing_ = true;
        const auto describe = [](const document::ParameterRecord* parameter,
                                 const QString& absent) {
            return parameter == nullptr ? absent : parameterSourceDescription(*parameter);
        };

        if (positionX_ != nullptr && positionY_ != nullptr) {
            const auto* parameter =
                parameterForRole(node, composition, document::kPositionParameterRole);
            const auto value = parameter == nullptr || session_ == nullptr
                                   ? std::nullopt
                                   : session_->constantVec2Value(parameter->id);
            const QString tip = describe(parameter, tr("Position is not exposed by this node"));
            for (auto* field : {positionX_, positionY_}) {
                field->setEnabled(value.has_value());
                field->setToolTip(tip);
            }
            if (value.has_value()) {
                const QSignalBlocker blockX(positionX_);
                const QSignalBlocker blockY(positionY_);
                positionX_->setValue(value->x);
                positionY_->setValue(value->y);
            }
        }

        if (opacity_ != nullptr) {
            const auto* parameter =
                parameterForRole(node, composition, document::kOpacityParameterRole);
            const auto value = parameter == nullptr || session_ == nullptr
                                   ? std::nullopt
                                   : session_->constantValue(parameter->id);
            opacity_->setEnabled(value.has_value());
            opacity_->setToolTip(describe(parameter, tr("Opacity is not exposed by this node")));
            const QSignalBlocker blocker(opacity_);
            opacity_->setValue(value.has_value() ? *value * 100.0 : 100.0);
        }

        if (colorChip_ != nullptr) {
            const auto* parameter =
                parameterForRole(node, composition, document::kSolidColorParameterRole);
            const auto value = parameter == nullptr || session_ == nullptr
                                   ? std::nullopt
                                   : session_->constantColorValue(parameter->id);
            if (value.has_value()) {
                colorChip_->setColor(kit::KColor::fromRgba(
                    static_cast<float>(value->red), static_cast<float>(value->green),
                    static_cast<float>(value->blue), static_cast<float>(value->alpha)));
            }
            // The swatch quantizes to 8 bits and clamps, so an HDR or negative authoring channel
            // cannot be shown in it honestly; the exact, unclipped value travels in the tooltip
            // alongside the reason the chip does not open a picker.
            const QString exact =
                value.has_value() ? exactColorText(*value) : describe(parameter, tr("No color"));
            colorChip_->setToolTip(tr("%1\nRead-only: no command sets a color yet").arg(exact));
        }

        std::size_t readOnlyIndex = 0;
        for (const auto& binding : node.parameters) {
            if (binding.role == document::kPositionParameterRole ||
                binding.role == document::kOpacityParameterRole ||
                binding.role == document::kSolidColorParameterRole) {
                continue;
            }
            const auto* parameter = composition.parameters().find(binding.parameterId);
            if (readOnlyIndex < readOnlyRows_.size()) {
                readOnlyRows_[readOnlyIndex].second =
                    parameter == nullptr ? QString{} : parameterText(*parameter);
            }
            ++readOnlyIndex;
        }
        refreshing_ = false;
    }

    // Recomputes the card's own extent from what it actually carries -- the header text, the widest
    // row label, and the widest control -- rather than from a spelled card width, then positions
    // each proxy inside it.
    void relayout() {
        const QFontMetricsF headerMetrics(kit::font(kit::TypeRole::UiSmall));
        const QFontMetricsF rowMetrics(kit::font(kit::TypeRole::UiSmall));
        const QFontMetricsF valueMetrics(kit::font(kit::TypeRole::Value));

        qreal labelColumn = 0.0;
        qreal controlColumn = 0.0;
        qreal rowHeight = 0.0;
        // sizeHint(), never the CURRENT width: a control is stretched to the card's own control
        // column below, so measuring its live width here would feed the card's width back into
        // itself and make the layout depend on how many times it had been run.
        for (const auto& row : valueRows_) {
            labelColumn = std::max(labelColumn, rowMetrics.horizontalAdvance(row.label));
            controlColumn =
                std::max(controlColumn, static_cast<qreal>(row.field->sizeHint().width()));
            rowHeight = std::max(rowHeight, static_cast<qreal>(row.field->sizeHint().height()));
        }
        if (colorChip_ != nullptr) {
            labelColumn = std::max(labelColumn, rowMetrics.horizontalAdvance(colorRowLabel_));
            controlColumn =
                std::max(controlColumn, static_cast<qreal>(colorChip_->sizeHint().width()));
            rowHeight = std::max(rowHeight, static_cast<qreal>(colorChip_->sizeHint().height()));
        }
        for (const auto& [label, value] : readOnlyRows_) {
            labelColumn = std::max(labelColumn, rowMetrics.horizontalAdvance(label));
            controlColumn = std::max(controlColumn, valueMetrics.horizontalAdvance(value));
            rowHeight = std::max(rowHeight, valueMetrics.height());
        }
        if (rowHeight <= 0.0) {
            rowHeight = kit::px(kit::Size::Control);
        }

        const auto rowCount = static_cast<qreal>(valueRows_.size() + readOnlyRows_.size() +
                                                 (colorChip_ != nullptr ? 1 : 0));
        const qreal contentWidth =
            rowCount > 0.0 ? labelColumn + kCardLabelGap + controlColumn : 0.0;
        const qreal width =
            std::max({kCardMinimumWidth, 2.0 * kCardPadding + contentWidth,
                      2.0 * kCardPadding + headerMetrics.horizontalAdvance(title_)});
        // A card with no parameter rows is exactly its header: no empty body lip below it, which
        // would read as a clipped row rather than as a node that simply has nothing to edit.
        const qreal height =
            kCardHeaderHeight +
            (rowCount > 0.0 ? rowCount * (rowHeight + kCardRowGap) - kCardRowGap + kCardPadding
                            : 0.0);

        if (!qFuzzyCompare(width, width_) || !qFuzzyCompare(height, height_)) {
            prepareGeometryChange();
            width_ = width;
            height_ = height;
        }
        labelColumnWidth_ = labelColumn;
        rowHeight_ = rowHeight;

        const qreal controlLeft = kCardPadding + labelColumn + kCardLabelGap;
        const qreal controlSpan = width_ - kCardPadding - controlLeft;
        qreal y = kCardHeaderHeight;
        for (const auto& row : valueRows_) {
            row.field->resize(static_cast<int>(controlSpan), row.field->sizeHint().height());
            positionProxy(row.field, controlLeft, y + (rowHeight - row.field->height()) / 2.0);
            y += rowHeight + kCardRowGap;
        }
        if (colorChip_ != nullptr) {
            colorChip_->resize(colorChip_->sizeHint());
            // The color row is the last row carrying a real widget -- read-only rows below it are
            // painted, not positioned -- so `y` is deliberately not advanced again here.
            positionProxy(colorChip_, controlLeft, y + (rowHeight - colorChip_->height()) / 2.0);
        }
        update();
    }

    void positionProxy(const QWidget* widget, const qreal x, const qreal y) {
        for (auto* child : childItems()) {
            auto* proxy = qgraphicsitem_cast<QGraphicsProxyWidget*>(child);
            if (proxy != nullptr && proxy->widget() == widget) {
                proxy->setPos(x, y);
                return;
            }
        }
    }

    document::NodeId id_;
    CompositionSession* session_ = nullptr;
    QString title_;
    qreal width_ = kCardMinimumWidth;
    qreal height_ = kCardHeaderHeight + kCardPadding;
    qreal labelColumnWidth_ = 0.0;
    qreal rowHeight_ = kit::px(kit::Size::Control);
    bool hasInputSocket_ = false;
    bool hasOutputSocket_ = false;
    bool fieldsBuilt_ = false;
    bool refreshing_ = false;
    std::vector<std::string> builtRoles_;
    std::vector<ValueRow> valueRows_;
    std::vector<std::pair<QString, QString>> readOnlyRows_;
    kit::KValueField* positionX_ = nullptr;
    kit::KValueField* positionY_ = nullptr;
    kit::KValueField* opacity_ = nullptr;
    kit::KColorChip* colorChip_ = nullptr;
    QString colorRowLabel_;
    QGraphicsDropShadowEffect* dragShadow_ = nullptr;
    std::vector<NodeEdgeItem*> edges_;
};

// A typed wire. Hairline by default, brightened while hovered or while either endpoint card is
// selected -- the same "one surface step brighter" idea the kit's own hover recipe states, applied
// to ink instead of a fill (kit::hoverFillFor() is that recipe, and is reused verbatim rather than
// picking a second highlight color).
class NodeEdgeItem final : public QGraphicsPathItem {
  public:
    NodeEdgeItem(NodeItem& source, NodeItem& destination)
        : source_(source), destination_(destination) {
        setZValue(-1.0);
        setAcceptHoverEvents(true);
        source_.addEdge(*this);
        destination_.addEdge(*this);
        updatePath();
    }

    void updatePath() {
        const QPointF start = source_.outputAnchor();
        const QPointF end = destination_.inputAnchor();
        // A horizontal-tangent cubic: the handles reach along x only, so a wire always leaves an
        // output and enters an input horizontally no matter where the two cards sit.
        const qreal handle =
            std::max<qreal>(kCardMinimumWidth / 2.0, std::abs(end.x() - start.x()) / 2.0);
        QPainterPath curve(start);
        curve.cubicTo(start + QPointF(handle, 0.0), end - QPointF(handle, 0.0), end);
        setPath(curve);
    }

    void paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*) override {
        const QColor base = kit::color(socketColorToken(kGraphTransportKind));
        const bool emphasized = hovered_ || source_.isSelected() || destination_.isSelected();
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setBrush(Qt::NoBrush);
        painter->setPen(QPen(emphasized ? kit::hoverFillFor(base) : base, kit::kHairlineWidth,
                             Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter->drawPath(path());
    }

  protected:
    void hoverEnterEvent(QGraphicsSceneHoverEvent* event) override {
        hovered_ = true;
        update();
        QGraphicsPathItem::hoverEnterEvent(event);
    }

    void hoverLeaveEvent(QGraphicsSceneHoverEvent* event) override {
        hovered_ = false;
        update();
        QGraphicsPathItem::hoverLeaveEvent(event);
    }

  private:
    NodeItem& source_;
    NodeItem& destination_;
    bool hovered_ = false;
};

void NodeItem::paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget*) {
    const QRectF bounds = cardRect();
    const bool selected = option->state.testFlag(QStyle::State_Selected);
    painter->setRenderHint(QPainter::Antialiasing, true);

    // SurfaceRaised card + hairline Border, then the Accent inset edge when selected.
    kit::fillRoundedSurface(*painter, bounds, kit::color(kit::Color::SurfaceRaised),
                            kit::color(kit::Color::Border), kCardRadius);
    if (selected) {
        const qreal inset = kSelectionEdgeWidth / 2.0;
        painter->setBrush(Qt::NoBrush);
        painter->setPen(QPen(kit::color(kit::Color::Accent), kSelectionEdgeWidth));
        const auto radius = static_cast<qreal>(kit::radiusPx(
            kCardRadius, static_cast<int>(std::min(bounds.width(), bounds.height()))));
        painter->drawRoundedRect(bounds.adjusted(inset, inset, -inset, -inset), radius, radius);
    }

    // The header strip: one step down the surface ladder from the card so it reads as chrome, with
    // its own hairline foot rather than a second rounded rectangle.
    QPainterPath header;
    // Winding, not QPainterPath's default odd-even rule: the header is a rounded rectangle UNION a
    // square strip that squares off its bottom corners, and the two subpaths overlap. Under
    // odd-even the overlap cancels and the strip is left unpainted -- a dark band across the bottom
    // of every header, which is exactly what the pre-U4 projection drew.
    header.setFillRule(Qt::WindingFill);
    const auto headerRadius = static_cast<qreal>(
        kit::radiusPx(kCardRadius, static_cast<int>(std::min(bounds.width(), kCardHeaderHeight))));
    header.addRoundedRect(QRectF(0.0, 0.0, bounds.width(), kCardHeaderHeight), headerRadius,
                          headerRadius);
    header.addRect(QRectF(0.0, kCardHeaderHeight - headerRadius, bounds.width(), headerRadius));
    painter->setPen(Qt::NoPen);
    painter->fillPath(header, kit::color(kit::Color::Surface));
    kit::applyHairlinePen(*painter, kit::color(kit::Color::Border));
    painter->drawLine(QPointF(0.0, kCardHeaderHeight), QPointF(bounds.width(), kCardHeaderHeight));

    painter->setFont(kit::font(kit::TypeRole::UiSmall));
    painter->setPen(kit::color(kit::Color::Foreground));
    const QRectF titleRect(kCardPadding, 0.0, bounds.width() - 2.0 * kCardPadding,
                           kCardHeaderHeight);
    painter->drawText(
        titleRect, Qt::AlignVCenter | Qt::AlignLeft,
        QFontMetricsF(painter->font()).elidedText(title_, Qt::ElideRight, titleRect.width()));

    // Port dots, in the transport's own token color, centered on the header's mid-line. A dot is
    // drawn only where the graph actually connects: nothing in src/document exposes a node's
    // declared-but-unconnected ports to src/ui, so drawing a socket that no edge reaches would be a
    // guess rather than a projection.
    painter->setPen(Qt::NoPen);
    painter->setBrush(kit::color(socketColorToken(kGraphTransportKind)));
    const qreal socketY = kCardHeaderHeight / 2.0;
    const qreal socketRadius = kSocketDiameter / 2.0;
    if (hasInputSocket_) {
        painter->drawEllipse(QPointF(0.0, socketY), socketRadius, socketRadius);
    }
    if (hasOutputSocket_) {
        painter->drawEllipse(QPointF(bounds.width(), socketY), socketRadius, socketRadius);
    }

    // Row labels. The controls themselves are real kit widgets in proxies; only their names are
    // painted here, in the shared right-aligned label column.
    painter->setFont(kit::font(kit::TypeRole::UiSmall));
    qreal y = kCardHeaderHeight;
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

} // namespace

// --- NodeGraphicsScene -------------------------------------------------------------------------

NodeGraphicsScene::NodeGraphicsScene(QObject* parent) : QGraphicsScene(parent) {
    setObjectName("nodeGraphicsScene");
    setBackgroundBrush(kit::color(kit::Color::Background));
    setItemIndexMethod(QGraphicsScene::BspTreeIndex);
}

void NodeGraphicsScene::setSession(CompositionSession* session) { session_ = session; }

void NodeGraphicsScene::drawBackground(QPainter* painter, const QRectF& rect) {
    // The canvas surface itself stays the scene's own Background brush; the grid is drawn on top of
    // it, one step up the surface ladder, so it reads as texture rather than as a second color.
    QGraphicsScene::drawBackground(painter, rect);

    const qreal scale = painter->worldTransform().m11();
    if (!(scale > 0.0) || kGridSpacing * scale < kGridMinimumDeviceSpacing) {
        return;
    }
    // Integer step counts, not a floating-point loop variable: repeatedly adding a pitch to a
    // double accumulates error across a wide exposed rectangle, and the dots would slowly drift off
    // the lattice the far side of the canvas is drawn on.
    const qreal first = std::floor(rect.left() / kGridSpacing) * kGridSpacing;
    const qreal top = std::floor(rect.top() / kGridSpacing) * kGridSpacing;
    const auto columns = static_cast<int>(std::floor((rect.right() - first) / kGridSpacing)) + 1;
    const auto lines = static_cast<int>(std::floor((rect.bottom() - top) / kGridSpacing)) + 1;
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setPen(Qt::NoPen);
    painter->setBrush(kit::color(kit::surfaceStep(kit::Color::Background, 2)));
    for (int column = 0; column < columns; ++column) {
        const qreal x = first + static_cast<qreal>(column) * kGridSpacing;
        for (int line = 0; line < lines; ++line) {
            const qreal y = top + static_cast<qreal>(line) * kGridSpacing;
            painter->drawEllipse(QPointF(x, y), kGridDotRadius, kGridDotRadius);
        }
    }
    painter->restore();
}

void NodeGraphicsScene::setProjection(const document::Snapshot& snapshot,
                                      const document::CompositionId compositionId) {
    const auto* composition = snapshot.project().findComposition(compositionId);
    if (composition == nullptr) {
        clear();
        setSceneRect({});
        return;
    }

    std::map<std::uint64_t, NodeItem*> existing;
    std::vector<QGraphicsItem*> staleEdges;
    for (auto* item : items()) {
        if (auto* node = dynamic_cast<NodeItem*>(item)) {
            existing.emplace(node->id().value(), node);
        } else if (dynamic_cast<NodeEdgeItem*>(item) != nullptr) {
            staleEdges.push_back(item);
        }
    }
    // Edges carry no widget state and no artist-owned position, so they are always rebuilt; node
    // cards are not, because theirs is exactly the state this reconciliation exists to preserve.
    for (auto* edge : staleEdges) {
        removeItem(edge);
        delete edge;
    }
    for (const auto& [id, item] : existing) {
        item->clearEdges();
        item->clearSockets();
    }

    std::array<int, 4> rows{};
    std::vector<std::uint64_t> present;
    present.reserve(composition->graph().nodes().size());
    for (const auto& node : composition->graph().nodes()) {
        const std::size_t column = layoutColumn(node);
        const auto found = existing.find(node.id.value());
        NodeItem* item = nullptr;
        if (found != existing.end()) {
            item = found->second;
        } else {
            item = new NodeItem(node.id, session_);
            addItem(item);
            item->setPos(kNodeColumnOrigin + static_cast<qreal>(column) * kNodeColumnPitch,
                         kNodeColumnOrigin + static_cast<qreal>(rows.at(column)) * kNodeRowPitch);
        }
        // Counted for reused cards too, so a node added later lands BELOW the column's existing
        // cards instead of on top of the first one.
        ++rows.at(column);
        item->refresh(node, *composition);
        present.push_back(node.id.value());
    }

    for (const auto& [id, item] : existing) {
        if (std::ranges::find(present, id) == present.end()) {
            removeItem(item);
            // deleteLater(), not delete: a card can be dropped from inside one of its own field
            // widgets' signal emission (an edit that removes the node), and unwinding through a
            // freed widget is not something a projection may risk.
            item->deleteLater();
        }
    }

    rebuildEdges(*composition);
    const QRectF bounds = itemsBoundingRect();
    setSceneRect(bounds.isEmpty() ? QRectF(-kNodeSceneMargin, -kNodeSceneMargin,
                                           kNodeSceneMargin * 2.0, kNodeSceneMargin * 2.0)
                                  : bounds.adjusted(-kNodeSceneMargin, -kNodeSceneMargin,
                                                    kNodeSceneMargin, kNodeSceneMargin));
}

QGraphicsItem* NodeGraphicsScene::findNodeItem(const document::NodeId nodeId) const {
    const auto matching = items();
    const auto found = std::ranges::find_if(matching, [nodeId](const auto* item) {
        return item->data(kNodeItemKindRole).toString() == QStringLiteral("node") &&
               item->data(kNodeStableIdRole).toULongLong() == nodeId.value();
    });
    return found == matching.end() ? nullptr : *found;
}

QWidget* NodeGraphicsScene::nodeFieldForTest(const document::NodeId nodeId,
                                             const QString& fieldObjectName) const {
    auto* item = dynamic_cast<NodeItem*>(findNodeItem(nodeId));
    return item == nullptr ? nullptr : item->fieldWidget(fieldObjectName);
}

NodeSockets NodeGraphicsScene::nodeSocketsForTest(const document::NodeId nodeId) const {
    const auto* item = dynamic_cast<const NodeItem*>(findNodeItem(nodeId));
    return item == nullptr ? NodeSockets{}
                           : NodeSockets{item->hasInputSocket(), item->hasOutputSocket()};
}

void NodeGraphicsScene::rebuildEdges(const document::Composition& composition) {
    for (const auto& edge : composition.graph().edges()) {
        auto* source = dynamic_cast<NodeItem*>(findNodeItem(edge.source.nodeId));
        auto* destination =
            dynamic_cast<NodeItem*>(findNodeItem(destinationNodeId(edge.destination)));
        if (source != nullptr && destination != nullptr) {
            addItem(new NodeEdgeItem(*source, *destination));
            source->markOutputSocket();
            destination->markInputSocket();
        }
    }
}

// --- NodeGraphicsView --------------------------------------------------------------------------

NodeGraphicsView::NodeGraphicsView(QWidget* parent) : QGraphicsView(parent) {
    setObjectName("nodeGraphicsView");
    setAccessibleName(tr("Composition node graph"));
    setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
    setDragMode(QGraphicsView::RubberBandDrag);
    setViewportUpdateMode(QGraphicsView::MinimalViewportUpdate);
    setFrameShape(QFrame::NoFrame);
    // Zoom/pan live entirely in the view transform (see the class comment): with scrolling off and
    // a top-left alignment, viewportTransform() has no scroll offset folded into it, so
    // sceneFromViewport() is an exact inverse and a zoom step's cursor invariant is exact.
    setTransformationAnchor(QGraphicsView::NoAnchor);
    setResizeAnchor(QGraphicsView::NoAnchor);
    setAlignment(Qt::AlignLeft | Qt::AlignTop);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // The view's OWN scene rectangle, which overrides the scene's for scrolling purposes -- see
    // kCanvasHalfExtent for why it is fixed and oversized rather than tracking the graph.
    setSceneRect(-kCanvasHalfExtent, -kCanvasHalfExtent, kCanvasHalfExtent * 2.0,
                 kCanvasHalfExtent * 2.0);
    // StrongFocus so Space/F/Z reach the canvas without a prior click -- the Viewer's own rule.
    setFocusPolicy(Qt::StrongFocus);
}

double NodeGraphicsView::zoomFactor() const noexcept { return transform().m11(); }

bool NodeGraphicsView::viewAdjusted() const noexcept { return viewAdjusted_; }

QPointF NodeGraphicsView::sceneFromViewport(const QPointF viewportPoint) const {
    return viewportTransform().inverted().map(viewportPoint);
}

void NodeGraphicsView::zoomAboutViewportPoint(const QPointF viewportPoint, const double factor) {
    const double current = zoomFactor();
    if (!(current > 0.0) || !(factor > 0.0)) {
        return;
    }
    const double target =
        std::clamp(current * factor, ViewTransform::kMinZoom, ViewTransform::kMaxZoom);
    const double applied = target / current;
    if (qFuzzyCompare(applied, 1.0)) {
        viewAdjusted_ = true;
        return;
    }
    // Scale about `viewportPoint` in VIEWPORT space, composed after the current transform: shift
    // the point to the origin, scale, shift back. Qt maps row vectors (p * M), so "apply A then B"
    // is A * B -- the composition order below is exactly that, and nothing about it depends on a
    // scroll bar's value or range.
    QTransform about = QTransform::fromTranslate(-viewportPoint.x(), -viewportPoint.y());
    about *= QTransform::fromScale(applied, applied);
    about *= QTransform::fromTranslate(viewportPoint.x(), viewportPoint.y());
    setTransform(transform() * about);
    viewAdjusted_ = true;
}

void NodeGraphicsView::zoomStep(const int notches) {
    if (notches == 0) {
        return;
    }
    zoomAboutViewportPoint(QRectF(viewport()->rect()).center(), std::pow(kZoomStepFactor, notches));
}

QRectF NodeGraphicsView::graphBounds() const {
    return scene() == nullptr ? QRectF() : scene()->itemsBoundingRect();
}

bool NodeGraphicsView::applyCenteredScale(const double scale) {
    const QRectF bounds = graphBounds();
    const QRectF view = QRectF(viewport()->rect());
    if (bounds.isEmpty() || view.isEmpty()) {
        return false;
    }
    const double clamped = std::clamp(scale, ViewTransform::kMinZoom, ViewTransform::kMaxZoom);
    QTransform framed = QTransform::fromTranslate(-bounds.center().x(), -bounds.center().y());
    framed *= QTransform::fromScale(clamped, clamped);
    framed *= QTransform::fromTranslate(view.center().x(), view.center().y());
    setTransform(framed);
    return true;
}

void NodeGraphicsView::frameGraph() {
    const QRectF bounds = graphBounds();
    const QRectF view = QRectF(viewport()->rect());
    if (bounds.isEmpty() || view.isEmpty()) {
        return;
    }
    if (!applyCenteredScale(
            std::min(view.width() / bounds.width(), view.height() / bounds.height()))) {
        return;
    }
    // Fit means "keep framing everything": a later projection rebuild re-frames until the artist
    // moves the view themselves.
    viewAdjusted_ = false;
    framedOnce_ = true;
}

void NodeGraphicsView::zoomToActualSize() {
    // A gesture that could not move the view does not count as the artist having taken the view
    // over. Latching the flag on an empty canvas -- no composition, or a collapsed panel -- would
    // permanently stop the auto-framing that a rebuild and a resize depend on, and the canvas would
    // never frame the content that arrived afterwards.
    if (!applyCenteredScale(1.0)) {
        return;
    }
    viewAdjusted_ = true;
    framedOnce_ = true;
}

void NodeGraphicsView::showEvent(QShowEvent* event) {
    QGraphicsView::showEvent(event);
    if (!framedOnce_ && !viewAdjusted_) {
        frameGraph();
    }
}

void NodeGraphicsView::resizeEvent(QResizeEvent* event) {
    QGraphicsView::resizeEvent(event);
    // The Viewer's Fit recomputes its rectangle from the available area on every paint, so a
    // resized Viewer stays fitted. This canvas keeps that behavior for the same state: while the
    // artist has not moved the view, a resize re-frames the graph. Once they have, the view is
    // theirs and a resize leaves it exactly where they put it.
    if (!viewAdjusted_) {
        frameGraph();
    }
}

void NodeGraphicsView::contextMenuEvent(QContextMenuEvent* event) {
    emit contextMenuRequested(event->pos());
    event->accept();
}

void NodeGraphicsView::wheelEvent(QWheelEvent* event) {
    if (panActive_) {
        event->ignore();
        return;
    }
    // The wheel ALWAYS zooms the canvas, including over an in-node field. The canvas's
    // zoom-about-cursor rule is decision 1's contract and an artist reaching for the wheel over a
    // dense graph means "zoom" every time; a field that silently ate the gesture because it
    // happened to hold focus would make the canvas feel broken in exactly the places it is densest.
    // The field loses nothing it needs: its own steppers, and Up/Down/PageUp/PageDown while
    // focused (kit/value_field.cpp), still step it.
    const int notches = event->angleDelta().y() / kWheelDetent;
    if (notches == 0) {
        QGraphicsView::wheelEvent(event);
        return;
    }
    zoomAboutViewportPoint(event->position(), std::pow(kZoomStepFactor, notches));
    event->accept();
}

void NodeGraphicsView::mousePressEvent(QMouseEvent* event) {
    if (!panActive_ && (event->button() == Qt::MiddleButton ||
                        (event->button() == Qt::LeftButton && spaceHeld_))) {
        panActive_ = true;
        panButton_ = event->button();
        panOrigin_ = event->position();
        panBaseTransform_ = transform();
        setDragMode(QGraphicsView::NoDrag);
        setFocus(Qt::MouseFocusReason);
        updatePanCursor();
        event->accept();
        return;
    }
    QGraphicsView::mousePressEvent(event);
}

void NodeGraphicsView::mouseMoveEvent(QMouseEvent* event) {
    if (!panActive_) {
        QGraphicsView::mouseMoveEvent(event);
        return;
    }
    // TOTAL displacement from the press point applied to the transform frozen there -- never a
    // chain of per-move deltas (the Viewer's own pan rule).
    const QPointF delta = event->position() - panOrigin_;
    setTransform(panBaseTransform_ * QTransform::fromTranslate(delta.x(), delta.y()));
    viewAdjusted_ = true;
    event->accept();
}

void NodeGraphicsView::mouseReleaseEvent(QMouseEvent* event) {
    if (panActive_ && event->button() == panButton_) {
        panActive_ = false;
        panButton_ = Qt::NoButton;
        setDragMode(QGraphicsView::RubberBandDrag);
        updatePanCursor();
        event->accept();
        return;
    }
    QGraphicsView::mouseReleaseEvent(event);
}

void NodeGraphicsView::keyPressEvent(QKeyEvent* event) {
    if (!panActive_) {
        if (!event->isAutoRepeat() && event->key() == Qt::Key_Space) {
            spaceHeld_ = true;
            updatePanCursor();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Z) {
            zoomToActualSize();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_F) {
            frameGraph();
            event->accept();
            return;
        }
    }
    QGraphicsView::keyPressEvent(event);
}

void NodeGraphicsView::keyReleaseEvent(QKeyEvent* event) {
    if (!event->isAutoRepeat() && event->key() == Qt::Key_Space) {
        spaceHeld_ = false;
        updatePanCursor();
        event->accept();
        return;
    }
    QGraphicsView::keyReleaseEvent(event);
}

void NodeGraphicsView::updatePanCursor() {
    if (panActive_) {
        viewport()->setCursor(Qt::ClosedHandCursor);
    } else if (spaceHeld_) {
        viewport()->setCursor(Qt::OpenHandCursor);
    } else {
        viewport()->unsetCursor();
    }
}

// --- NodeGraphEditor ---------------------------------------------------------------------------

NodeGraphEditor::NodeGraphEditor(CompositionSession& session, QWidget* parent)
    : QWidget(parent), session_(session) {
    setObjectName("nodeGraphEditor");
    setAccessibleName(tr("Nodes editor"));

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    scene_ = new NodeGraphicsScene(this);
    scene_->setSession(&session_);
    view_ = new NodeGraphicsView(this);
    view_->setScene(scene_);
    layout->addWidget(view_);

    connect(&session_, &CompositionSession::snapshotChanged, this, &NodeGraphEditor::rebuild);
    connect(&session_, &CompositionSession::compositionChanged, this, &NodeGraphEditor::rebuild);
    connect(&session_, &CompositionSession::selectionChanged, this,
            &NodeGraphEditor::updateSelection);
    connect(scene_, &QGraphicsScene::selectionChanged, this,
            &NodeGraphEditor::sceneSelectionChanged);
    connect(view_, &NodeGraphicsView::contextMenuRequested, this,
            &NodeGraphEditor::showContextMenu);

    rebuild();
}

NodeGraphEditor::~NodeGraphEditor() { scene_->blockSignals(true); }

NodeGraphicsScene* NodeGraphEditor::graphScene() const noexcept { return scene_; }

NodeGraphicsView* NodeGraphEditor::graphView() const noexcept { return view_; }

void NodeGraphEditor::rebuild() {
    rebuilding_ = true;
    scene_->setProjection(session_.snapshot(), session_.compositionId());
    updateSelection();
    rebuilding_ = false;
    if (!view_->viewAdjusted()) {
        view_->frameGraph();
    }
}

void NodeGraphEditor::updateSelection() {
    const bool wasRebuilding = rebuilding_;
    rebuilding_ = true;
    scene_->clearSelection();

    std::optional<document::NodeId> selectedNode;
    if (const auto* nodeId = std::get_if<document::NodeId>(&session_.selection().primary)) {
        selectedNode = *nodeId;
    } else if (const auto* layerId =
                   std::get_if<document::LayerId>(&session_.selection().primary)) {
        selectedNode = session_.boundaryNodeForLayer(*layerId);
    }
    if (selectedNode.has_value()) {
        if (auto* item = scene_->findNodeItem(*selectedNode)) {
            item->setSelected(true);
        }
    }
    rebuilding_ = wasRebuilding;
}

void NodeGraphEditor::sceneSelectionChanged() {
    if (rebuilding_) {
        return;
    }
    auto* item = firstNodeItem(scene_->selectedItems());
    if (item == nullptr) {
        session_.clearSelection();
        return;
    }
    session_.selectNode(item->id());
}

QMenu* NodeGraphEditor::buildContextMenu(QWidget* parent) {
    // Kit-styled through the application-wide QMenu stylesheet rule every other Bloom menu already
    // picks up (the Timeline's Add menu, the Viewer's canvas menu) -- no per-menu styling here.
    auto* menu = new QMenu(parent);
    menu->setObjectName(QStringLiteral("nodeCanvasMenu"));
    menu->setAccessibleName(tr("Node graph menu"));

    auto* addMenu = menu->addMenu(tr("Add"));
    addMenu->setObjectName(QStringLiteral("nodeAddMenu"));
    addMenu->setAccessibleName(tr("Add layer menu"));
    addMenu->setEnabled(session_.composition() != nullptr);

    auto* addSolidAction = addMenu->addAction(tr("Solid"));
    addSolidAction->setObjectName(QStringLiteral("nodeAddSolidLayerAction"));
    addSolidAction->setToolTip(
        tr("Add a solid using the next built-in reference-linear-sRGB proof color"));
    connect(addSolidAction, &QAction::triggered, this,
            [this] { (void)addDefaultSolidLayer(session_); });

    auto* addTextAction = addMenu->addAction(tr("Text"));
    addTextAction->setObjectName(QStringLiteral("nodeAddTextLayerAction"));
    addTextAction->setToolTip(tr("Add a text layer"));
    connect(addTextAction, &QAction::triggered, this,
            [this] { (void)addDefaultTextLayer(session_); });

    // No Remove/Delete, and no Duplicate. Verified against the WHOLE command layer, not inferred:
    // src/commands/include/bloom/commands/operations.hpp declares AddSolidLayer, AddTextLayer,
    // SetProjectName, SetCompositionName, SetCompositionDuration, SetCompositionFormat,
    // SetParameterSource and MoveLayerBefore, and animation_operations.hpp declares the keyframe/
    // curve operations -- there is no delete-layer, remove-node, duplicate or rename-layer command
    // anywhere, and CompositionSession exposes no such entry point either. The honesty rule says an
    // item that cannot do anything must not appear at all, so neither is drawn (not even disabled),
    // and both are reported as command-layer gaps rather than faked here. Edge dragging/rewiring is
    // absent for the same reason: no connect/disconnect command exists.
    menu->addSeparator();

    auto* fitAction = menu->addAction(tr("Fit"));
    fitAction->setObjectName(QStringLiteral("nodeFitAction"));
    connect(fitAction, &QAction::triggered, view_, &NodeGraphicsView::frameGraph);

    auto* actualSizeAction = menu->addAction(tr("100%"));
    actualSizeAction->setObjectName(QStringLiteral("nodeActualSizeAction"));
    connect(actualSizeAction, &QAction::triggered, view_, &NodeGraphicsView::zoomToActualSize);

    // The same Fit / 100% / Zoom In / Zoom Out set, in the same order and with the same separator,
    // that ViewerEditor::contextMenuEvent() offers -- one application, one canvas menu.
    menu->addSeparator();

    auto* zoomInAction = menu->addAction(tr("Zoom In"));
    zoomInAction->setObjectName(QStringLiteral("nodeZoomInAction"));
    connect(zoomInAction, &QAction::triggered, view_, [this] { view_->zoomStep(1); });

    auto* zoomOutAction = menu->addAction(tr("Zoom Out"));
    zoomOutAction->setObjectName(QStringLiteral("nodeZoomOutAction"));
    connect(zoomOutAction, &QAction::triggered, view_, [this] { view_->zoomStep(-1); });

    return menu;
}

QMenu* NodeGraphEditor::contextMenuForTest() { return buildContextMenu(this); }

void NodeGraphEditor::showContextMenu(const QPoint& viewportPosition) {
    // A right-click ON a card selects it first, through the same session selection a left-click
    // routes through -- never a menu-local "context target" that could disagree with what the rest
    // of the application thinks is selected.
    if (auto* node = nodeItemAncestor(view_->itemAt(viewportPosition))) {
        session_.selectNode(node->id());
    }
    // QPointer, because exec() runs a nested event loop: anything that closed this panel from
    // inside the menu would take the menu (its child) with it, and the cleanup below would then be
    // touching freed memory. Nothing the menu currently offers can do that; the guard is what keeps
    // that true when something later can.
    const QPointer<QMenu> menu = buildContextMenu(view_);
    menu->exec(view_->viewport()->mapToGlobal(viewportPosition));
    if (!menu.isNull()) {
        menu->deleteLater();
    }
}

} // namespace bloom::ui
