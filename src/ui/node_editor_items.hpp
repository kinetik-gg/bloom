#pragma once
#include <bloom/ui/node_editor.hpp>

#include <bloom/ui/composition_authoring.hpp>
#include <bloom/ui/composition_editors.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/viewer_editor.hpp>

#include <bloom/ui/kit/color.hpp>
#include <bloom/ui/kit/color_chip.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/painting.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/kit/value_field.hpp>

#include <bloom/core/blend_mode.hpp>
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
#include <QSizeF>
#include <QStyleOptionGraphicsItem>
#include <QVariant>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
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
// One ordered slot's worth of the Merge node's multi-input pill (task S1, item 7): the pill grows
// by this much per layer it carries, so its length IS the stack's depth.
inline constexpr qreal kStackSlotPitch = kit::px(kit::Spacing::M);
// The pointer slop around a socket, beyond its own painted extent.
inline constexpr qreal kSocketHitSlop = 16.0 - kSocketDiameter / 2.0;
inline constexpr qreal kNodeSceneMargin = kit::px(kit::Spacing::XXL) * 2;
inline constexpr qreal kSelectionEdgeWidth = 2.0;
// A group frame's own title strip, and how faintly its body reads against the canvas: a frame is
// background, so its fill is the raised surface at low opacity rather than a second opaque plate.
inline constexpr qreal kGroupTitleHeight = kit::px(kit::Size::PanelHeader);
inline constexpr qreal kGroupFillOpacity = 0.35;
inline constexpr auto kGroupRadius = kit::Radius::Panel;
class NodeEdgeItem;
class NodeItem;
class NodeGroupItem;
QString displayTypeName(std::string_view typeId);
// The artist-facing name of a node TYPE (task S1, item 7). Four built-ins are named rather than
// spelled out of their type id -- Solid, Layer, Merge, Output -- and everything else falls back to
// displayTypeName(). Type ids themselves are unchanged; this is vocabulary, not identity.
QString nodeTypeDisplayName(std::string_view typeId);
// The small label above a card's own name, or empty. A layer boundary card is named after its
// layer, so the eyebrow is what still says the node is a Layer.
QString nodeEyebrow(const document::Composition& composition, const document::NodeRecord& node);
// The artist-facing heading a node category is listed under in an Add surface (task S1, item 4).
QString nodeCategoryName(document::NodeCategory category);
// The order the headings appear in: the pipeline's own order, from what makes an image to what
// consumes one. Add surfaces list their entries in this order, and the search popup emits a heading
// whenever the order moves on.
[[nodiscard]] std::span<const document::NodeCategory> nodeCategoryOrder();
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
NodeGroupItem* groupItemAncestor(QGraphicsItem* item);
QString socketKindName(document::SocketValueKind kind);
QPainterPath linkPath(QPointF start, QPointF end);

