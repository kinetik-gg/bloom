#include <bloom/ui/node_editor.hpp>

#include <bloom/ui/composition_session.hpp>

#include <bloom/core/color.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/project.hpp>

#include <QBrush>
#include <QColor>
#include <QGraphicsObject>
#include <QGraphicsPathItem>
#include <QHBoxLayout>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QStyleOptionGraphicsItem>
#include <QVariant>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace bloom::ui {
namespace {

constexpr qreal kNodeWidth = 190.0;
constexpr qreal kNodeHeaderHeight = 30.0;
constexpr qreal kNodeRowHeight = 22.0;

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

QString parameterText(const document::ParameterRecord& parameter) {
    const auto* constant = std::get_if<document::ConstantValueSource>(&parameter.source);
    if (constant == nullptr) {
        return std::holds_alternative<document::AnimationCurveSource>(parameter.source)
                   ? QStringLiteral("Animated")
                   : QStringLiteral("Driven");
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
                return QStringLiteral("RGBA %1, %2, %3, %4")
                    .arg(QString::number(value.red, 'g', std::numeric_limits<double>::max_digits10))
                    .arg(QString::number(value.green, 'g',
                                         std::numeric_limits<double>::max_digits10))
                    .arg(
                        QString::number(value.blue, 'g', std::numeric_limits<double>::max_digits10))
                    .arg(QString::number(value.alpha, 'g',
                                         std::numeric_limits<double>::max_digits10));
            } else if constexpr (std::is_same_v<Value, std::string>) {
                return QString::fromStdString(value).left(24);
            } else {
                return QStringLiteral("%1/%2").arg(value.numerator()).arg(value.denominator());
            }
        },
        constant->value);
}

class NodeEdgeItem;

class NodeItem final : public QGraphicsObject {
  public:
    NodeItem(const document::NodeRecord& node, const document::ParameterStore& parameters)
        : id_(node.id), title_(displayTypeName(node.typeId)) {
        setData(kNodeItemKindRole, QStringLiteral("node"));
        setData(kNodeStableIdRole, QVariant::fromValue<qulonglong>(node.id.value()));
        setFlags(ItemIsMovable | ItemIsSelectable | ItemSendsGeometryChanges);
        setCursor(Qt::OpenHandCursor);
        setToolTip(QStringLiteral("%1\nNode %2").arg(title_).arg(node.id.value()));

        parameterRows_.reserve(node.parameters.size());
        for (const auto& binding : node.parameters) {
            const auto* parameter = parameters.find(binding.parameterId);
            if (parameter != nullptr) {
                parameterRows_.push_back(
                    {QString::fromStdString(binding.role), parameterText(*parameter)});
            }
        }
    }

