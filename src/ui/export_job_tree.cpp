#include "export_job_tree.hpp"

#include "asset_drop.hpp"
#include "export_panel_text.hpp"

#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/icons.hpp>
#include <bloom/ui/kit/painting.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <QDrag>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QHeaderView>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QTreeWidgetItemIterator>

#include <algorithm>

namespace bloom::ui::exportpanel {
namespace {

[[nodiscard]] bool selectedRow(const QStyleOptionViewItem& option) {
    return (option.state & QStyle::State_Selected) != 0;
}

[[nodiscard]] bool outputRow(const QModelIndex& index) {
    return index.siblingAtColumn(NameColumn).data(kOutputRole).toULongLong() != 0;
}

[[nodiscard]] kit::Color columnInk(const bool selected) {
    return selected ? kit::Color::Foreground : kit::Color::Muted;
}

} // namespace

QRect ExportRowDelegate::checkRect(const QRect& cell) {
    const int box = kit::px(kit::Size::PropertiesCheckBox);
    return {cell.left() + (cell.width() - box) / 2, cell.top() + (cell.height() - box) / 2, box,
            box};
}

void ExportRowDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                              const QModelIndex& index) const {
    // The row's background has been painted by ExportJobTree::drawRow across the viewport.
    painter->save();
    const bool selected = selectedRow(option);
    const QRect cell = option.rect;
    const bool child = outputRow(index);
    const int right = cell.left() + cell.width() - kit::px(kit::Spacing::S);
    const int left = cell.left() + kit::px(kit::Spacing::S);
    const auto metrics = option.fontMetrics;
    painter->setFont(option.font);
    const qreal ratio = painter->device()->devicePixelRatio();
    const auto text = index.data(Qt::DisplayRole).toString();
    const bool enabled =
        index.siblingAtColumn(EnabledColumn).data(Qt::CheckStateRole).toInt() == Qt::Checked;
    switch (index.column()) {
    case LabelColumn: {
        const auto label = index.data(kLabelRole);
        if (label.isValid()) {
            const int box = kit::px(kit::Size::IconSmall);
            const QRectF swatch(cell.left() + (cell.width() - box) / 2.0,
                                cell.top() + (cell.height() - box) / 2.0, box, box);
            kit::fillRoundedSurface(*painter, swatch, label.value<QColor>(),
                                    kit::color(kit::Color::Border), kit::Radius::Small);
        }
        break;
    }
    case EnabledColumn: {
        if (index.data(Qt::CheckStateRole).isValid()) {
            const bool checked = index.data(Qt::CheckStateRole).toInt() == Qt::Checked;
            const QRect box = checkRect(cell);
            const bool editable = index.data(kEditableRole).toBool();
            kit::fillRoundedSurface(
                *painter, box, kit::color(checked ? kit::Color::Accent : kit::Color::Field),
                kit::color(checked ? kit::Color::Accent : kit::Color::Border), kit::Radius::Small);
            if (checked) {
                const auto glyph =
                    kit::iconPixmap(kit::IconId::Check, kit::Size::IconSmall, kit::Color::OnAccent,
                                    editable ? kit::State::Normal : kit::State::Disabled,
                                    kit::IconWeight::Bold, ratio);
                const QSize logical = glyph.deviceIndependentSize().toSize();
                painter->drawPixmap(box.left() + (box.width() - logical.width()) / 2,
                                    box.top() + (box.height() - logical.height()) / 2, glyph);
            }
        }
        break;
    }
    case NameColumn: {
        int textLeft = left;
        if (index.data(kGlyphRole).isValid()) {
            const int box = kit::px(kit::Size::IconMedium);
            const auto pixmap = kit::iconPixmap(
                static_cast<kit::IconId>(index.data(kGlyphRole).toInt()), kit::IconRole::Chrome,
                selected ? kit::Color::OnAccent : child ? kit::Color::Muted : kit::Color::Foreground,
                kit::State::Normal, ratio);
            painter->drawPixmap(cell.left() + kit::px(kit::Spacing::S),
                                cell.top() + (cell.height() - box) / 2, pixmap);
            textLeft = cell.left() + kit::px(kit::Spacing::S) + kit::px(kit::Size::AssetIndent);
        }
        painter->setPen(kit::color(selected ? kit::Color::OnAccent
                                            : (!enabled || child) ? kit::Color::Muted
                                                                  : kit::Color::Foreground));
        painter->drawText(QRect(textLeft, cell.top(), std::max(0, right - textLeft), cell.height()),
                          Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine,
                          metrics.elidedText(text, Qt::ElideRight, std::max(0, right - textLeft)));
        break;
    }
    case CompositionColumn:
        if (child) {
            // The destination reads across the Composition and Range cells, its file name kept.
            int wide = cell.width();
            if (const auto* view = qobject_cast<const QTreeWidget*>(option.widget);
                view != nullptr && !view->header()->isSectionHidden(RangeColumn))
                wide += view->header()->sectionSize(RangeColumn);
            const int span = cell.left() + wide - kit::px(kit::Spacing::S) - left;
            painter->setPen(kit::color(columnInk(selected)));
            painter->drawText(QRect(left, cell.top(), std::max(0, span), cell.height()),
                              Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine,
                              metrics.elidedText(index.data(kDestinationRole).toString(),
                                                 Qt::ElideMiddle, std::max(0, span)));
            break;
        }
        [[fallthrough]];
    case RangeColumn:
        if (child)
            break;
        painter->setPen(kit::color(columnInk(selected)));
        painter->drawText(QRect(left, cell.top(), std::max(0, right - left), cell.height()),
                          Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine,
                          metrics.elidedText(text, Qt::ElideRight, std::max(0, right - left)));
        break;
    case StatusColumn: {
        const auto tone = static_cast<StatusTone>(index.data(kToneRole).toInt());
        painter->setPen(kit::color(tone == StatusTone::Neutral ? columnInk(selected)
                                                               : toneColor(tone)));
        painter->drawText(QRect(left, cell.top(), std::max(0, right - left), cell.height()),
                          Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine,
                          metrics.elidedText(text, Qt::ElideRight, std::max(0, right - left)));
        break;
    }
    default:
        break;
    }
    painter->restore();
}

