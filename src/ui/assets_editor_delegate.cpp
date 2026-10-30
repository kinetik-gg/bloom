// The Assets rows: AssetRowDelegate paints every cell (glyph, name, muted secondary text and column
// text) and AssetTree::drawRow paints the row behind them across the whole viewport, so
// alternation, hover and the selected recipe never stop at the last column or notch at an indent.
//
// Geometry (docs/ux/ui-grammar.md, Assets): a row is one Size::AssetRow; each level indents by
// Size::AssetIndent, which is the caret column; the kind glyph sits at the Name cell's left and the
// name one AssetIndent later, so a child's glyph starts exactly where its parent's name does.
#include "assets_editor_internal.hpp"
#include <QAbstractItemView>
#include <QEvent>
#include <QHeaderView>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QTreeView>
#include <algorithm>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/icons.hpp>
#include <bloom/ui/kit/painting.hpp>

namespace bloom::ui::assets {
namespace {
[[nodiscard]] bool selectedRow(const QStyleOptionViewItem& option) {
    return (option.state & QStyle::State_Selected) != 0;
}

[[nodiscard]] bool secondaryRow(const QModelIndex& index) {
    return index.siblingAtColumn(NameColumn).data(kSecondaryRowRole).toBool();
}

// The name's own ink, and the quieter ink of everything else on the row.
[[nodiscard]] kit::Color primaryInk(const QModelIndex& index, const bool selected) {
    return selected ? kit::Color::OnAccent : secondaryRow(index) ? kit::Color::Muted
                                                                 : kit::Color::Foreground;
}

[[nodiscard]] kit::Color columnInk(const bool selected) {
    return selected ? kit::Color::Foreground : kit::Color::Muted;
}

[[nodiscard]] kit::IconId glyphFor(const QStyleOptionViewItem& option, const QModelIndex& index) {
    auto glyph = static_cast<kit::IconId>(index.data(kGlyphRole).toInt());
    // An open folder shows it is open.
    if (glyph == kit::IconId::Folder)
        if (const auto* view = qobject_cast<const QTreeView*>(option.widget);
            view != nullptr && view->isExpanded(index))
            glyph = kit::IconId::FolderOpen;
    return glyph;
}
} // namespace

QRect AssetRowDelegate::glyphRect(const QRect& cell) {
    const int box = kit::px(kit::Size::IconMedium);
    return {cell.left(), cell.top() + (cell.height() - box) / 2, box, box};
}

int AssetRowDelegate::textLeft(const QRect& cell, const int column) {
    // Name: one indent past the glyph, which is where a child row's glyph starts. Other columns
    // inset by the header section's own padding, so a cell's text sits under its title.
    return cell.left() + (column == NameColumn ? kit::px(kit::Size::AssetIndent)
                                               : kit::px(kit::Spacing::S));
}

kit::Color AssetRowDelegate::glyphInk(const QModelIndex& index, const bool selected) {
    const auto tone = index.siblingAtColumn(NameColumn).data(kGlyphToneRole);
    return tone.isValid() ? static_cast<kit::Color>(tone.toInt()) : primaryInk(index, selected);
}

void AssetRowDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                             const QModelIndex& index) const {
    // No background here: AssetTree::drawRow has painted the whole row.
    painter->save();
    const bool selected = selectedRow(option);
    const QRect cell = option.rect;
    // The exclusive right edge of the text, one header padding in from the cell's own.
    const int right = cell.left() + cell.width() - kit::px(kit::Spacing::S);
    const int left = textLeft(cell, index.column());
    const auto metrics = option.fontMetrics;
    painter->setFont(option.font);
    const auto text = index.data(Qt::DisplayRole).toString();
    if (index.column() == NameColumn) {
        if (index.data(kGlyphRole).isValid()) {
            const qreal ratio = painter->device()->devicePixelRatioF();
            const auto pixmap = kit::iconPixmap(glyphFor(option, index), kit::IconRole::Chrome,
                                                glyphInk(index, selected), kit::State::Normal,
                                                ratio);
            painter->drawPixmap(glyphRect(cell).topLeft(), pixmap);
        }
        const auto secondary = index.data(kSecondaryTextRole).toString();
        const auto trailing = secondary.isEmpty() ? QString{} : QStringLiteral(" · ") + secondary;
        const int trailingWidth = metrics.horizontalAdvance(trailing);
        const int available = std::max(0, right - left - trailingWidth);
        const auto name = metrics.elidedText(text, Qt::ElideRight, available);
        const QRect nameRect(left, cell.top(), metrics.horizontalAdvance(name), cell.height());
        painter->setPen(kit::color(primaryInk(index, selected)));
        painter->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine, name);
        if (!trailing.isEmpty()) {
            painter->setPen(kit::color(kit::Color::Muted));
            painter->drawText(QRect(nameRect.left() + nameRect.width(), cell.top(), trailingWidth,
                                    cell.height()),
                              Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine, trailing);
        }
    } else if (!text.isEmpty()) {
        // A path keeps its file name: it elides in the middle, the full path is the tooltip.
        const auto mode = index.column() == LocationColumn ? Qt::ElideMiddle : Qt::ElideRight;
        painter->setPen(kit::color(secondaryRow(index) ? kit::Color::Muted : columnInk(selected)));
        painter->drawText(QRect(left, cell.top(), std::max(0, right - left), cell.height()),
                          Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine,
                          metrics.elidedText(text, mode, std::max(0, right - left)));
    }
    painter->restore();
}