    [[nodiscard]] QRectF boundingRect() const override {
        const auto rowCount = std::max<std::size_t>(parameterRows_.size(), 1);
        return {0.0, 0.0, kNodeWidth,
                kNodeHeaderHeight + kNodeRowHeight * static_cast<qreal>(rowCount) + 10.0};
    }

    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget*) override {
        const QRectF bounds = boundingRect();
        const QColor border = option->state.testFlag(QStyle::State_Selected) ? QColor(44, 158, 232)
                                                                             : QColor(71, 74, 80);
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(QPen(border, option->state.testFlag(QStyle::State_Selected) ? 2.0 : 1.0));
        painter->setBrush(QColor(35, 37, 41));
        painter->drawRoundedRect(bounds, 7.0, 7.0);

        QPainterPath header;
        header.addRoundedRect(QRectF(0.0, 0.0, bounds.width(), kNodeHeaderHeight), 7.0, 7.0);
        header.addRect(QRectF(0.0, kNodeHeaderHeight - 7.0, bounds.width(), 7.0));
        painter->fillPath(header, QColor(48, 51, 57));

        painter->setPen(QColor(235, 237, 240));
        painter->drawText(QRectF(12.0, 0.0, bounds.width() - 24.0, kNodeHeaderHeight),
                          Qt::AlignVCenter | Qt::AlignLeft, title_);

        if (parameterRows_.empty()) {
            painter->setPen(QColor(139, 144, 153));
            painter->drawText(
                QRectF(12.0, kNodeHeaderHeight, bounds.width() - 24.0, kNodeRowHeight + 10.0),
                Qt::AlignCenter, QStringLiteral("No exposed parameters"));
            return;
        }

        qreal y = kNodeHeaderHeight + 5.0;
        for (const auto& [role, value] : parameterRows_) {
            painter->setPen(QColor(174, 178, 186));
            painter->drawText(QRectF(12.0, y, bounds.width() * 0.54, kNodeRowHeight),
                              Qt::AlignVCenter | Qt::AlignLeft, role);
            painter->setPen(QColor(222, 224, 229));
            painter->drawText(
                QRectF(bounds.width() * 0.52, y, bounds.width() * 0.42 - 12.0, kNodeRowHeight),
                Qt::AlignVCenter | Qt::AlignRight, value);
            y += kNodeRowHeight;
        }
    }

    [[nodiscard]] document::NodeId id() const noexcept { return id_; }
    void addEdge(NodeEdgeItem& edge) { edges_.push_back(&edge); }

  protected:
    QVariant itemChange(GraphicsItemChange change, const QVariant& value) override;

  private:
    document::NodeId id_;
    QString title_;
    std::vector<std::pair<QString, QString>> parameterRows_;
    std::vector<NodeEdgeItem*> edges_;
};

