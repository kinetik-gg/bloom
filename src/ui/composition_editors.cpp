#include <bloom/ui/composition_editors.hpp>

#include <bloom/ui/composition_session.hpp>

#include <bloom/core/color.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/project.hpp>

#include <QAbstractItemView>
#include <QAction>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPaintEvent>
#include <QPainter>
#include <QSignalBlocker>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>

namespace bloom::ui {
namespace {

constexpr int kTimelineLayerIdRole = Qt::UserRole + 1;
constexpr int kTimelineSlotIdRole = Qt::UserRole + 2;
constexpr int kCompositionIdRole = Qt::UserRole + 1;
constexpr core::Color4d kDefaultSolidColor{0.18, 0.18, 0.18, 1.0};

const document::LayerOutputBoundary* layerBoundary(const document::Composition& composition,
                                                   const document::LayerId layerId) {
    const auto boundaries = composition.graph().layerOutputs();
    const auto found = std::ranges::find_if(
        boundaries, [layerId](const auto& candidate) { return candidate.layerId == layerId; });
    return found == boundaries.end() ? nullptr : &*found;
}

const document::ParameterRecord* nodeParameter(const document::Composition& composition,
                                               const document::NodeId nodeId,
                                               const std::string_view role) {
    const auto* node = composition.graph().findNode(nodeId);
    if (node == nullptr) {
        return nullptr;
    }
    const auto binding = std::ranges::find_if(
        node->parameters, [role](const auto& candidate) { return candidate.role == role; });
    return binding == node->parameters.end() ? nullptr
                                             : composition.parameters().find(binding->parameterId);
}

QString sourceDescription(const document::ParameterRecord& parameter) {
    if (std::holds_alternative<document::AnimationCurveSource>(parameter.source)) {
        return QStringLiteral("Animated");
    }
    if (std::holds_alternative<document::DriverBindingSource>(parameter.source)) {
        return QStringLiteral("Driven by graph");
    }
    return QStringLiteral("Constant");
}

QString layerName(const document::Composition& composition, const document::LayerId layerId) {
    const auto* boundary = layerBoundary(composition, layerId);
    if (boundary == nullptr || boundary->name.empty()) {
        return QStringLiteral("Layer %1").arg(layerId.value());
    }
    return QString::fromStdString(boundary->name);
}

const document::NodeRecord* directSourceNode(const CompositionSession& session,
                                             const document::LayerId layerId) {
    const auto* composition = session.composition();
    const auto sourceNodeId = session.directSourceNodeForLayer(layerId);
    return composition != nullptr && sourceNodeId.has_value()
               ? composition->graph().findNode(*sourceNodeId)
               : nullptr;
}

bool isKnownSource(const document::NodeRecord* node, const std::string_view typeId,
                   const std::uint32_t schemaVersion) {
    return node != nullptr && node->typeId == typeId && node->schemaVersion == schemaVersion;
}

const document::NodeRecord* selectedPresentationSource(const CompositionSession& session) {
    if (const auto* layerId = std::get_if<document::LayerId>(&session.selection().primary)) {
        return directSourceNode(session, *layerId);
    }
    return session.selectedNode();
}

QString layerKind(const CompositionSession& session, const document::LayerId layerId) {
    const auto* sourceNode = directSourceNode(session, layerId);
    if (isKnownSource(sourceNode, document::kSolidSourceNodeType,
                      document::kSolidSourceNodeSchemaVersion)) {
        return TimelineEditor::tr("Solid");
    }
    if (isKnownSource(sourceNode, document::kTextSourceNodeType,
                      document::kTextSourceNodeSchemaVersion)) {
        return TimelineEditor::tr("Text");
    }
    return TimelineEditor::tr("Layer");
}

qulonglong nextLayerNumber(const CompositionSession& session, const std::string_view typeId,
                           const std::uint32_t schemaVersion) {
    qulonglong count = 0;
    const auto* composition = session.composition();
    if (composition == nullptr) {
        return 1;
    }
    for (const auto& entry : composition->graph().layerStack().entries()) {
        const auto* sourceNode = directSourceNode(session, entry.layerId);
        if (isKnownSource(sourceNode, typeId, schemaVersion)) {
            ++count;
        }
    }
    return count + 1;
}

QString exactNumber(const double value) {
    std::array<char, 64> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                                      std::chars_format::general);
    if (result.ec != std::errc{}) {
        return QString::number(value, 'g', std::numeric_limits<double>::max_digits10);
    }
    return QString::fromLatin1(buffer.data(), static_cast<qsizetype>(result.ptr - buffer.data()));
}

QString exactColor(const core::Color4d color) {
    return QStringLiteral("R %1  G %2  B %3  A %4")
        .arg(exactNumber(color.red), exactNumber(color.green), exactNumber(color.blue),
             exactNumber(color.alpha));
}

QString selectionName(const CompositionSession& session) {
    const auto* composition = session.composition();
    if (composition == nullptr) {
        return QStringLiteral("No composition");
    }
    if (const auto* layerId = std::get_if<document::LayerId>(&session.selection().primary)) {
        return layerName(*composition, *layerId);
    }
    if (const auto* nodeId = std::get_if<document::NodeId>(&session.selection().primary)) {
        const auto* node = composition->graph().findNode(*nodeId);
        return node == nullptr ? QStringLiteral("Node unavailable")
                               : QString::fromStdString(node->typeId);
    }
    if (const auto* parameterId =
            std::get_if<document::ParameterId>(&session.selection().primary)) {
        const auto* parameter = composition->parameters().find(*parameterId);
        return parameter == nullptr ? QStringLiteral("Parameter unavailable")
                                    : QString::fromStdString(parameter->schemaKey);
    }
    return QStringLiteral("Nothing selected");
}

QToolButton* makeToolButton(QString text, QString accessibleName, QWidget* parent) {
    auto* button = new QToolButton(parent);
    button->setText(std::move(text));
    button->setAccessibleName(std::move(accessibleName));
    button->setAutoRaise(true);
    return button;
}

} // namespace

