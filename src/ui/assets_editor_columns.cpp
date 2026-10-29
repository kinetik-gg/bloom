// The Assets column controller: which columns show, their widths and the sort, kept as a per-user
// setting (never project data). Name stretches, so the rows always span the panel; every other
// shown column fits its content until the user drags it, and a dragged width is remembered.
#include "assets_editor_internal.hpp"
#include <QAction>
#include <QHeaderView>
#include <QMenu>
#include <QSettings>
#include <QTreeWidgetItemIterator>
#include <QVariant>
#include <algorithm>
#include <bloom/ui/kit/tokens.hpp>

namespace bloom::ui::assets {

QString columnTitle(const int column) {
    switch (column) {
    case NameColumn:
        return QObject::tr("Name");
    case TypeColumn:
        return QObject::tr("Type");
    case DimensionsColumn:
        return QObject::tr("Dimensions");
    case DurationColumn:
        return QObject::tr("Duration");
    case FrameRateColumn:
        return QObject::tr("Frame Rate");
    case FileSizeColumn:
        return QObject::tr("File Size");
    case TagsColumn:
        return QObject::tr("Tags");
    case LocationColumn:
        return QObject::tr("File Location");
    default:
        return {};
    }
}

ColumnController::ColumnController(QTreeWidget& tree) : QObject(&tree), tree_(tree) {
    tree_.setColumnCount(ColumnCount);
    QStringList titles;
    for (int column = 0; column < ColumnCount; ++column)
        titles.push_back(columnTitle(column));
    tree_.setHeaderLabels(titles);
    auto* header = tree_.header();
    header->setObjectName(QStringLiteral("assetsTreeHeader"));
    header->setSectionsMovable(true);
    header->setFirstSectionMovable(false);
    header->setStretchLastSection(false);
    header->setHighlightSections(false);
    header->setSectionResizeMode(QHeaderView::Interactive);
    // Name takes whatever the other columns leave, at every panel width.
    header->setSectionResizeMode(NameColumn, QHeaderView::Stretch);
    header->setContextMenuPolicy(Qt::CustomContextMenu);
    header->setToolTip(
        QObject::tr("Click to sort, drag to resize or reorder; right-click to choose columns"));
    for (int column = TypeColumn; column < ColumnCount; ++column)
        header->setSectionHidden(column, !defaultColumnVisible(column));
    tree_.sortByColumn(NameColumn, Qt::AscendingOrder);
    connect(header, &QHeaderView::sectionResized, this, [this](const int column, int, int) {
        // A width the user dragged stays; Name's is never the user's (it stretches).
        if (!fitting_ && !restoring_ && column != NameColumn &&
            !tree_.header()->isSectionHidden(column))
            sized_.insert(column);
    });
    connect(header, &QHeaderView::sortIndicatorChanged, this, [this] { save(); });
    connect(header, &QHeaderView::sectionMoved, this, [this] { save(); });
}

void ColumnController::restore() {
    QSettings settings;
    sized_.clear();
    // A comma-separated list: a one-item QVariantList would read back from an INI file as a bare
    // string.
    for (const auto& value :
         settings.value(QString::fromLatin1(kSizedColumnsKey)).toString().split(QLatin1Char(','),
                                                                               Qt::SkipEmptyParts))
        if (const int column = value.toInt(); column > NameColumn && column < ColumnCount)
            sized_.insert(column);
    const auto state = settings.value(QString::fromLatin1(kHeaderStateKey)).toByteArray();
    auto* header = tree_.header();
    // Restoring re-emits the sort and resize signals; none of them is the user's, so nothing is
    // saved or marked as dragged while it runs.
    restoring_ = true;
    if (!state.isEmpty() && header->restoreState(state)) {
        // Name is never hidden and always stretches, whatever a saved state says.
        header->setSectionHidden(NameColumn, false);
        header->setSectionResizeMode(NameColumn, QHeaderView::Stretch);
        tree_.setSortingEnabled(true);
    }
    restoring_ = false;
}

void ColumnController::save() const {
    if (restoring_)
        return;
    QSettings settings;
    settings.setValue(QString::fromLatin1(kHeaderStateKey), tree_.header()->saveState());
    QList<int> columns(sized_.begin(), sized_.end());
    std::ranges::sort(columns);
    QStringList sized;
    for (const int column : columns)
        sized.push_back(QString::number(column));
    settings.setValue(QString::fromLatin1(kSizedColumnsKey), sized.join(QLatin1Char(',')));
}

void ColumnController::setVisible(const int column, const bool visible) {
    if (column <= NameColumn || column >= ColumnCount)
        return;
    tree_.header()->setSectionHidden(column, !visible);
    if (visible && !sized_.contains(column))
        fitColumn(column);
    save();
}

void ColumnController::fill(QMenu& menu) {
    menu.clear();
    for (int index = TypeColumn; index < ColumnCount; ++index) {
        auto* action = menu.addAction(columnTitle(index));
        action->setObjectName(QStringLiteral("assetsColumnAction%1").arg(index));
        action->setCheckable(true);
        action->setChecked(!tree_.header()->isSectionHidden(index));
        connect(action, &QAction::toggled, this,
                [this, index](const bool visible) { setVisible(index, visible); });
    }
}

void ColumnController::fitColumn(const int column) {
    if (column == NameColumn || tree_.header()->isSectionHidden(column))
        return;
    // The widest cell of every row, collapsed or scrolled away included (the view's own
    // sizeHintForColumn() reads only the rows on screen), or the title, whichever is wider; never
    // below one value cell, so an empty column still reads as a column.
    // One header padding each side, and one pixel for a fractional advance rounding down.
    const auto padding =
        kit::px(kit::Spacing::S) + kit::px(kit::Spacing::S) + kit::px(kit::Size::Hairline);
    const auto metrics = tree_.fontMetrics();
    int width = tree_.header()->fontMetrics().horizontalAdvance(columnTitle(column)) + padding;
    for (QTreeWidgetItemIterator it(&tree_); *it; ++it)
        width = std::max(width, metrics.horizontalAdvance((*it)->text(column)) + padding);
    width = std::max(width, kit::px(kit::Size::ValueCellMin));
    fitting_ = true;
    tree_.header()->resizeSection(column, width);
    fitting_ = false;
}

void ColumnController::fit() {
    for (int column = TypeColumn; column < ColumnCount; ++column)
        if (!sized_.contains(column))
            fitColumn(column);
}

} // namespace bloom::ui::assets