class NodeEdgeItem final : public QGraphicsPathItem {
  public:
    NodeEdgeItem(NodeItem& source, NodeItem& destination)
        : source_(source), destination_(destination) {
        setZValue(-1.0);
        setPen(QPen(QColor(102, 170, 224), 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        source_.addEdge(*this);
        destination_.addEdge(*this);
        updatePath();
    }

    void updatePath() {
        const QPointF sourcePoint = source_.mapToScene(source_.boundingRect().center());
        const QPointF destinationPoint =
            destination_.mapToScene(destination_.boundingRect().center());
        const QPointF start(source_.mapToScene(source_.boundingRect().topRight()).x(),
                            sourcePoint.y());
        const QPointF end(destination_.mapToScene(destination_.boundingRect().topLeft()).x(),
                          destinationPoint.y());
        const qreal handle = std::max<qreal>(60.0, std::abs(end.x() - start.x()) * 0.48);

        QPainterPath curve(start);
        curve.cubicTo(start + QPointF(handle, 0.0), end - QPointF(handle, 0.0), end);
        setPath(curve);
    }

  private:
    NodeItem& source_;
    NodeItem& destination_;
};

QVariant NodeItem::itemChange(const GraphicsItemChange change, const QVariant& value) {
    if (change == ItemPositionHasChanged) {
        for (auto* edge : edges_) {
            edge->updatePath();
        }
    }
    return QGraphicsObject::itemChange(change, value);
}

NodeItem* nodeItem(const QList<QGraphicsItem*>& items) {
    for (auto* item : items) {
        if (auto* node = dynamic_cast<NodeItem*>(item)) {
            return node;
        }
    }
    return nullptr;
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

} // namespace

NodeGraphicsScene::NodeGraphicsScene(QObject* parent) : QGraphicsScene(parent) {
    setObjectName("nodeGraphicsScene");
    setBackgroundBrush(QColor(25, 26, 29));
    setItemIndexMethod(QGraphicsScene::BspTreeIndex);
}

void NodeGraphicsScene::setProjection(const document::Snapshot& snapshot,
                                      const document::CompositionId compositionId) {
    std::map<std::uint64_t, QPointF> positions;
    for (auto* item : items()) {
        if (item->data(kNodeItemKindRole).toString() == QStringLiteral("node")) {
            positions.emplace(item->data(kNodeStableIdRole).toULongLong(), item->pos());
        }
    }
    clear();

    const auto* composition = snapshot.project().findComposition(compositionId);
    if (composition == nullptr) {
        setSceneRect({});
        return;
    }

    std::array<int, 4> rows{};
    for (const auto& node : composition->graph().nodes()) {
        auto* item = new NodeItem(node, composition->parameters());
        addItem(item);
        const auto saved = positions.find(node.id.value());
        if (saved != positions.end()) {
            item->setPos(saved->second);
        } else {
            const std::size_t column = layoutColumn(node);
            item->setPos(40.0 + static_cast<qreal>(column) * 270.0,
                         40.0 + static_cast<qreal>(rows.at(column)++) * 150.0);
        }
    }

    rebuildEdges(*composition);
    const QRectF bounds = itemsBoundingRect();
    setSceneRect(bounds.isEmpty() ? QRectF(-100.0, -100.0, 200.0, 200.0)
                                  : bounds.adjusted(-120.0, -120.0, 120.0, 120.0));
}

QGraphicsItem* NodeGraphicsScene::findNodeItem(const document::NodeId nodeId) const {
    const auto matching = items();
    const auto found = std::ranges::find_if(matching, [nodeId](const auto* item) {
        return item->data(kNodeItemKindRole).toString() == QStringLiteral("node") &&
               item->data(kNodeStableIdRole).toULongLong() == nodeId.value();
    });
    return found == matching.end() ? nullptr : *found;
}

void NodeGraphicsScene::rebuildEdges(const document::Composition& composition) {
    for (const auto& edge : composition.graph().edges()) {
        auto* source = dynamic_cast<NodeItem*>(findNodeItem(edge.source.nodeId));
        auto* destination =
            dynamic_cast<NodeItem*>(findNodeItem(destinationNodeId(edge.destination)));
        if (source != nullptr && destination != nullptr) {
            addItem(new NodeEdgeItem(*source, *destination));
        }
    }
}

NodeGraphicsView::NodeGraphicsView(QWidget* parent) : QGraphicsView(parent) {
    setObjectName("nodeGraphicsView");
    setAccessibleName(tr("Composition node graph"));
    setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
    setDragMode(QGraphicsView::RubberBandDrag);
    setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    setResizeAnchor(QGraphicsView::AnchorViewCenter);
    setViewportUpdateMode(QGraphicsView::MinimalViewportUpdate);
    setFrameShape(QFrame::NoFrame);
}

void NodeGraphicsView::wheelEvent(QWheelEvent* event) {
    const qreal currentScale = transform().m11();
    const qreal requested = std::pow(1.0015, event->angleDelta().y());
    const qreal nextScale = std::clamp(currentScale * requested, 0.2, 3.0);
    const qreal applied = nextScale / currentScale;
    scale(applied, applied);
    event->accept();
}

NodeGraphEditor::NodeGraphEditor(CompositionSession& session, QWidget* parent)
    : QWidget(parent), session_(session) {
    setObjectName("nodeGraphEditor");
    setAccessibleName(tr("Nodes editor"));

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    scene_ = new NodeGraphicsScene(this);
    view_ = new NodeGraphicsView(this);
    view_->setScene(scene_);
    layout->addWidget(view_);

    connect(&session_, &CompositionSession::snapshotChanged, this, &NodeGraphEditor::rebuild);
    connect(&session_, &CompositionSession::compositionChanged, this, &NodeGraphEditor::rebuild);
    connect(&session_, &CompositionSession::selectionChanged, this,
            &NodeGraphEditor::updateSelection);
    connect(scene_, &QGraphicsScene::selectionChanged, this,
            &NodeGraphEditor::sceneSelectionChanged);

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
}

void NodeGraphEditor::updateSelection() {
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
    rebuilding_ = false;
}

void NodeGraphEditor::sceneSelectionChanged() {
    if (rebuilding_) {
        return;
    }
    auto* item = nodeItem(scene_->selectedItems());
    if (item == nullptr) {
        session_.clearSelection();
        return;
    }
    session_.selectNode(item->id());
}

} // namespace bloom::ui