TimelineEditor::TimelineEditor(CompositionSession& session, QWidget* parent)
    : QWidget(parent), session_(session) {
    setObjectName("timelineEditor");
    setAccessibleName(tr("Layers timeline"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* controls = new QWidget(this);
    controls->setObjectName("timelineControls");
    auto* controlsLayout = new QHBoxLayout(controls);
    controlsLayout->setContentsMargins(8, 5, 8, 5);
    controlsLayout->setSpacing(4);

    auto* title = new QLabel(tr("Layers"), controls);
    title->setObjectName("editorSectionTitle");
    addButton_ = makeToolButton(tr("Add"), tr("Add layer"), controls);
    addButton_->setObjectName("addLayerButton");
    addButton_->setPopupMode(QToolButton::InstantPopup);
    addButton_->setToolTip(tr("Add a structured layer"));
    auto* addMenu = new QMenu(tr("Add Layer"), addButton_);
    addMenu->setObjectName("addLayerMenu");
    addMenu->setAccessibleName(tr("Add layer menu"));
    auto* addSolidAction = addMenu->addAction(tr("Solid"));
    addSolidAction->setObjectName("addSolidLayerAction");
    addSolidAction->setToolTip(tr("Add a middle-gray solid layer"));
    auto* addTextAction = addMenu->addAction(tr("Text"));
    addTextAction->setObjectName("addTextLayerAction");
    addTextAction->setToolTip(tr("Add a text layer"));
    addButton_->setMenu(addMenu);
    undoButton_ = makeToolButton(tr("Undo"), tr("Undo last edit"), controls);
    redoButton_ = makeToolButton(tr("Redo"), tr("Redo last edit"), controls);
    controlsLayout->addWidget(title);
    controlsLayout->addWidget(addButton_);
    controlsLayout->addStretch(1);
    controlsLayout->addWidget(undoButton_);
    controlsLayout->addWidget(redoButton_);

    layers_ = new QTreeWidget(this);
    layers_->setObjectName("layerStackView");
    layers_->setAccessibleName(tr("Composition layers"));
    layers_->setColumnCount(3);
    layers_->setHeaderLabels({tr("Name"), tr("Kind"), tr("Opacity")});
    layers_->setRootIsDecorated(false);
    layers_->setAlternatingRowColors(true);
    layers_->setSelectionMode(QAbstractItemView::SingleSelection);
    layers_->setUniformRowHeights(true);
    layers_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    layers_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    layers_->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);

    layout->addWidget(controls);
    layout->addWidget(layers_, 1);

    connect(addSolidAction, &QAction::triggered, this, [this] {
        const auto layerNumber = nextLayerNumber(session_, document::kSolidSourceNodeType,
                                                 document::kSolidSourceNodeSchemaVersion);
        (void)session_.addSolidLayer(tr("Solid %1").arg(layerNumber), kDefaultSolidColor);
    });
    connect(addTextAction, &QAction::triggered, this, [this] {
        const auto layerNumber = nextLayerNumber(session_, document::kTextSourceNodeType,
                                                 document::kTextSourceNodeSchemaVersion);
        (void)session_.addTextLayer(tr("Text %1").arg(layerNumber), tr("Text"));
    });
    connect(undoButton_, &QToolButton::clicked, &session_, &CompositionSession::undo);
    connect(redoButton_, &QToolButton::clicked, &session_, &CompositionSession::redo);
    connect(layers_, &QTreeWidget::itemSelectionChanged, this, [this] {
        if (rebuilding_) {
            return;
        }
        const auto selected = layers_->selectedItems();
        if (selected.empty()) {
            session_.clearSelection();
            return;
        }
        session_.selectLayer(document::LayerId::fromRaw(
            selected.front()->data(0, kTimelineLayerIdRole).toULongLong()));
    });
    connect(&session_, &CompositionSession::snapshotChanged, this, &TimelineEditor::rebuild);
    connect(&session_, &CompositionSession::compositionChanged, this, &TimelineEditor::rebuild);
    connect(&session_, &CompositionSession::selectionChanged, this,
            &TimelineEditor::updateSelection);
    connect(&session_, &CompositionSession::historyChanged, this,
            &TimelineEditor::updateHistoryActions);

    rebuild();
    updateHistoryActions();
}

void TimelineEditor::rebuild() {
    rebuilding_ = true;
    layers_->clear();
    const auto* composition = session_.composition();
    if (composition != nullptr) {
        for (const auto& entry : composition->graph().layerStack().entries()) {
            const auto* boundary = layerBoundary(*composition, entry.layerId);
            const QString name = layerName(*composition, entry.layerId);
            const QString kind = layerKind(session_, entry.layerId);
            QString opacity = QStringLiteral("—");
            if (boundary != nullptr) {
                if (const auto* parameter = nodeParameter(*composition, boundary->nodeId,
                                                          document::kOpacityParameterRole)) {
                    const auto* constant =
                        std::get_if<document::ConstantValueSource>(&parameter->source);
                    if (constant != nullptr) {
                        if (const auto* value = std::get_if<double>(&constant->value)) {
                            opacity = QStringLiteral("%1%").arg(*value * 100.0, 0, 'f', 0);
                        }
                    } else {
                        opacity = sourceDescription(*parameter);
                    }
                }
            }

            auto* row = new QTreeWidgetItem(layers_, {name, kind, opacity});
            row->setData(0, kTimelineLayerIdRole,
                         QVariant::fromValue<qulonglong>(entry.layerId.value()));
            row->setData(0, kTimelineSlotIdRole,
                         QVariant::fromValue<qulonglong>(entry.slotId.value()));
            row->setToolTip(
                0, tr("Layer %1 · Slot %2").arg(entry.layerId.value()).arg(entry.slotId.value()));
        }
    }
    updateSelection();
    rebuilding_ = false;
}

void TimelineEditor::updateSelection() {
    const QSignalBlocker blocker(layers_);
    layers_->clearSelection();
    const auto* layerId = std::get_if<document::LayerId>(&session_.selection().primary);
    if (layerId == nullptr && session_.selection().contextualLayer.has_value()) {
        layerId = &*session_.selection().contextualLayer;
    }
    if (layerId == nullptr) {
        return;
    }
    for (int index = 0; index < layers_->topLevelItemCount(); ++index) {
        auto* row = layers_->topLevelItem(index);
        if (row->data(0, kTimelineLayerIdRole).toULongLong() == layerId->value()) {
            row->setSelected(true);
            layers_->scrollToItem(row);
            break;
        }
    }
}

void TimelineEditor::updateHistoryActions() {
    undoButton_->setEnabled(session_.canUndo());
    redoButton_->setEnabled(session_.canRedo());
    const QString undoLabel = session_.undoLabel();
    const QString redoLabel = session_.redoLabel();
    undoButton_->setToolTip(undoLabel.isEmpty() ? tr("Nothing to undo")
                                                : tr("Undo %1").arg(undoLabel));
    redoButton_->setToolTip(redoLabel.isEmpty() ? tr("Nothing to redo")
                                                : tr("Redo %1").arg(redoLabel));
}

PropertiesEditor::PropertiesEditor(CompositionSession& session, QWidget* parent)
    : QWidget(parent), session_(session) {
    setObjectName("propertiesEditor");
    setAccessibleName(tr("Properties editor"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 10, 12, 12);
    layout->setSpacing(10);

    selectionLabel_ = new QLabel(this);
    selectionLabel_->setObjectName("propertiesSelectionTitle");
    selectionLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(selectionLabel_);

    auto* transformForm = new QFormLayout;
    transformForm->setContentsMargins(0, 0, 0, 0);
    transformForm->setHorizontalSpacing(10);
    transformForm->setVerticalSpacing(7);

    positionX_ = new QDoubleSpinBox(this);
    positionY_ = new QDoubleSpinBox(this);
    opacity_ = new QDoubleSpinBox(this);
    for (auto* editor : {positionX_, positionY_}) {
        editor->setRange(-1'000'000.0, 1'000'000.0);
        editor->setDecimals(2);
        editor->setSingleStep(1.0);
    }
    positionX_->setObjectName("positionXEditor");
    positionX_->setAccessibleName(tr("Position X"));
    positionY_->setObjectName("positionYEditor");
    positionY_->setAccessibleName(tr("Position Y"));
    opacity_->setObjectName("opacityEditor");
    opacity_->setAccessibleName(tr("Opacity"));
    opacity_->setRange(0.0, 100.0);
    opacity_->setDecimals(1);
    opacity_->setSingleStep(1.0);
    opacity_->setSuffix(QStringLiteral(" %"));

    auto* position = new QWidget(this);
    auto* positionLayout = new QHBoxLayout(position);
    positionLayout->setContentsMargins(0, 0, 0, 0);
    positionLayout->setSpacing(5);
    positionLayout->addWidget(new QLabel(QStringLiteral("X"), position));
    positionLayout->addWidget(positionX_);
    positionLayout->addWidget(new QLabel(QStringLiteral("Y"), position));
    positionLayout->addWidget(positionY_);

    transformForm->addRow(tr("Position"), position);
    transformForm->addRow(tr("Opacity"), opacity_);
    layout->addLayout(transformForm);

    solidColorPanel_ = new QWidget(this);
    solidColorPanel_->setObjectName("solidColorProperties");
    auto* solidColorLayout = new QVBoxLayout(solidColorPanel_);
    solidColorLayout->setContentsMargins(0, 4, 0, 0);
    solidColorLayout->setSpacing(7);
    auto* solidColorTitle = new QLabel(tr("Solid Source"), solidColorPanel_);
    solidColorTitle->setObjectName("editorSectionTitle");
    solidColorLayout->addWidget(solidColorTitle);

    auto* solidColorForm = new QFormLayout;
    solidColorForm->setContentsMargins(0, 0, 0, 0);
    solidColorForm->setHorizontalSpacing(10);
    solidColorForm->setVerticalSpacing(7);
    solidColorValue_ = new QLabel(solidColorPanel_);
    solidColorValue_->setObjectName("solidColorValue");
    solidColorValue_->setAccessibleName(tr("Solid RGBA value"));
    solidColorValue_->setTextInteractionFlags(Qt::TextSelectableByMouse |
                                              Qt::TextSelectableByKeyboard);
    solidColorValue_->setWordWrap(true);
    solidAlphaAssociation_ = new QLabel(solidColorPanel_);
    solidAlphaAssociation_->setObjectName("solidAlphaAssociation");
    solidAlphaAssociation_->setAccessibleName(tr("Solid alpha association"));
    solidAlphaAssociation_->setTextInteractionFlags(Qt::TextSelectableByMouse |
                                                    Qt::TextSelectableByKeyboard);
    solidColorEncoding_ = new QLabel(solidColorPanel_);
    solidColorEncoding_->setObjectName("solidColorEncoding");
    solidColorEncoding_->setAccessibleName(tr("Solid color encoding"));
    solidColorEncoding_->setTextInteractionFlags(Qt::TextSelectableByMouse |
                                                 Qt::TextSelectableByKeyboard);
    solidColorForm->addRow(tr("RGBA"), solidColorValue_);
    solidColorForm->addRow(tr("Alpha"), solidAlphaAssociation_);
    solidColorForm->addRow(tr("Encoding"), solidColorEncoding_);
    solidColorLayout->addLayout(solidColorForm);
    layout->addWidget(solidColorPanel_);
    layout->addStretch(1);

    const auto commitPosition = [this] {
        if (!rebuilding_) {
            (void)session_.setSelectedPosition(positionX_->value(), positionY_->value());
        }
    };
    connect(positionX_, &QDoubleSpinBox::editingFinished, this, commitPosition);
    connect(positionY_, &QDoubleSpinBox::editingFinished, this, commitPosition);
    connect(opacity_, &QDoubleSpinBox::editingFinished, this, [this] {
        if (!rebuilding_) {
            (void)session_.setSelectedOpacity(opacity_->value() / 100.0);
        }
    });
    connect(&session_, &CompositionSession::snapshotChanged, this, &PropertiesEditor::rebuild);
    connect(&session_, &CompositionSession::compositionChanged, this, &PropertiesEditor::rebuild);
    connect(&session_, &CompositionSession::selectionChanged, this, &PropertiesEditor::rebuild);

    rebuild();
}

void PropertiesEditor::rebuild() {
    rebuilding_ = true;
    selectionLabel_->setText(selectionName(session_));

    const auto* position = session_.parameterForSelection(document::kPositionParameterRole);
    const auto positionValue =
        position == nullptr ? std::nullopt : session_.constantVec2Value(position->id);
    const bool canEditPosition = positionValue.has_value();
    positionX_->setEnabled(canEditPosition);
    positionY_->setEnabled(canEditPosition);
    if (canEditPosition) {
        const QSignalBlocker blockX(positionX_);
        const QSignalBlocker blockY(positionY_);
        positionX_->setValue(positionValue->x);
        positionY_->setValue(positionValue->y);
    }
    const QString positionTip = position == nullptr
                                    ? tr("Position is not exposed by this selection")
                                    : sourceDescription(*position);
    positionX_->setToolTip(positionTip);
    positionY_->setToolTip(positionTip);

    configureOpacity();
    configureSolidColor();
    rebuilding_ = false;
}

void PropertiesEditor::configureOpacity() {
    const auto* parameter = session_.parameterForSelection(document::kOpacityParameterRole);
    const auto value = parameter == nullptr ? std::nullopt : session_.constantValue(parameter->id);
    opacity_->setEnabled(value.has_value());
    const QSignalBlocker blocker(opacity_);
    opacity_->setValue(value.has_value() ? *value * 100.0 : 100.0);
    opacity_->setToolTip(parameter == nullptr ? tr("Opacity is not exposed by this selection")
                                              : sourceDescription(*parameter));
}

void PropertiesEditor::configureSolidColor() {
    const auto* parameter = session_.parameterForSelection(document::kSolidColorParameterRole);
    const auto* sourceNode = selectedPresentationSource(session_);
    const bool isSolid = isKnownSource(sourceNode, document::kSolidSourceNodeType,
                                       document::kSolidSourceNodeSchemaVersion) &&
                         parameter != nullptr &&
                         parameter->schemaKey == document::kSolidColorParameterSchemaKey;
    solidColorPanel_->setVisible(isSolid);
    if (!isSolid) {
        return;
    }

    const auto value = session_.constantColorValue(parameter->id);
    solidColorValue_->setText(value.has_value() ? exactColor(*value)
                                                : sourceDescription(*parameter));
    solidColorValue_->setToolTip(
        tr("Straight scene-linear authoring values; negative and HDR RGB are not clipped"));
    solidAlphaAssociation_->setText(tr("Straight (unassociated)"));
    solidColorEncoding_->setText(
        QString::fromUtf8(document::kSolidColorEncoding.data(),
                          static_cast<qsizetype>(document::kSolidColorEncoding.size())));
}

ViewerEditor::ViewerEditor(CompositionSession& session, QWidget* parent)
    : QWidget(parent), session_(session) {
    setObjectName("viewerEditor");
    setAccessibleName(tr("Composition viewer"));
    setAccessibleDescription(
        tr("Render evaluation is not connected; no composition pixels are displayed."));
    setMinimumSize(220, 150);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    connect(&session_, &CompositionSession::snapshotChanged, this,
            qOverload<>(&ViewerEditor::update));
    connect(&session_, &CompositionSession::compositionChanged, this,
            qOverload<>(&ViewerEditor::update));
    connect(&session_, &CompositionSession::selectionChanged, this,
            qOverload<>(&ViewerEditor::update));
}

void ViewerEditor::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event)
    QPainter painter(this);
    painter.fillRect(rect(), QColor(15, 16, 18));

    const QRect frame = rect().adjusted(28, 28, -28, -28);
    painter.fillRect(frame, QColor(39, 41, 45));
    painter.setPen(QPen(QColor(77, 80, 87), 1.0));
    painter.drawRect(frame.adjusted(0, 0, -1, -1));

    constexpr int gridStep = 32;
    painter.setPen(QPen(QColor(46, 48, 53), 1.0));
    for (int x = frame.left() + gridStep; x < frame.right(); x += gridStep) {
        painter.drawLine(x, frame.top(), x, frame.bottom());
    }
    for (int y = frame.top() + gridStep; y < frame.bottom(); y += gridStep) {
        painter.drawLine(frame.left(), y, frame.right(), y);
    }

    const auto* composition = session_.composition();
    painter.setPen(QColor(201, 204, 210));
    painter.drawText(frame.adjusted(14, 10, -14, -10), Qt::AlignLeft | Qt::AlignTop,
                     composition == nullptr ? tr("No composition")
                                            : QString::fromStdString(composition->name()));

    painter.setPen(QColor(139, 144, 153));
    painter.drawText(frame, Qt::AlignCenter,
                     tr("Composition output\nRender evaluation is not connected yet"));

    if (session_.selection().contextualLayer.has_value() && composition != nullptr) {
        const QString name = layerName(*composition, *session_.selection().contextualLayer);
        const QRect selectionStatus(frame.left() + 12, frame.bottom() - 38, frame.width() - 24, 26);
        painter.setPen(QPen(QColor(44, 158, 232), 1.0));
        painter.setBrush(QColor(27, 46, 61));
        painter.drawRoundedRect(selectionStatus, 4.0, 4.0);
        painter.setPen(QColor(208, 231, 247));
        const QString status = tr("Selected: %1 · evaluated bounds unavailable").arg(name);
        painter.drawText(
            selectionStatus.adjusted(8, 0, -8, 0), Qt::AlignVCenter | Qt::AlignLeft,
            painter.fontMetrics().elidedText(status, Qt::ElideRight, selectionStatus.width() - 16));
    }
}