QSize ExportRowDelegate::sizeHint(const QStyleOptionViewItem& option,
                                  const QModelIndex& index) const {
    const int width = option.fontMetrics.horizontalAdvance(index.data(Qt::DisplayRole).toString()) +
                      kit::px(kit::Spacing::S) + kit::px(kit::Spacing::S) +
                      (index.column() == NameColumn ? kit::px(kit::Size::AssetIndent) : 0);
    return {width, kit::px(kit::Size::AssetRow)};
}

ExportJobTree::ExportJobTree(CompositionSession& session, QWidget* const parent)
    : QTreeWidget(parent), session_(session) {
    setObjectName(QStringLiteral("exportJobTree"));
    setColumnCount(ColumnCount);
    setHeaderLabels({{}, {}, tr("Name"), tr("Composition"), tr("Range"), tr("Status")});
    setItemDelegate(new ExportRowDelegate(this));
    setRootIsDecorated(true);
    setIndentation(kit::px(kit::Size::AssetIndent));
    setUniformRowHeights(true);
    setAlternatingRowColors(false);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setSelectionBehavior(QAbstractItemView::SelectRows);
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setMouseTracking(true);
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setDragEnabled(true);
    setAcceptDrops(true);
    setDragDropMode(QAbstractItemView::DragDrop);
    setContextMenuPolicy(Qt::CustomContextMenu);
    setAccessibleName(tr("Export queue"));
    setAccessibleDescription(
        tr("Export jobs with their outputs, in the order they render. Drag to reorder."));
    auto* header = this->header();
    header->setStretchLastSection(false);
    header->setSectionsMovable(false);
    header->setHighlightSections(false);
    header->setSectionResizeMode(LabelColumn, QHeaderView::Fixed);
    header->setSectionResizeMode(EnabledColumn, QHeaderView::Fixed);
    header->setSectionResizeMode(NameColumn, QHeaderView::Stretch);
    for (const int column : {CompositionColumn, RangeColumn, StatusColumn})
        header->setSectionResizeMode(column, QHeaderView::Interactive);
    fitColumns();
}

void ExportJobTree::fitColumns() {
    auto* header = this->header();
    header->resizeSection(LabelColumn, kit::px(kit::Size::ToggleCell));
    header->resizeSection(EnabledColumn, kit::px(kit::Size::ToggleCell));
    header->resizeSection(CompositionColumn, kit::px(kit::Size::DropdownWidthWide));
    header->resizeSection(RangeColumn,
                          kit::px(kit::Size::DropdownWidthWide) + kit::px(kit::Spacing::XL));
    header->resizeSection(StatusColumn,
                          kit::px(kit::Size::DropdownWidthWide) + kit::px(kit::Spacing::XXL));
}

