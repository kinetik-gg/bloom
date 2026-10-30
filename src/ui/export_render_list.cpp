// The render list's columns and rows. Geometry follows the Assets list (ui-grammar.md, Assets): a
// row is one Size::AssetRow, each level indents by Size::AssetIndent (the caret column), and a
// cell's text sits one header padding in from its edge, so it starts under its column title. The
// row background is drawRow()'s across the whole viewport; a job and its lines share one band, so
// a job reads as one block and the next job as the next.

#include "export_render_list.hpp"

#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/painting.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <QCoreApplication>
#include <QEvent>
#include <QHeaderView>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QTreeWidgetItemIterator>

#include <algorithm>
#include <array>

namespace bloom::ui::exportpanel {
namespace {

[[nodiscard]] QString tr(const char* source) {
    return QCoreApplication::translate("ExportRenderList", source);
}

[[nodiscard]] int cellPadding() { return kit::px(kit::Spacing::S); }

} // namespace

QString listColumnTitle(const int column) {
    switch (column) {
    case RenderColumn:
        return tr("Render");
    case NumberColumn:
        return tr("#");
    case CompositionColumn:
        return tr("Composition");
    case StatusColumn:
        return tr("Status");
    case StartedColumn:
        return tr("Started");
    case RenderTimeColumn:
        return tr("Render Time");
    default:
        return {};
    }
}

RowKind rowKind(const QTreeWidgetItem* item) {
    return item != nullptr ? static_cast<RowKind>(item->data(0, kRowKindRole).toInt()) : RowKind::Job;
}

document::ExportJobId rowJob(const QTreeWidgetItem* item) {
    return document::ExportJobId::fromRaw(item != nullptr ? item->data(0, kJobRole).toULongLong() : 0);
}

document::ExportOutputId rowOutput(const QTreeWidgetItem* item) {
    return document::ExportOutputId::fromRaw(item != nullptr ? item->data(0, kOutputRole).toULongLong()
                                                             : 0);
}

void RenderListDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                               const QModelIndex& index) const {
    // No background: RenderList::drawRow painted the row. Widgets own the other cells.
    const auto text = index.data(Qt::DisplayRole).toString();
    if (text.isEmpty() || index.parent().isValid())
        return;
    const int column = index.column();
    if (column != NumberColumn && column != CompositionColumn && column != StartedColumn &&
        column != RenderTimeColumn)
        return;
    painter->save();
    const bool selected = (option.state & QStyle::State_Selected) != 0;
    const auto ink = column == CompositionColumn ? (selected ? kit::Color::OnAccent : kit::Color::Foreground)
                                                 : (selected ? kit::Color::Foreground : kit::Color::Muted);
    const QRect cell = option.rect.adjusted(cellPadding(), 0, -cellPadding(), 0);
    painter->setFont(option.font);
    painter->setPen(kit::color(ink));
    painter->drawText(cell, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine,
                      option.fontMetrics.elidedText(text, Qt::ElideRight, std::max(0, cell.width())));
    painter->restore();
}

QSize RenderListDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const {
    // A line row that folds onto two lines says so through its own size hint.
    const auto own = index.data(Qt::SizeHintRole);
    if (own.isValid())
        return own.toSize();
    return {option.fontMetrics.horizontalAdvance(index.data(Qt::DisplayRole).toString()) +
                cellPadding() + cellPadding(),
            kit::px(kit::Size::AssetRow)};
}

void RenderListDelegate::updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& option,
                                              const QModelIndex&) const {
    editor->setGeometry(option.rect);
}

