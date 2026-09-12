#pragma once
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

#include <QLineEdit>
#include <bloom/ui/kit/icons.hpp>
namespace bloom::ui::node_editor {
inline constexpr qreal kCardPadding = kit::px(kit::Spacing::S);
inline constexpr qreal kCardLabelGap = kit::px(kit::Spacing::S);
inline constexpr qreal kCardRowGap = kit::px(kit::Spacing::XXS);
inline constexpr qreal kCardHeaderHeight = kit::px(kit::Size::PanelHeader);
inline constexpr auto kCardRadius = kit::Radius::Medium;
inline constexpr qreal kCardMinimumWidth = 128.0;
inline constexpr qreal kSocketDiameter = 8.0;
inline constexpr qreal kSocketRowHeight = kit::px(kit::Size::ControlCompact);
inline constexpr qreal kNodeSceneMargin = kit::px(kit::Spacing::XXL) * 2;
inline constexpr qreal kSelectionEdgeWidth = 2.0;
class NodeEdgeItem;
class NodeItem;
QString displayTypeName(std::string_view typeId);
QString nodeDisplayName(const document::Composition& composition, const document::NodeRecord& node);
const document::ParameterRecord* parameterForRole(const document::NodeRecord& node,
                                                  const document::Composition& composition,
                                                  std::string_view role);
QString parameterText(const document::ParameterRecord& parameter);
document::NodeId destinationNodeId(const document::InputPortRef& input);
kit::KValueField* makeCardField(const QString& objectName, const QString& accessibleName,
                                double minimum, double maximum, int decimals, const QString& unit);
NodeItem* nodeItemAncestor(QGraphicsItem* item);
NodeItem* firstNodeItem(const QList<QGraphicsItem*>& items);
QString socketKindName(document::SocketValueKind kind);
QPainterPath linkPath(QPointF start, QPointF end);

class SocketItem final : public QGraphicsItem {
  public:
    SocketItem(document::NodeId node, QString name, document::SocketValueKind kind,
               std::optional<document::InputPortRef> input,
               std::optional<document::OutputPortRef> output, bool structural,
               QGraphicsItem* parent);
    [[nodiscard]] QRectF boundingRect() const override { return {-16, -16, 32, 32}; }
    [[nodiscard]] QPainterPath shape() const override;
    void paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*) override;
    // How much vertical room this socket claims on an expanded card. One ordinary port is one
    // kSocketRowHeight row; the card sums these rather than multiplying by the socket count, so a
    // socket that is taller than a row can exist without the card's body landing on top of it.
    [[nodiscard]] qreal rowHeight() const;
    [[nodiscard]] bool draggable() const {
        return !structural_ && kind == document::SocketValueKind::Image;
    }
    void setAuthoringEnabled(bool enabled);
    QString name;
    document::SocketValueKind kind;
    std::optional<document::InputPortRef> input;
    std::optional<document::OutputPortRef> output;

  protected:
    void hoverEnterEvent(QGraphicsSceneHoverEvent* event) override;
    void hoverLeaveEvent(QGraphicsSceneHoverEvent* event) override;

  private:
    QString description_;
    bool structural_;
    bool hovered_ = false;
};

