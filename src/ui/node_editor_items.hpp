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

    // Any proxied control, not only a kit::KValueField: task S3's text content row is a QLineEdit,
    // because the kit has no string field and adding one is a kit change outside this task's fence.
    // Everything relayout() needs from a row is its sizeHint(), so QWidget is the honest type here.
    struct ValueRow final {
        QString label;
        QWidget* widget = nullptr;
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

    // The rest of the Layer Output transform, each through exactly the session method
    // PropertiesEditor's matching row calls, so the two surfaces cannot drift. Scale is authored as
    // a percentage on the card exactly as it is in the panel.
    void commitAnchor() {
        if (refreshing_ || anchorX_ == nullptr || anchorY_ == nullptr || !selectSelf()) {
            return;
        }
        (void)session_->setSelectedAnchor(anchorX_->value(), anchorY_->value());
    }

    void commitScale() {
        if (refreshing_ || scaleX_ == nullptr || scaleY_ == nullptr || !selectSelf()) {
            return;
        }
        (void)session_->setSelectedScale(scaleX_->value() / 100.0, scaleY_->value() / 100.0);
    }

    void commitRotation() {
        if (refreshing_ || rotation_ == nullptr || !selectSelf()) {
            return;
        }
        (void)session_->setSelectedRotation(rotation_->value());
    }

    // Task S3's three text writes, each through exactly the session method PropertiesEditor's own
    // Text Source row calls, so the two surfaces cannot drift.
    void commitTextContent() {
        if (refreshing_ || textContent_ == nullptr || !selectSelf()) {
            return;
        }
        (void)session_->setSelectedTextContent(textContent_->text());
    }

    void commitTextSize() {
        if (refreshing_ || textSize_ == nullptr || !selectSelf()) {
            return;
        }
        (void)session_->setSelectedTextSize(textSize_->value());
    }

    // One chip, two schemas: the role string is "color" for both a solid source and a text source
    // (see document::kTextColorParameterRole), so the card builds one control and dispatches on the
    // node's own type only to pick the honest undo label.
    void commitColor(const kit::KColor& color) {
        if (refreshing_ || colorChip_ == nullptr || !selectSelf()) {
            return;
        }
        const core::Color4d value{static_cast<double>(color.red), static_cast<double>(color.green),
                                  static_cast<double>(color.blue),
                                  static_cast<double>(color.alpha)};
        (void)(isTextSource_ ? session_->setSelectedTextColor(value)
                             : session_->setSelectedSolidColor(value));
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
    //   color    -> a KColorChip, committed through CompositionSession::setSelectedSolidColor() or
    //               setSelectedTextColor(). It was read-only while no command set a color; both of
    //               those now exist (SetParameterSource carrying a Color4d constant), so the chip
    //               opens its picker and commits. The swatch's own value model is displayable
    //               straight RGBA in [0, 1], so an HDR or negative authored channel still travels
    //               in the tooltip rather than in the swatch.
    //   text     -> a QLineEdit, committed through setSelectedTextContent() on
    //               editingFinished/returnPressed -- not per keystroke, so typing a word is one
    //               undo step. The kit has no string field; adding one is a kit change outside this
    //               task.
    //   size     -> one KValueField over the text size schema's own domain, committed through
    //               setSelectedTextSize().
    //   anything else -> a painted read-only value row, because no kit primitive carries that value
    //               and no command writes it.
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
        anchorX_ = nullptr;
        anchorY_ = nullptr;
        scaleX_ = nullptr;
        scaleY_ = nullptr;
        rotation_ = nullptr;
        opacity_ = nullptr;
        colorChip_ = nullptr;
        textContent_ = nullptr;
        textSize_ = nullptr;
        colorRowLabel_.clear();
        isTextSource_ = node.typeId == document::kTextSourceNodeType;

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
            } else if (role == document::kAnchorParameterRole) {
                // Range/decimals/step/unit mirror PropertiesEditor's Anchor editors verbatim.
                anchorX_ = makeCardField(QStringLiteral("nodeAnchorXEditor"), tr("Anchor X"),
                                         -1'000'000.0, 1'000'000.0, 2, QStringLiteral("px"));
                anchorY_ = makeCardField(QStringLiteral("nodeAnchorYEditor"), tr("Anchor Y"),
                                         -1'000'000.0, 1'000'000.0, 2, QStringLiteral("px"));
                addProxy(anchorX_);
                addProxy(anchorY_);
                connect(anchorX_, &kit::KValueField::valueChanged, this,
                        [this] { commitAnchor(); });
                connect(anchorY_, &kit::KValueField::valueChanged, this,
                        [this] { commitAnchor(); });
                valueRows_.push_back({tr("Anchor X"), anchorX_});
                valueRows_.push_back({tr("Anchor Y"), anchorY_});
            } else if (role == document::kScaleParameterRole) {
                scaleX_ = makeCardField(QStringLiteral("nodeScaleXEditor"), tr("Scale X"),
                                        -100'000.0, 100'000.0, 2, QStringLiteral("%"));
                scaleY_ = makeCardField(QStringLiteral("nodeScaleYEditor"), tr("Scale Y"),
                                        -100'000.0, 100'000.0, 2, QStringLiteral("%"));
                addProxy(scaleX_);
                addProxy(scaleY_);
                connect(scaleX_, &kit::KValueField::valueChanged, this, [this] { commitScale(); });
                connect(scaleY_, &kit::KValueField::valueChanged, this, [this] { commitScale(); });
                valueRows_.push_back({tr("Scale X"), scaleX_});
                valueRows_.push_back({tr("Scale Y"), scaleY_});
            } else if (role == document::kRotationParameterRole) {
                rotation_ = makeCardField(QStringLiteral("nodeRotationEditor"), tr("Rotation"),
                                          -100'000.0, 100'000.0, 2, QString::fromUtf8("\u00b0"));
                addProxy(rotation_);
                connect(rotation_, &kit::KValueField::valueChanged, this,
                        [this] { commitRotation(); });
                valueRows_.push_back({tr("Rotation"), rotation_});
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
                colorChip_->setAccessibleName(isTextSource_ ? tr("Text color") : tr("Solid color"));
                colorChip_->setControlSize(kit::KColorChip::ControlSize::Compact);
                colorChip_->resize(colorChip_->sizeHint());
                addProxy(colorChip_);
                connect(colorChip_, &kit::KColorChip::colorChanged, this,
                        [this](const kit::KColor& color) { commitColor(color); });
                colorRowLabel_ = tr("Color");
            } else if (role == document::kTextParameterRole) {
                textContent_ = new QLineEdit;
                textContent_->setObjectName(QStringLiteral("nodeTextContentEditor"));
                textContent_->setAccessibleName(tr("Text content"));
                textContent_->setFont(kit::font(kit::TypeRole::Ui));
                textContent_->resize(textContent_->sizeHint());
                addProxy(textContent_);
                connect(textContent_, &QLineEdit::editingFinished, this,
                        [this] { commitTextContent(); });
                valueRows_.push_back({tr("Text"), textContent_});
            } else if (role == document::kTextSizeParameterRole) {
                // Range/decimals/step/unit mirror PropertiesEditor's Size editor verbatim, so the
                // same gesture in either surface produces the same value.
                textSize_ =
                    makeCardField(QStringLiteral("nodeTextSizeEditor"), tr("Text size"), 1.0,
                                  document::kMaximumTextSizePixels, 1, QStringLiteral("px"));
                addProxy(textSize_);
                connect(textSize_, &kit::KValueField::valueChanged, this,
                        [this] { commitTextSize(); });
                valueRows_.push_back({tr("Size"), textSize_});
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

        if (anchorX_ != nullptr && anchorY_ != nullptr) {
            const auto* parameter =
                parameterForRole(node, composition, document::kAnchorParameterRole);
            const auto value = parameter == nullptr || session_ == nullptr
                                   ? std::nullopt
                                   : session_->constantVec2Value(parameter->id);
            const QString tip = describe(parameter, tr("Anchor is not exposed by this node"));
            for (auto* field : {anchorX_, anchorY_}) {
                field->setEnabled(value.has_value());
                field->setToolTip(tip);
            }
            if (value.has_value()) {
                const QSignalBlocker blockX(anchorX_);
                const QSignalBlocker blockY(anchorY_);
                anchorX_->setValue(value->x);
                anchorY_->setValue(value->y);
            }
        }

        if (scaleX_ != nullptr && scaleY_ != nullptr) {
            const auto* parameter =
                parameterForRole(node, composition, document::kScaleParameterRole);
            const auto value = parameter == nullptr || session_ == nullptr
                                   ? std::nullopt
                                   : session_->constantVec2Value(parameter->id);
            const QString tip = describe(parameter, tr("Scale is not exposed by this node"));
            for (auto* field : {scaleX_, scaleY_}) {
                field->setEnabled(value.has_value());
                field->setToolTip(tip);
            }
            // Stored as a unitless factor, shown as a percentage, exactly as in the properties
            // grid.
            const QSignalBlocker blockX(scaleX_);
            const QSignalBlocker blockY(scaleY_);
            scaleX_->setValue(value.has_value() ? value->x * 100.0 : 100.0);
            scaleY_->setValue(value.has_value() ? value->y * 100.0 : 100.0);
        }

        if (rotation_ != nullptr) {
            const auto* parameter =
                parameterForRole(node, composition, document::kRotationParameterRole);
            const auto value = parameter == nullptr || session_ == nullptr
                                   ? std::nullopt
                                   : session_->constantValue(parameter->id);
            rotation_->setEnabled(value.has_value());
            rotation_->setToolTip(describe(parameter, tr("Rotation is not exposed by this node")));
            const QSignalBlocker blocker(rotation_);
            rotation_->setValue(value.value_or(document::kDefaultRotationDegrees));
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

        if (textContent_ != nullptr) {
            const auto* parameter =
                parameterForRole(node, composition, document::kTextParameterRole);
            const auto value = parameter == nullptr || session_ == nullptr
                                   ? std::nullopt
                                   : session_->constantStringValue(parameter->id);
            textContent_->setEnabled(value.has_value());
            textContent_->setToolTip(describe(parameter, tr("Text is not exposed by this node")));
            if (value.has_value() && textContent_->text() != *value) {
                const QSignalBlocker blocker(textContent_);
                textContent_->setText(*value);
            }
        }

        if (textSize_ != nullptr) {
            const auto* parameter =
                parameterForRole(node, composition, document::kTextSizeParameterRole);
            const auto value = parameter == nullptr || session_ == nullptr
                                   ? std::nullopt
                                   : session_->constantValue(parameter->id);
            textSize_->setEnabled(value.has_value());
            textSize_->setToolTip(describe(parameter, tr("Size is not exposed by this node")));
            const QSignalBlocker blocker(textSize_);
            textSize_->setValue(value.value_or(document::kDefaultTextSizePixels));
        }

        if (colorChip_ != nullptr) {
            const auto* parameter =
                parameterForRole(node, composition, document::kSolidColorParameterRole);
            const auto value = parameter == nullptr || session_ == nullptr
                                   ? std::nullopt
                                   : session_->constantColorValue(parameter->id);
            colorChip_->setEnabled(value.has_value());
            if (value.has_value()) {
                const QSignalBlocker blocker(colorChip_);
                colorChip_->setColor(kit::KColor::fromRgba(
                    static_cast<float>(value->red), static_cast<float>(value->green),
                    static_cast<float>(value->blue), static_cast<float>(value->alpha)));
            }
            // The swatch quantizes to 8 bits and clamps, so an HDR or negative authoring channel
            // cannot be shown in it honestly; the exact, unclipped value travels in the tooltip,
            // together with what committing through the picker would do to such a value.
            const QString exact =
                value.has_value() ? exactColorText(*value) : describe(parameter, tr("No color"));
            colorChip_->setToolTip(
                value.has_value()
                    ? tr("%1\nEditing here commits a color inside the displayable [0, 1] range")
                          .arg(exact)
                    : exact);
        }

        std::size_t readOnlyIndex = 0;
        for (const auto& binding : node.parameters) {
            if (binding.role == document::kPositionParameterRole ||
                binding.role == document::kAnchorParameterRole ||
                binding.role == document::kScaleParameterRole ||
                binding.role == document::kRotationParameterRole ||
                binding.role == document::kOpacityParameterRole ||
                binding.role == document::kSolidColorParameterRole ||
                binding.role == document::kTextParameterRole ||
                binding.role == document::kTextSizeParameterRole) {
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
                std::max(controlColumn, static_cast<qreal>(row.widget->sizeHint().width()));
            rowHeight = std::max(rowHeight, static_cast<qreal>(row.widget->sizeHint().height()));
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
        const qreal width = layout_.width;
        const qreal socketHeight = static_cast<qreal>(sockets_.size()) * kSocketRowHeight;
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
        const qreal controlSpan = std::max(1.0, width_ - kCardPadding - controlLeft);
        qreal y = kCardHeaderHeight + socketHeight;
        for (const auto& row : valueRows_) {
            row.widget->resize(static_cast<int>(controlSpan), row.widget->sizeHint().height());
            positionProxy(row.widget, controlLeft, y + (rowHeight - row.widget->height()) / 2.0);
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
                         (socket->name == QString::fromUtf8(document::kPositionParameterRole) &&
                          (widget == positionX_ || widget == positionY_)) ||
                         (socket->name == QString::fromUtf8(document::kAnchorParameterRole) &&
                          (widget == anchorX_ || widget == anchorY_)) ||
                         (socket->name == QString::fromUtf8(document::kScaleParameterRole) &&
                          (widget == scaleX_ || widget == scaleY_)) ||
                         (socket->name == QString::fromUtf8(document::kRotationParameterRole) &&
                          widget == rotation_) ||
                         (socket->name == QString::fromUtf8(document::kOpacityParameterRole) &&
                          widget == opacity_) ||
                         (socket->name == QString::fromUtf8(document::kSolidColorParameterRole) &&
                          widget == colorChip_);
            }
            proxy->setVisible(!layout_.collapsed && !linked);
            proxy->setOpacity(layout_.muted ? 0.5 : 1.0);
            const qreal available = std::max(1.0, width_ - kCardPadding - proxy->pos().x());
            proxy->setScale(std::min(1.0, available / std::max(1, widget->width())));
        }
        const auto inputCount = std::ranges::count_if(
            sockets_, [](const auto* socket) { return socket->input.has_value(); });
        const auto outputCount = static_cast<std::ptrdiff_t>(sockets_.size()) - inputCount;
        int inputIndex = 0;
        int outputIndex = 0;
        for (std::size_t index = 0; index < sockets_.size(); ++index) {
            auto* socket = sockets_[index];
            const bool input = socket->input.has_value();
            const int edgeIndex = input ? inputIndex++ : outputIndex++;
            socket->setPos(input ? 0 : width_,
                           layout_.collapsed
                               ? kCardHeaderHeight * static_cast<qreal>(edgeIndex + 1) /
                                     static_cast<qreal>((input ? inputCount : outputCount) + 1)
                               : kCardHeaderHeight +
                                     (static_cast<qreal>(index) + 0.5) * kSocketRowHeight);
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
    kit::KValueField* anchorX_ = nullptr;
    kit::KValueField* anchorY_ = nullptr;
    kit::KValueField* scaleX_ = nullptr;
    kit::KValueField* scaleY_ = nullptr;
    kit::KValueField* rotation_ = nullptr;
    kit::KValueField* opacity_ = nullptr;
    kit::KColorChip* colorChip_ = nullptr;
    QLineEdit* textContent_ = nullptr;
    kit::KValueField* textSize_ = nullptr;
    // Only to choose the honest undo label and accessible name for the shared "color" role; the
    // control and its write path are identical for a solid and a text source.
    bool isTextSource_ = false;
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