RenderList::RenderList(QWidget* const parent) : QTreeWidget(parent) {
    setObjectName(QStringLiteral("exportRenderList"));
    setAccessibleName(tr("Render list"));
    setItemDelegate(new RenderListDelegate(this));
    setColumnCount(ListColumnCount);
    QStringList titles;
    for (int column = 0; column < ListColumnCount; ++column)
        titles.push_back(listColumnTitle(column));
    setHeaderLabels(titles);
    auto* top = header();
    top->setObjectName(QStringLiteral("exportRenderListHeader"));
    top->setSectionsMovable(false);
    top->setHighlightSections(false);
    top->setSectionsClickable(false);
    top->setSectionResizeMode(QHeaderView::Fixed);
    top->setStretchLastSection(true);
    top->setMinimumSectionSize(0);
    setRootIsDecorated(true);
    // One indent per level, which is also the caret column (ui-grammar.md, Assets).
    setIndentation(kit::px(kit::Size::AssetIndent));
    setUniformRowHeights(false);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setSelectionBehavior(QAbstractItemView::SelectRows);
    setAllColumnsShowFocus(true);
    setAlternatingRowColors(false);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setExpandsOnDoubleClick(true);
    setContextMenuPolicy(Qt::CustomContextMenu);
    viewport()->setMouseTracking(true);
    // A job row can be picked up and dropped elsewhere in the list; the editor takes the drop, so
    // drops from Assets and moves share one indicator and one place to land.
    setDragEnabled(true);
    setDragDropMode(QAbstractItemView::DragOnly);
    setAcceptDrops(false);
    viewport()->setAcceptDrops(false);
    connect(this, &QWidget::customContextMenuRequested, this, [this](const QPoint position) {
        auto* item = itemAt(position);
        if (item != nullptr && rowKind(item) == RowKind::Job)
            Q_EMIT menuRequested(static_cast<qulonglong>(rowJob(item).value()),
                                 viewport()->mapToGlobal(position));
    });
}

int RenderList::lineLabelWidth() const {
    const QFontMetrics metrics(kit::font(kit::TypeRole::Ui));
    int width = 0;
    for (const auto& text : {tr("Render Settings:"), tr("Output Module:"), tr("Output To:")})
        width = std::max(width, metrics.horizontalAdvance(text));
    return width + kit::px(kit::Spacing::XXS);
}

void RenderList::setModuleWidth(const int width) { moduleWidth_ = width; }

int RenderList::depthLeft(const int depth) const { return indentation() * (depth + 1); }

int RenderList::valueLeft() const {
    return depthLeft(1) + lineLabelWidth() + kit::px(kit::Spacing::S);
}

QTreeWidgetItem* RenderList::jobItem(const document::ExportJobId job) const {
    for (int index = 0; index < topLevelItemCount(); ++index)
        if (rowJob(topLevelItem(index)) == job)
            return topLevelItem(index);
    return nullptr;
}

QTreeWidgetItem* RenderList::jobItemAt(const QPoint position) const {
    auto* item = itemAt(position);
    while (item != nullptr && item->parent() != nullptr)
        item = item->parent();
    return item;
}

void RenderList::fitColumns() {
    if (fitting_)
        return;
    fitting_ = true;
    auto* top = header();
    const auto metrics = fontMetrics();
    const auto headerMetrics = top->fontMetrics();
    const int padding = cellPadding() + cellPadding() + kit::px(kit::Size::Hairline);
    std::array<int, ListColumnCount> widths{};
    const auto at = [&widths](const int column) -> int& { return widths[static_cast<std::size_t>(column)]; };
    bool progress = false;
    for (int column = 0; column < ListColumnCount; ++column)
        at(column) =
            listColumnTitle(column).isEmpty() ? 0 : headerMetrics.horizontalAdvance(listColumnTitle(column)) + padding;
    for (int index = 0; index < topLevelItemCount(); ++index) {
        const auto* item = topLevelItem(index);
        for (const int column : {NumberColumn, CompositionColumn, StatusColumn, StartedColumn, RenderTimeColumn})
            at(column) = std::max(at(column), metrics.horizontalAdvance(item->text(column)) + padding);
        progress = progress || item->data(StatusColumn, Qt::UserRole).toBool();
    }
    // The Render cell holds the row's caret (the tree's indent) and the centred check box.
    at(RenderColumn) = std::max(at(RenderColumn), indentation() + kit::px(kit::Size::ToggleCell) + padding);
    // A running job's thin bar sits after its words in the Status cell.
    if (progress)
        at(StatusColumn) += kit::px(kit::Size::DropdownWidth) + kit::px(kit::Spacing::S);
    at(CompositionColumn) =
        std::clamp(at(CompositionColumn), kit::px(kit::Size::DropdownWidthWide),
                   kit::px(kit::Size::DropdownWidthExpanded) + kit::px(kit::Size::DropdownWidthExpanded));
    at(ActionsColumn) = kit::px(kit::Size::Control) + kit::px(kit::Spacing::S);

    const int available = viewport()->width();
    const auto total = [&at, top] {
        int sum = 0;
        for (int column = 0; column < ListColumnCount; ++column)
            if (!top->isSectionHidden(column))
                sum += at(column);
        return sum;
    };
    top->setSectionHidden(StartedColumn, false);
    top->setSectionHidden(RenderTimeColumn, false);
    // Narrow: the time columns go first, then Composition and Status give way.
    if (total() > available)
        top->setSectionHidden(StartedColumn, true);
    if (total() > available)
        top->setSectionHidden(RenderTimeColumn, true);
    if (const int over = total() - available; over > 0) {
        const int floor = kit::px(kit::Size::ValueCellMin);
        const int give = std::min(over, std::max(0, at(CompositionColumn) - floor));
        at(CompositionColumn) -= give;
        if (const int still = over - give; still > 0)
            at(StatusColumn) = std::max(floor, at(StatusColumn) - still);
    }
    for (int column = 0; column + 1 < ListColumnCount; ++column)
        if (top->sectionSize(column) != at(column))
            top->resizeSection(column, at(column));
    fitting_ = false;
}