class NodeItem final : public QGraphicsObject {
  public:
    NodeItem(const document::NodeId id, CompositionSession* session) : id_(id), session_(session) {
        setData(kNodeItemKindRole, QStringLiteral("node"));
        setData(kNodeStableIdRole, QVariant::fromValue<qulonglong>(id.value()));
        setFlags(ItemIsSelectable | ItemSendsGeometryChanges);
        setAcceptHoverEvents(true);
        setCursor(Qt::ArrowCursor);

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
    void
    refresh(const document::NodeRecord& node, const document::Composition& composition,
            const document::NodeLayoutRecord& layout,
            const document::NodeDefinitionRegistry& registry = document::builtInNodeDefinitions()) {
        layout_ = layout;
        setData(kNodeMutedRole, layout.muted);
        setData(kNodeCollapsedRole, layout.collapsed);
        title_ = nodeDisplayName(composition, node);
        setToolTip(QStringLiteral("%1\n%2\nNode %3")
                       .arg(title_, displayTypeName(node.typeId))
                       .arg(id_.value()));
        ensureFields(node);
        buildSockets(node, composition, registry);
        refreshValues(node, composition);
        relayout();
    }

    [[nodiscard]] bool hasInputSocket() const {
        return std::ranges::any_of(sockets_,
                                   [](const auto* socket) { return socket->input.has_value(); });
    }
    [[nodiscard]] bool hasOutputSocket() const {
        return std::ranges::any_of(sockets_,
                                   [](const auto* socket) { return socket->output.has_value(); });
    }
    [[nodiscard]] const std::vector<SocketItem*>& sockets() const { return sockets_; }
    void setPreviewWidth(qreal width);
    [[nodiscard]] qreal cardWidth() const { return width_; }
    // The narrowest this card can be drawn without clipping its own content (task S1, item 2):
    // recomputed by every relayout() from the live label column, the narrowest usable control and
    // the widest socket name. A resize gesture clamps against it and a persisted width is raised to
    // it, so there is no width at which the card hides what it carries.
    [[nodiscard]] qreal minimumCardWidth() const { return minimumWidth_; }
    // The shared row-label column the card paints its row names into, and the pitch of one
    // parameter row. Diagnostic accessors: the card's own layout is private, and a test that wants
    // to state "no label is clipped at the minimum width" has to be able to say how wide the label
    // column actually is rather than re-deriving it from a font.
    [[nodiscard]] qreal labelColumnWidth() const { return labelColumnWidth_; }
    [[nodiscard]] qreal parameterRowHeight() const { return rowHeight_; }
    void setPrimary(bool primary) {
        primary_ = primary;
        update();
    }
    void setDragging(bool dragging) {
        if (dragShadow_ != nullptr)
            dragShadow_->setEnabled(dragging);
    }
    void startRename();
    void setAuthoringEnabled(bool enabled) {
        authoringEnabled_ = enabled;
        setCursor(enabled ? Qt::OpenHandCursor : Qt::ArrowCursor);
        for (auto* socket : sockets_)
            socket->setAuthoringEnabled(enabled);
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
    void hoverMoveEvent(QGraphicsSceneHoverEvent* event) override {
        if (authoringEnabled_)
            setCursor(std::abs(event->pos().x() - width_) <= 6 ? Qt::SizeHorCursor
                                                               : Qt::OpenHandCursor);
        QGraphicsObject::hoverMoveEvent(event);
    }

  private:
    void buildSockets(const document::NodeRecord& node, const document::Composition& composition,
                      const document::NodeDefinitionRegistry& registry);

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

    // A widget handed to a QGraphicsProxyWidget becomes a window, and Qt fills a window's own
    // rectangle with the palette's background before the widget paints. Inside a card that fill is
    // an opaque plate behind a control that only paints its own rounded cell, so the corners and
    // every pixel outside the cell read as a darker clipped band. WA_TranslucentBackground is the
    // one attribute that stops it, and it is set here -- once, for every hosted widget -- rather
    // than inside each kit control, because being hosted on a canvas is this card's business and
    // not the control's.
    static void hostTranslucent(QWidget& widget) {
        widget.setAttribute(Qt::WA_TranslucentBackground, true);
        widget.setAttribute(Qt::WA_NoSystemBackground, true);
        widget.setAutoFillBackground(false);
    }

    void addProxy(QWidget* widget) {
        hostTranslucent(*widget);
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
            if (!qgraphicsitem_cast<QGraphicsProxyWidget*>(child) || child == renameProxy_)
                continue;
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
        // The card's own floor, measured from what it actually carries (task S1, item 2): the
        // shared label column, the narrowest usable control beside it, and the widest socket name,
        // each inside the card's padding. A persisted or dragged width never goes below it, so no
        // label, field or socket name is ever clipped by the card that owns it -- which is also why
        // no proxy below needs scaling down any more.
        qreal socketColumn = 0.0;
        for (const auto* socket : sockets_) {
            socketColumn = std::max(socketColumn, rowMetrics.horizontalAdvance(socket->name));
        }
        minimumWidth_ = kCardMinimumWidth;
        if (rowCount > 0.0) {
            minimumWidth_ = std::max(minimumWidth_, kCardPadding + labelColumn + kCardLabelGap +
                                                        controlColumn + kCardPadding);
        }
        if (socketColumn > 0.0) {
            minimumWidth_ = std::max(minimumWidth_, 2.0 * kCardPadding + socketColumn);
        }
        const qreal width = std::max(layout_.width, minimumWidth_);
        qreal socketHeight = 0.0;
        for (const auto* socket : sockets_) {
            socketHeight += socket->rowHeight();
        }
        // A card with no parameter rows is exactly its header: no empty body lip below it, which
        // would read as a clipped row rather than as a node that simply has nothing to edit.
        const qreal height = layout_.collapsed
                                 ? kCardHeaderHeight
                                 : kCardHeaderHeight + socketHeight +
                                       (rowCount > 0.0 ? rowCount * (rowHeight + kCardRowGap) -
                                                             kCardRowGap + kCardPadding
                                                       : 0.0);

        if (!qFuzzyCompare(width, width_) || !qFuzzyCompare(height, height_)) {
            prepareGeometryChange();
            width_ = width;
            height_ = height;
        }
        labelColumnWidth_ = labelColumn;
        rowHeight_ = rowHeight;

        const qreal controlLeft = kCardPadding + labelColumn + kCardLabelGap;
        // CEIL, never truncate: a fractional span rounded down leaves the control a pixel short of
        // the card's own padding, and at a fractional row pitch that gap is exactly the sliver of
        // card surface that made a full-width field look inset.
        const qreal controlSpan = std::max(1.0, std::ceil(width_ - kCardPadding - controlLeft));
        qreal y = kCardHeaderHeight + socketHeight;
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
        for (auto* child : childItems()) {
            auto* proxy = qgraphicsitem_cast<QGraphicsProxyWidget*>(child);
            if (proxy == nullptr || proxy == renameProxy_)
                continue;
            const auto* widget = proxy->widget();
            bool linked = false;
            for (const auto* socket : sockets_) {
                if (!socket->input || !linkedInputs_.contains(socket->name))
                    continue;
                // Future parameter sockets must match the row role; today's schema has only
                // Image transport and therefore cannot drive numeric/color kit fields.
                linked = linked ||
                         (socket->name == QStringLiteral("position") &&
                          (widget == positionX_ || widget == positionY_)) ||
                         (socket->name == QStringLiteral("opacity") && widget == opacity_) ||
                         (socket->name == QStringLiteral("color") && widget == colorChip_);
            }
            proxy->setVisible(!layout_.collapsed && !linked);
            proxy->setOpacity(layout_.muted ? 0.5 : 1.0);
            // Exactly 1, always. A fractional scale resampled a control's own hairlines, padding
            // and text into a blurred, visibly smaller copy of itself -- and it only ever existed
            // to squeeze a control into a card too narrow for it, which minimumWidth_ above now
            // makes impossible.
            proxy->setScale(1.0);
        }
        const auto inputCount = std::ranges::count_if(
            sockets_, [](const auto* socket) { return socket->input.has_value(); });
        const auto outputCount = static_cast<std::ptrdiff_t>(sockets_.size()) - inputCount;
        int inputIndex = 0;
        int outputIndex = 0;
        qreal socketY = kCardHeaderHeight;
        for (auto* socket : sockets_) {
            const bool input = socket->input.has_value();
            const int edgeIndex = input ? inputIndex++ : outputIndex++;
            const qreal rowExtent = socket->rowHeight();
            socket->setPos(input ? 0 : width_,
                           layout_.collapsed
                               ? kCardHeaderHeight * static_cast<qreal>(edgeIndex + 1) /
                                     static_cast<qreal>((input ? inputCount : outputCount) + 1)
                               : socketY + rowExtent / 2.0);
            socketY += rowExtent;
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
    qreal minimumWidth_ = kCardMinimumWidth;
    document::NodeLayoutRecord layout_;
    bool primary_ = false;
    bool authoringEnabled_ = false;
    std::vector<SocketItem*> sockets_;
    std::set<QString> linkedInputs_;
    QGraphicsProxyWidget* renameProxy_ = nullptr;
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

class NodeEdgeItem final : public QGraphicsPathItem {
  public:
    NodeEdgeItem(NodeItem& source, NodeItem& destination, SocketItem& output, SocketItem& input,
                 document::EdgeRecord edge, bool structural);
    void updatePath();
    void paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*) override;
    [[nodiscard]] QPainterPath shape() const override;
    [[nodiscard]] QRectF boundingRect() const override { return shape().boundingRect(); }
    void emphasize(bool enabled) {
        hovered_ = enabled;
        update();
    }
    document::EdgeRecord edge;
    bool structural;

  protected:
    void hoverEnterEvent(QGraphicsSceneHoverEvent* event) override;
    void hoverLeaveEvent(QGraphicsSceneHoverEvent* event) override;

  private:
    NodeItem& source_;
    NodeItem& destination_;
    SocketItem& output_;
    SocketItem& input_;
    bool hovered_ = false;
};

} // namespace bloom::ui::node_editor