MediaEditor::MediaEditor(CompositionSession& session, QWidget* parent)
    : QWidget(parent), session_(session) {
    setObjectName("mediaEditor");
    setAccessibleName(tr("Project media editor"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(7);
    auto* title = new QLabel(tr("Project"), this);
    title->setObjectName("editorSectionTitle");
    compositions_ = new QListWidget(this);
    compositions_->setObjectName("compositionList");
    compositions_->setAccessibleName(tr("Project compositions"));
    compositions_->setAlternatingRowColors(true);
    layout->addWidget(title);
    layout->addWidget(compositions_, 1);

    connect(compositions_, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem* current) {
                if (!rebuilding_ && current != nullptr) {
                    (void)session_.setComposition(document::CompositionId::fromRaw(
                        current->data(kCompositionIdRole).toULongLong()));
                }
            });
    connect(&session_, &CompositionSession::snapshotChanged, this, &MediaEditor::rebuild);
    connect(&session_, &CompositionSession::compositionChanged, this,
            &MediaEditor::updateSelection);

    rebuild();
}

void MediaEditor::rebuild() {
    rebuilding_ = true;
    compositions_->clear();
    for (const auto& composition : session_.snapshot().project().compositions()) {
        auto* item = new QListWidgetItem(QString::fromStdString(composition.name()), compositions_);
        item->setData(kCompositionIdRole,
                      QVariant::fromValue<qulonglong>(composition.id().value()));
        item->setToolTip(tr("Composition %1").arg(composition.id().value()));
    }
    updateSelection();
    rebuilding_ = false;
}

void MediaEditor::updateSelection() {
    const QSignalBlocker blocker(compositions_);
    for (int index = 0; index < compositions_->count(); ++index) {
        auto* item = compositions_->item(index);
        if (item->data(kCompositionIdRole).toULongLong() == session_.compositionId().value()) {
            compositions_->setCurrentItem(item);
            return;
        }
    }
    compositions_->setCurrentItem(nullptr);
}

} // namespace bloom::ui