void RenderList::resizeEvent(QResizeEvent* const event) {
    QTreeWidget::resizeEvent(event);
    fitColumns();
    Q_EMIT widthChanged();
}

bool RenderList::viewportEvent(QEvent* const event) {
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

void RenderList::setHoverRow(const QModelIndex& index) {
    QModelIndex row = index.isValid() ? index.siblingAtColumn(0) : QModelIndex{};
    // Only a job row is a row the pointer can act on; its lines are controls.
    if (row.isValid() && row.parent().isValid())
        row = {};
    if (QModelIndex(hoverRow_) == row)
        return;
    const auto repaint = [this](const QModelIndex& value) {
        if (!value.isValid())
            return;
        const QRect rect = visualRect(value);
        viewport()->update(QRect(0, rect.top(), viewport()->width(), rect.height()));
    };
    repaint(hoverRow_);
    hoverRow_ = row;
    repaint(row);
}

void RenderList::drawRow(QPainter* painter, const QStyleOptionViewItem& option,
                         const QModelIndex& index) const {
    const QRectF row(0, option.rect.top(), viewport()->width(), std::max(1, option.rect.height()));
    QModelIndex job = index.siblingAtColumn(0);
    while (job.parent().isValid())
        job = job.parent();
    // A job and its lines are one band; the bands alternate as Assets rows do.
    const bool alternate = job.row() % 2 == 1;
    const auto base = alternate ? kit::Color::Surface : kit::Color::Background;
    const bool isJob = !index.parent().isValid();
    const bool selected = isJob && selectionModel() != nullptr &&
                          selectionModel()->isSelected(index.siblingAtColumn(0));
    if (selected)
        kit::paintSelectedRow(*painter, row, base, true);
    else if (isJob && isEnabled() && hoverRow_.isValid() && QModelIndex(hoverRow_) == index.siblingAtColumn(0))
        painter->fillRect(row, kit::color(kit::surfaceStep(base, 1)));
    else if (alternate)
        painter->fillRect(row, kit::color(kit::Color::Surface));
    QTreeWidget::drawRow(painter, option, index);
}

QStringList RenderList::mimeTypes() const { return {QString::fromLatin1(kExportJobsMimeType)}; }

QMimeData* RenderList::mimeData(const QList<QTreeWidgetItem*>& items) const {
    QStringList ids;
    for (const auto* item : items)
        if (rowKind(item) == RowKind::Job)
            ids.push_back(QString::number(rowJob(item).value()));
    if (ids.isEmpty())
        return nullptr;
    auto* mime = new QMimeData;
    mime->setData(QString::fromLatin1(kExportJobsMimeType), ids.join(QLatin1Char('\n')).toLatin1());
    return mime;
}

} // namespace bloom::ui::exportpanel