QTreeWidgetItem* ExportJobTree::jobItem(const qulonglong job) const {
    for (int row = 0; row < topLevelItemCount(); ++row)
        if (topLevelItem(row)->data(NameColumn, kJobRole).toULongLong() == job)
            return topLevelItem(row);
    return nullptr;
}

QTreeWidgetItem* ExportJobTree::outputItem(const qulonglong job, const qulonglong output) const {
    if (auto* item = jobItem(job))
        for (int child = 0; child < item->childCount(); ++child)
            if (item->child(child)->data(NameColumn, kOutputRole).toULongLong() == output)
                return item->child(child);
    return nullptr;
}

int ExportJobTree::dropIndexAt(const QPoint position) const {
    const auto* item = itemAt(position);
    while (item != nullptr && item->parent() != nullptr)
        item = item->parent();
    if (item == nullptr)
        return topLevelItemCount();
    const int row = indexOfTopLevelItem(const_cast<QTreeWidgetItem*>(item));
    const QRect rect = visualItemRect(item);
    return position.y() > rect.top() + rect.height() / 2 ? row + 1 : row;
}

void ExportJobTree::resizeEvent(QResizeEvent* const event) {
    QTreeWidget::resizeEvent(event);
    // A narrow panel keeps the name and the status; the composition and range are in tooltips.
    const bool narrow = width() < kit::px(kit::Size::PanelMinWidth) + kit::px(kit::Size::PanelMinWidth);
    for (const int column : {CompositionColumn, RangeColumn})
        if (header()->isSectionHidden(column) != narrow)
            header()->setSectionHidden(column, narrow);
}

bool ExportJobTree::viewportEvent(QEvent* const event) {
    switch (event->type()) {
    case QEvent::MouseMove:
        setHoverRow(indexAt(static_cast<QMouseEvent*>(event)->position().toPoint()));
        break;
    case QEvent::Leave:
        setHoverRow({});
        break;
    default:
        break;
    }
    return QTreeWidget::viewportEvent(event);
}

void ExportJobTree::setHoverRow(const QModelIndex& index) {
    const QModelIndex row = index.isValid() ? index.siblingAtColumn(NameColumn) : QModelIndex{};
    if (QModelIndex(hoverRow_) == row)
        return;
    const auto repaint = [this](const QModelIndex& value) {
        if (!value.isValid())
            return;
        const QRect rect = visualRect(value);
        viewport()->update(QRect(0, rect.top(), viewport()->width(), rect.height()));
    };
    repaint(hoverRow_);
    hoverRow_ = QPersistentModelIndex(row);
    repaint(row);
}

void ExportJobTree::drawRow(QPainter* const painter, const QStyleOptionViewItem& option,
                            const QModelIndex& index) const {
    const int height = std::max(1, option.rect.height());
    const QRectF row(0, option.rect.top(), viewport()->width(), height);
    const bool alternate = ((option.rect.top() + verticalOffset()) / height) % 2 == 1;
    const auto base = alternate ? kit::Color::Surface : kit::Color::Background;
    const bool selected =
        selectionModel() != nullptr && selectionModel()->isSelected(index.siblingAtColumn(0));
    if (selected) {
        kit::paintSelectedRow(*painter, row, base, true);
    } else if (isEnabled() && hoverRow_.isValid() &&
               QModelIndex(hoverRow_) == index.siblingAtColumn(NameColumn)) {
        painter->fillRect(row, kit::color(kit::surfaceStep(base, 1)));
    } else if (alternate) {
        painter->fillRect(row, kit::color(kit::Color::Surface));
    }
    QTreeWidget::drawRow(painter, option, index);
    // The reorder drop indicator: an Accent line at the insertion point.
    if (dropRow_ >= 0 && !index.parent().isValid()) {
        const int r = index.row();
        const int hairline = kit::px(kit::Size::SelectionEdge);
        if (r == dropRow_)
            painter->fillRect(QRectF(0, option.rect.top(), viewport()->width(), hairline),
                              kit::color(kit::Color::Accent));
        else if (r == dropRow_ - 1 && dropRow_ == topLevelItemCount())
            painter->fillRect(QRectF(0, option.rect.top() + height - hairline, viewport()->width(),
                                     hairline),
                              kit::color(kit::Color::Accent));
    }
}