QSize AssetRowDelegate::sizeHint(const QStyleOptionViewItem& option,
                                 const QModelIndex& index) const {
    const auto metrics = option.fontMetrics;
    int width = metrics.horizontalAdvance(index.data(Qt::DisplayRole).toString()) +
                kit::px(kit::Spacing::S) * 2;
    if (index.column() == NameColumn) {
        width += kit::px(kit::Size::AssetIndent) - kit::px(kit::Spacing::S);
        if (const auto secondary = index.data(kSecondaryTextRole).toString(); !secondary.isEmpty())
            width += metrics.horizontalAdvance(QStringLiteral(" · ") + secondary);
    }
    return {width, kit::px(kit::Size::AssetRow)};
}

QWidget* AssetRowDelegate::createEditor(QWidget* parent, const QStyleOptionViewItem& option,
                                        const QModelIndex& index) const {
    if (index.column() != NameColumn)
        return QStyledItemDelegate::createEditor(parent, option, index);
    // Only the Name column renames, through the kit line edit.
    auto* editor = new kit::KLineEdit(parent);
    editor->setObjectName(QStringLiteral("assetsRenameField"));
    editor->setAccessibleName(QObject::tr("Name"));
    return editor;
}

void AssetRowDelegate::updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& option,
                                            const QModelIndex& index) const {
    if (index.column() != NameColumn) {
        QStyledItemDelegate::updateEditorGeometry(editor, option, index);
        return;
    }
    // Over the name, beside the glyph, one row (one control) tall.
    const QRect cell = option.rect;
    const int left = textLeft(cell, NameColumn) - kit::px(kit::Spacing::XS);
    editor->setGeometry(QRect(left, cell.top(), std::max(0, cell.right() - left + 1),
                              cell.height()));
}

bool AssetRowDelegate::eventFilter(QObject* object, QEvent* event) {
    // Escape confirms everywhere (owner, 2026-10-29): it commits the typed name exactly as Return
    // and clicking away do, instead of the stock delegate's revert. Undo is the discard.
    auto* editor = qobject_cast<QWidget*>(object);
    if (editor != nullptr && event->type() == QEvent::KeyPress &&
        static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
        Q_EMIT commitData(editor);
        Q_EMIT closeEditor(editor, QAbstractItemDelegate::NoHint);
        return true;
    }
    return QStyledItemDelegate::eventFilter(object, event);
}

bool AssetTree::viewportEvent(QEvent* event) {
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

void AssetTree::setHoverRow(const QModelIndex& index) {
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
    hoverRow_ = row;
    repaint(row);
}

void AssetTree::drawRow(QPainter* painter, const QStyleOptionViewItem& option,
                        const QModelIndex& index) const {
    const int height = std::max(1, option.rect.height());
    const QRectF row(0, option.rect.top(), viewport()->width(), height);
    // Rows are uniform, so the visual row number is the row's offset down the whole list.
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
}

} // namespace bloom::ui::assets