class SocketItem final : public QGraphicsItem {
  public:
    SocketItem(document::NodeId node, QString name, document::SocketValueKind kind,
               std::optional<document::InputPortRef> input,
               std::optional<document::OutputPortRef> output, bool structural,
               QGraphicsItem* parent);
    [[nodiscard]] QRectF boundingRect() const override;
    [[nodiscard]] QPainterPath shape() const override;
    void paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*) override;

    // The Merge node's ONE ordered multi-input (task S1, item 7). The layer stack's slot model is
    // untouched underneath: these are its slots, in stack order, and this single socket is the port
    // that stands for all of them -- instead of one repeated "content" row per layer.
    void setOrderedInputs(std::vector<document::InputPortRef> inputs);
    [[nodiscard]] const std::vector<document::InputPortRef>& orderedInputs() const noexcept {
        return orderedInputs_;
    }
    [[nodiscard]] bool multiInput() const noexcept { return !orderedInputs_.empty(); }
    // True when `ref` is this socket's own input, or -- for the ordered multi-input -- any of the
    // slots it stands for. This is how an edge finds the socket that terminates it.
    [[nodiscard]] bool accepts(const document::InputPortRef& ref) const;
    // The slot position the pointer is over during a drag, drawn as a caret across the pill. The
    // pill is dimmed as incompatible at the same time (stack slots are structural and accept no
    // drop), so the caret says "this is the position you are at", never "release here and it will
    // land".
    void setDropIndicator(std::optional<std::size_t> slotIndex);
    [[nodiscard]] std::optional<std::size_t> dropIndicator() const noexcept {
        return dropIndicator_;
    }
    // Which ordered slot a point in this socket's own coordinates falls on.
    [[nodiscard]] std::optional<std::size_t> slotIndexAt(QPointF localPoint) const;
    // The pill's painted extent along the card's edge; kSocketDiameter for an ordinary round
    // socket.
    [[nodiscard]] qreal pillLength() const;
    // How much vertical room this socket claims on an expanded card. One ordinary port is one
    // kSocketRowHeight row; the card sums these rather than multiplying by the socket count, so a
    // socket that is taller than a row can exist without the card's body landing on top of it.
    [[nodiscard]] qreal rowHeight() const;
    // Task S7: every kind is linkable now, not Image alone. The only non-draggable sockets left are
    // the structural Layer Output / stack-slot boundary, which a link gesture must not break --
    // removing the layer is how that connection goes.
    [[nodiscard]] bool draggable() const { return !structural_; }
    void setAuthoringEnabled(bool enabled);

    // How this socket reads while a link drag is in flight (task S1, item 6). A compatible socket
    // brightens toward Foreground and an incompatible one fades to the disabled ink, so a drag
    // names its own landing sites instead of leaving the artist to aim and find out.
    enum class DragAffinity : std::uint8_t { Idle, Compatible, Incompatible };
    void setDragAffinity(DragAffinity affinity);
    [[nodiscard]] DragAffinity dragAffinity() const noexcept { return affinity_; }
    // The ink this socket paints right now, affinity included. Exposed so a test can state the
    // brighten/dim rule in the same terms the painter applies it.
    [[nodiscard]] QColor paintedInk() const;

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
    DragAffinity affinity_ = DragAffinity::Idle;
    std::vector<document::InputPortRef> orderedInputs_;
    std::optional<std::size_t> dropIndicator_;
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
        eyebrow_ = nodeEyebrow(composition, node);
        setToolTip(QStringLiteral("%1\n%2\nNode %3")
                       .arg(title_, nodeTypeDisplayName(node.typeId))
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
    // Takes the inline rename field off the card and out of the scene immediately, then defers its
    // deletion. See the definition for why hiding it is not enough.
    void retireRenameProxy();

    // Any proxied control, not only a kit::KValueField: task S3's text content row is a QLineEdit,
    // because the kit has no string field and adding one is a kit change outside this task's fence.
    // Everything relayout() needs from a row is its sizeHint(), so QWidget is the honest type here.
    struct ValueRow final {
        QString label;
        QWidget* widget = nullptr;
        // Task S5, item 0: the row's keyframe diamond, for a row whose parameter is animatable.
        // Null for a row that is not (text content), so the layout below reserves the diamond
        // column only when some row actually carries one.
        KeyframeDiamond* diamond = nullptr;
        // The role this row's diamond keys, so refreshValues() can bind it to the node's own
        // parameter without re-deriving which role built which control.
        std::string_view role;
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

    void commitBlendMode(const int index) {
        if (refreshing_ || blendMode_ == nullptr || index < 0 || !selectSelf()) {
            return;
        }
        const auto mode =
            core::blendModeFromStoredValue(blendMode_->itemData(index).value<std::int64_t>());
        if (mode.has_value()) {
            (void)session_->setSelectedBlendMode(*mode);
        }
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
        hostTranslucent(*widget);
        auto* proxy = new QGraphicsProxyWidget(this);
        proxy->setWidget(widget);
    }

    // Declares which parameter role a control edits, so relayout() can apply the one
    // linked-hides-the- widget rule by role instead of by identity. A control with no role
    // registered -- a keyframe diamond, a rename editor -- is never hidden by a link, which is
    // correct: neither edits a value.
    void registerControlRole(const QWidget* widget, const std::string_view role) {
        if (widget != nullptr) {
            controlRoles_.emplace(
                widget, QString::fromUtf8(role.data(), static_cast<qsizetype>(role.size())));
        }
    }

    // The card's own keyframe diamond for `role` (task S5, item 0): the SAME shared
    // ui::KeyframeDiamond the Properties rows use, hosted on the canvas the way every other card
    // control is. Null when this card has no session to read, which is the same guard every commit*
    // path above already applies. objectName "nodeKeyframeDiamond" is new -- enumerated in this
    // task's report.
    [[nodiscard]] KeyframeDiamond* makeCardDiamond(const std::string_view role) {
        if (session_ == nullptr) {
            return nullptr;
        }
        auto* diamond = new KeyframeDiamond(*session_, std::string(role));
        diamond->setObjectName(QStringLiteral("nodeKeyframeDiamond"));
        diamond->resize(diamond->sizeHint());
        addProxy(diamond);
        return diamond;
    }

    // Builds the card's rows once per role set. Editable rows exist only for roles that have BOTH a
    // kit primitive able to carry the value AND an existing session/command path able to write it
    // (decision 5's honesty rule):
    //
    //   position -> two KValueFields (X, Y), committed through
    //               CompositionSession::setSelectedPosition()
    //   opacity  -> one KValueField, committed through CompositionSession::setSelectedOpacity()
    //   color    -> a KColorChip, committed through CompositionSession::setSelectedSolidColor() or
    //               setSelectedTextColor(). The swatch's own value model is displayable straight
    //               RGBA in [0, 1], so an HDR or negative authored channel still travels in the
    //               tooltip rather than in the swatch. Task S5 made the colour schemas animatable,
    //               so the chip's row carries a keyframe diamond like every other animatable row,
    //               and a commit on an animated colour writes a key at the session time.
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
        controlRoles_.clear();
        positionX_ = nullptr;
        positionY_ = nullptr;
        anchorX_ = nullptr;
        anchorY_ = nullptr;
        scaleX_ = nullptr;
        scaleY_ = nullptr;
        rotation_ = nullptr;
        opacity_ = nullptr;
        blendMode_ = nullptr;
        colorChip_ = nullptr;
        colorDiamond_ = nullptr;
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
                registerControlRole(positionX_, document::kPositionParameterRole);
                registerControlRole(positionY_, document::kPositionParameterRole);
                connect(positionX_, &kit::KValueField::valueChanged, this,
                        [this] { commitPosition(); });
                connect(positionY_, &kit::KValueField::valueChanged, this,
                        [this] { commitPosition(); });
                valueRows_.push_back({QStringLiteral("X"), positionX_,
                                      makeCardDiamond(document::kPositionParameterRole),
                                      document::kPositionParameterRole});
                valueRows_.push_back({QStringLiteral("Y"), positionY_, nullptr, {}});
            } else if (role == document::kAnchorParameterRole) {
                // Range/decimals/step/unit mirror PropertiesEditor's Anchor editors verbatim.
                anchorX_ = makeCardField(QStringLiteral("nodeAnchorXEditor"), tr("Anchor X"),
                                         -1'000'000.0, 1'000'000.0, 2, QStringLiteral("px"));
                anchorY_ = makeCardField(QStringLiteral("nodeAnchorYEditor"), tr("Anchor Y"),
                                         -1'000'000.0, 1'000'000.0, 2, QStringLiteral("px"));
                addProxy(anchorX_);
                addProxy(anchorY_);
                registerControlRole(anchorX_, document::kAnchorParameterRole);
                registerControlRole(anchorY_, document::kAnchorParameterRole);
                connect(anchorX_, &kit::KValueField::valueChanged, this,
                        [this] { commitAnchor(); });
                connect(anchorY_, &kit::KValueField::valueChanged, this,
                        [this] { commitAnchor(); });
                valueRows_.push_back({tr("Anchor X"), anchorX_,
                                      makeCardDiamond(document::kAnchorParameterRole),
                                      document::kAnchorParameterRole});
                valueRows_.push_back({tr("Anchor Y"), anchorY_, nullptr, {}});
            } else if (role == document::kScaleParameterRole) {
                scaleX_ = makeCardField(QStringLiteral("nodeScaleXEditor"), tr("Scale X"),
                                        -100'000.0, 100'000.0, 2, QStringLiteral("%"));
                scaleY_ = makeCardField(QStringLiteral("nodeScaleYEditor"), tr("Scale Y"),
                                        -100'000.0, 100'000.0, 2, QStringLiteral("%"));
                addProxy(scaleX_);
                addProxy(scaleY_);
                registerControlRole(scaleX_, document::kScaleParameterRole);
                registerControlRole(scaleY_, document::kScaleParameterRole);
                connect(scaleX_, &kit::KValueField::valueChanged, this, [this] { commitScale(); });
                connect(scaleY_, &kit::KValueField::valueChanged, this, [this] { commitScale(); });
                valueRows_.push_back({tr("Scale X"), scaleX_,
                                      makeCardDiamond(document::kScaleParameterRole),
                                      document::kScaleParameterRole});
                valueRows_.push_back({tr("Scale Y"), scaleY_, nullptr, {}});
            } else if (role == document::kRotationParameterRole) {
                rotation_ = makeCardField(QStringLiteral("nodeRotationEditor"), tr("Rotation"),
                                          -100'000.0, 100'000.0, 2, QString::fromUtf8("\u00b0"));
                addProxy(rotation_);
                registerControlRole(rotation_, document::kRotationParameterRole);
                connect(rotation_, &kit::KValueField::valueChanged, this,
                        [this] { commitRotation(); });
                valueRows_.push_back({tr("Rotation"), rotation_,
                                      makeCardDiamond(document::kRotationParameterRole),
                                      document::kRotationParameterRole});
            } else if (role == document::kOpacityParameterRole) {
                opacity_ = makeCardField(QStringLiteral("nodeOpacityEditor"), tr("Opacity"), 0.0,
                                         100.0, 1, QStringLiteral("%"));
                addProxy(opacity_);
                registerControlRole(opacity_, document::kOpacityParameterRole);
                connect(opacity_, &kit::KValueField::valueChanged, this,
                        [this] { commitOpacity(); });
                valueRows_.push_back({tr("Opacity"), opacity_,
                                      makeCardDiamond(document::kOpacityParameterRole),
                                      document::kOpacityParameterRole});
            } else if (role == document::kBlendModeParameterRole) {
                // A KDropdown rather than a field: the value is a closed vocabulary, offered in the
                // same order and with the same words the timeline row and the properties grid use,
                // and committed through the same session method, so the three surfaces cannot
                // drift.
                blendMode_ = new kit::KDropdown;
                blendMode_->setObjectName(QStringLiteral("nodeBlendModeDropdown"));
                blendMode_->setAccessibleName(tr("Blending"));
                blendMode_->setControlSize(kit::KDropdown::ControlSize::Compact);
                for (const auto mode : core::kBlendModes) {
                    blendMode_->addItem(blendModeDisplayName(mode),
                                        QVariant::fromValue(core::blendModeStoredValue(mode)));
                }
                blendMode_->resize(blendMode_->sizeHint());
                addProxy(blendMode_);
                registerControlRole(blendMode_, document::kBlendModeParameterRole);
                connect(blendMode_, &kit::KDropdown::currentIndexChanged, this,
                        [this](const int index) { commitBlendMode(index); });
                valueRows_.push_back({tr("Blending"), blendMode_, nullptr, {}});
            } else if (role == document::kSolidColorParameterRole) {
                colorChip_ = new kit::KColorChip;
                colorChip_->setObjectName(QStringLiteral("nodeColorChip"));
                colorChip_->setAccessibleName(isTextSource_ ? tr("Text color") : tr("Solid color"));
                colorChip_->setControlSize(kit::KColorChip::ControlSize::Compact);
                colorChip_->resize(colorChip_->sizeHint());
                addProxy(colorChip_);
                registerControlRole(colorChip_, document::kSolidColorParameterRole);
                connect(colorChip_, &kit::KColorChip::colorChanged, this,
                        [this](const kit::KColor& color) { commitColor(color); });
                colorRowLabel_ = tr("Color");
                // The colour row is the one row that is not a ValueRow (the chip is positioned on
                // its own, below), so its diamond is held directly rather than in valueRows_.
                colorDiamond_ = makeCardDiamond(document::kSolidColorParameterRole);
            } else if (role == document::kTextParameterRole) {
                textContent_ = new QLineEdit;
                textContent_->setObjectName(QStringLiteral("nodeTextContentEditor"));
                textContent_->setAccessibleName(tr("Text content"));
                textContent_->setFont(kit::font(kit::TypeRole::Ui));
                textContent_->resize(textContent_->sizeHint());
                addProxy(textContent_);
                registerControlRole(textContent_, document::kTextParameterRole);
                connect(textContent_, &QLineEdit::editingFinished, this,
                        [this] { commitTextContent(); });
                valueRows_.push_back({tr("Text"), textContent_, nullptr, {}});
            } else if (role == document::kTextSizeParameterRole) {
                // Range/decimals/step/unit mirror PropertiesEditor's Size editor verbatim, so the
                // same gesture in either surface produces the same value.
                textSize_ =
                    makeCardField(QStringLiteral("nodeTextSizeEditor"), tr("Text size"), 1.0,
                                  document::kMaximumTextSizePixels, 1, QStringLiteral("px"));
                addProxy(textSize_);
                registerControlRole(textSize_, document::kTextSizeParameterRole);
                connect(textSize_, &kit::KValueField::valueChanged, this,
                        [this] { commitTextSize(); });
                valueRows_.push_back({tr("Size"), textSize_,
                                      makeCardDiamond(document::kTextSizeParameterRole),
                                      document::kTextSizeParameterRole});
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
                                   : session_->effectiveVec2Value(parameter->id);
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
                                   : session_->effectiveVec2Value(parameter->id);
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
                                   : session_->effectiveVec2Value(parameter->id);
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
                                   : session_->effectiveScalarValue(parameter->id);
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
                                   : session_->effectiveScalarValue(parameter->id);
            opacity_->setEnabled(value.has_value());
            opacity_->setToolTip(describe(parameter, tr("Opacity is not exposed by this node")));
            const QSignalBlocker blocker(opacity_);
            opacity_->setValue(value.has_value() ? *value * 100.0 : 100.0);
        }

        if (blendMode_ != nullptr) {
            // The mode comes from the session's one reader, keyed by the LAYER this boundary owns,
            // not from the node's parameter record: blendModeForLayer() is the same lookup the
            // timeline row and the properties grid use, so all three show the same value.
            const auto* parameter =
                parameterForRole(node, composition, document::kBlendModeParameterRole);
            const auto layerId =
                session_ == nullptr ? std::nullopt : session_->layerForNode(node.id);
            const auto mode = layerId.has_value() && session_ != nullptr
                                  ? session_->blendModeForLayer(*layerId)
                                  : std::nullopt;
            blendMode_->setEnabled(mode.has_value());
            blendMode_->setToolTip(describe(parameter, tr("Blending is not exposed by this node")));
            const QSignalBlocker blocker(blendMode_);
            int row = 0;
            if (mode.has_value()) {
                const auto stored = core::blendModeStoredValue(*mode);
                for (int index = 0; index < blendMode_->count(); ++index) {
                    if (blendMode_->itemData(index).value<std::int64_t>() == stored) {
                        row = index;
                        break;
                    }
                }
            }
            blendMode_->setCurrentIndex(row);
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
                                   : session_->effectiveScalarValue(parameter->id);
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
                                   : session_->effectiveColorValue(parameter->id);
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

        // Every diamond is bound to THIS node's own parameter (never the selection's) and then
        // re-read, in one pass, so a card that is not selected still paints the truth for its own
        // rows -- and clicking one keys that parameter without moving the selection.
        for (const auto& row : valueRows_) {
            if (row.diamond == nullptr) {
                continue;
            }
            const auto* parameter = parameterForRole(node, composition, row.role);
            row.diamond->setParameterId(parameter == nullptr ? document::ParameterId{}
                                                             : parameter->id);
            row.diamond->refresh();
        }
        if (colorDiamond_ != nullptr) {
            const auto* parameter =
                parameterForRole(node, composition, document::kSolidColorParameterRole);
            colorDiamond_->setParameterId(parameter == nullptr ? document::ParameterId{}
                                                               : parameter->id);
            colorDiamond_->refresh();
        }

        std::size_t readOnlyIndex = 0;
        for (const auto& binding : node.parameters) {
            if (binding.role == document::kPositionParameterRole ||
                binding.role == document::kAnchorParameterRole ||
                binding.role == document::kScaleParameterRole ||
                binding.role == document::kRotationParameterRole ||
                binding.role == document::kOpacityParameterRole ||
                binding.role == document::kBlendModeParameterRole ||
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
        // The diamond column is reserved only when this card actually carries one, so a card with
        // no animatable parameter is exactly as wide as it was before task S5.
        qreal diamondColumn = 0.0;
        for (const auto& row : valueRows_) {
            if (row.diamond != nullptr) {
                diamondColumn =
                    std::max(diamondColumn, static_cast<qreal>(row.diamond->sizeHint().width()));
            }
        }
        if (colorDiamond_ != nullptr) {
            diamondColumn =
                std::max(diamondColumn, static_cast<qreal>(colorDiamond_->sizeHint().width()));
        }
        const qreal diamondSpan = diamondColumn > 0.0 ? diamondColumn + kCardLabelGap : 0.0;
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
                                                        diamondSpan + controlColumn + kCardPadding);
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

        const qreal diamondLeft = kCardPadding + labelColumn + kCardLabelGap;
        const qreal controlLeft = diamondLeft + diamondSpan;
        // CEIL, never truncate: a fractional span rounded down leaves the control a pixel short of
        // the card's own padding, and at a fractional row pitch that gap is exactly the sliver of
        // card surface that made a full-width field look inset.
        const qreal controlSpan = std::max(1.0, std::ceil(width_ - kCardPadding - controlLeft));
        qreal y = kCardHeaderHeight + socketHeight;
        for (const auto& row : valueRows_) {
            row.widget->resize(static_cast<int>(controlSpan), row.widget->sizeHint().height());
            positionProxy(row.widget, controlLeft, y + (rowHeight - row.widget->height()) / 2.0);
            if (row.diamond != nullptr) {
                row.diamond->resize(row.diamond->sizeHint());
                positionProxy(row.diamond, diamondLeft,
                              y + (rowHeight - row.diamond->height()) / 2.0);
            }
            y += rowHeight + kCardRowGap;
        }
        if (colorChip_ != nullptr) {
            colorChip_->resize(colorChip_->sizeHint());
            // The color row is the last row carrying a real widget -- read-only rows below it are
            // painted, not positioned -- so `y` is deliberately not advanced again here.
            positionProxy(colorChip_, controlLeft, y + (rowHeight - colorChip_->height()) / 2.0);
            if (colorDiamond_ != nullptr) {
                colorDiamond_->resize(colorDiamond_->sizeHint());
                positionProxy(colorDiamond_, diamondLeft,
                              y + (rowHeight - colorDiamond_->height()) / 2.0);
            }
        }
        for (auto* child : childItems()) {
            auto* proxy = qgraphicsitem_cast<QGraphicsProxyWidget*>(child);
            if (proxy == nullptr || proxy == renameProxy_)
                continue;
            const auto* widget = proxy->widget();
            // Task S7, item 3: one rule, by role. Every control the card builds registers the
            // parameter role it edits, and a role whose socket is linked hides its control -- which
            // is what makes "unlinked shows the widget, linked shows only the socket" one sentence
            // rather than a per-control chain that had to be extended for every new row.
            const auto role = controlRoles_.find(widget);
            const bool linked = role != controlRoles_.end() && linkedInputs_.contains(role->second);
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
    QString eyebrow_;
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
    // Which parameter role each control edits. Keyed by widget because that is what the proxy sweep
    // in relayout() has in hand, and rebuilt with the fields themselves in ensureFields().
    std::map<const QWidget*, QString> controlRoles_;
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
    kit::KDropdown* blendMode_ = nullptr;
    kit::KColorChip* colorChip_ = nullptr;
    KeyframeDiamond* colorDiamond_ = nullptr;
    QLineEdit* textContent_ = nullptr;
    kit::KValueField* textSize_ = nullptr;
    // Only to choose the honest undo label and accessible name for the shared "color" role; the
    // control and its write path are identical for a solid and a text source.
    bool isTextSource_ = false;
    QString colorRowLabel_;
    QGraphicsDropShadowEffect* dragShadow_ = nullptr;
    std::vector<NodeEdgeItem*> edges_;
};

// One node group, painted BEHIND its members: the bounding rectangle of the member cards plus the
// record's own padding, a hairline Border, a faint SurfaceRaised fill and Radius::Panel, with an
// inline-editable title strip along its top.
//
// The frame owns no geometry of its own. Its rectangle is recomputed from the live member cards --
// including mid-drag, which is what makes it follow its members instead of lagging a snapshot
// behind. While a member is being dragged OUT of it, the dragged cards are excluded from that
// computation, so the frame holds still and the artist can see whether the card is landing inside
// it; a gesture that drags every member at once excludes nothing, and the frame travels with them.
class NodeGroupItem final : public QGraphicsObject {
  public:
    NodeGroupItem(document::NodeGroupId id, CompositionSession* session);

    [[nodiscard]] document::NodeGroupId id() const noexcept { return id_; }
    [[nodiscard]] const std::set<document::NodeId>& members() const noexcept { return members_; }
    [[nodiscard]] const QString& title() const noexcept { return title_; }

    // Reconciles the frame against its record. Geometry follows separately, through setFrameRect().
    void refresh(const document::NodeGroupRecord& record);
    // `rect` is in scene coordinates; the item moves to its top-left and keeps a local origin.
    void setFrameRect(QRectF rect);
    [[nodiscard]] QRectF frameRect() const { return {pos(), size_}; }
    [[nodiscard]] QRectF titleRect() const { return {0.0, 0.0, size_.width(), kGroupTitleHeight}; }
    [[nodiscard]] document::Vec2d padding() const noexcept { return padding_; }

    void startRename();
    void setAuthoringEnabled(bool enabled);

    [[nodiscard]] QRectF boundingRect() const override { return {QPointF{}, size_}; }
    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget*) override;

  private:
    void retireRenameProxy();

    document::NodeGroupId id_;
    CompositionSession* session_ = nullptr;
    QString title_;
    std::set<document::NodeId> members_;
    document::Vec2d padding_{};
    QSizeF size_;
    QGraphicsProxyWidget* renameProxy_ = nullptr;
    bool authoringEnabled_ = false;
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