void ExportJobTree::mousePressEvent(QMouseEvent* const event) {
    pressedCheck_ = QPersistentModelIndex();
    const auto index = indexAt(event->position().toPoint());
    if (event->button() == Qt::LeftButton && index.isValid() && index.column() == EnabledColumn &&
        index.data(kEditableRole).toBool()) {
        // The check toggles on release; the press must not start a drag or move the selection.
        pressedCheck_ = index;
        return;
    }
    QTreeWidget::mousePressEvent(event);
}

void ExportJobTree::mouseReleaseEvent(QMouseEvent* const event) {
    if (pressedCheck_.isValid()) {
        const QModelIndex pressed = pressedCheck_;
        pressedCheck_ = QPersistentModelIndex();
        if (indexAt(event->position().toPoint()) == pressed) {
            const auto name = pressed.siblingAtColumn(NameColumn);
            Q_EMIT enabledToggled(name.data(kJobRole).toULongLong(),
                                  name.data(kOutputRole).toULongLong(),
                                  pressed.data(Qt::CheckStateRole).toInt() != Qt::Checked);
        }
        return;
    }
    QTreeWidget::mouseReleaseEvent(event);
}

void ExportJobTree::keyPressEvent(QKeyEvent* const event) {
    if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) {
        Q_EMIT removeRequested();
        event->accept();
        return;
    }
    QTreeWidget::keyPressEvent(event);
}

void ExportJobTree::startDrag(Qt::DropActions) {
    const auto* item = currentItem();
    // Only a job reorders; its outputs travel with it.
    if (item == nullptr || item->parent() != nullptr)
        return;
    auto* mime = new QMimeData;
    mime->setData(kJobMimeType,
                  QByteArray::number(item->data(NameColumn, kJobRole).toULongLong()));
    QDrag drag(this);
    drag.setMimeData(mime);
    (void)drag.exec(Qt::MoveAction, Qt::MoveAction);
    setDropRow(-1);
}

bool ExportJobTree::acceptsDrag(const QMimeData& mime) const {
    if (mime.hasFormat(kJobMimeType))
        return true;
    for (const auto& item : layerItemsFromMime(mime, session_))
        if (std::holds_alternative<document::CompositionId>(item))
            return true;
    return false;
}

void ExportJobTree::dragEnterEvent(QDragEnterEvent* const event) {
    if (acceptsDrag(*event->mimeData()))
        event->acceptProposedAction();
    else
        event->ignore();
}

void ExportJobTree::dragMoveEvent(QDragMoveEvent* const event) {
    if (!acceptsDrag(*event->mimeData())) {
        event->ignore();
        return;
    }
    setDropRow(event->mimeData()->hasFormat(kJobMimeType)
                   ? dropIndexAt(event->position().toPoint())
                   : -1);
    event->acceptProposedAction();
}

void ExportJobTree::dragLeaveEvent(QDragLeaveEvent* const event) {
    setDropRow(-1);
    QTreeWidget::dragLeaveEvent(event);
}

void ExportJobTree::setDropRow(const int index) {
    if (dropRow_ == index)
        return;
    dropRow_ = index;
    viewport()->update();
}

void ExportJobTree::dropEvent(QDropEvent* const event) {
    const int target = dropIndexAt(event->position().toPoint());
    setDropRow(-1);
    const auto& mime = *event->mimeData();
    if (mime.hasFormat(kJobMimeType)) {
        const auto job = mime.data(kJobMimeType).toULongLong();
        const auto* moved = jobItem(job);
        if (moved != nullptr) {
            // The index after the move: removing the job shifts a later target up by one.
            const int from = indexOfTopLevelItem(const_cast<QTreeWidgetItem*>(moved));
            const int to = target > from ? target - 1 : target;
            if (to != from)
                Q_EMIT moveRequested(job, to);
        }
        // The queue snapshot rebuilds the rows; Qt must not move the items itself.
        event->setDropAction(Qt::IgnoreAction);
        event->accept();
        return;
    }
    QList<qulonglong> compositions;
    for (const auto& item : layerItemsFromMime(mime, session_))
        if (const auto* composition = std::get_if<document::CompositionId>(&item))
            compositions.push_back(static_cast<qulonglong>(composition->value()));
    if (!compositions.isEmpty())
        Q_EMIT compositionsDropped(compositions);
    event->setDropAction(Qt::CopyAction);
    event->accept();
}

} // namespace bloom::ui::exportpanel
