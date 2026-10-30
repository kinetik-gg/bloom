#include <bloom/ui/kit/popup_list.hpp>

#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/icons.hpp>
#include <bloom/ui/kit/mnemonic_style.hpp>
#include <bloom/ui/kit/painting.hpp>

#include <QAbstractItemModel>
#include <QApplication>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QListView>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QStyledItemDelegate>
#include <QToolTip>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace bloom::ui::kit {
namespace {

enum class RowKind : std::uint8_t { Entry, Heading, Separator };

[[nodiscard]] RowKind kindOf(const QModelIndex& index) {
    if (index.data(kListSeparatorRole).toBool())
        return RowKind::Separator;
    if (index.data(kListHeadingRole).toBool())
        return RowKind::Heading;
    return RowKind::Entry;
}

[[nodiscard]] bool choosable(const QModelIndex& index) {
    return index.isValid() && kindOf(index) == RowKind::Entry &&
           index.flags().testFlag(Qt::ItemIsEnabled);
}

// Draws a pixmap centred in `box`, on whole device pixels.
void drawCentred(QPainter& painter, const QRectF& box, const QPixmap& pixmap) {
    const qreal ratio = painter.device()->devicePixelRatio();
    const QSizeF extent = pixmap.deviceIndependentSize();
    painter.drawPixmap(
        QPointF(std::round((box.x() + (box.width() - extent.width()) / 2.0) * ratio) / ratio,
                std::round((box.y() + (box.height() - extent.height()) / 2.0) * ratio) / ratio),
        pixmap);
}

class ListDelegate final : public QStyledItemDelegate {
  public:
    ListDelegate(KPopupList& surface, QListView& view)
        : QStyledItemDelegate(&view), surface_(surface), view_(view) {}

    [[nodiscard]] QSize sizeHint(const QStyleOptionViewItem&,
                                 const QModelIndex& index) const override {
        switch (kindOf(index)) {
        case RowKind::Separator:
            return {0, surface_.separatorHeight()};
        case RowKind::Heading:
            return {0, surface_.headingHeight()};
        case RowKind::Entry:
            break;
        }
        return {0, surface_.rowHeight()};
    }

    [[nodiscard]] int textRight(const QRect& row) const { return row.right() + 1 - px(Spacing::MenuItemX); }

    [[nodiscard]] QString label(const QModelIndex& index) const {
        return index.data(Qt::DisplayRole).toString();
    }

    [[nodiscard]] bool elided(const QStyleOptionViewItem& option, const QModelIndex& index) const {
        const QFontMetrics metrics(kindOf(index) == RowKind::Heading ? font(TypeRole::UiSmall)
                                                                     : option.font);
        const int available = textRight(option.rect) - (option.rect.left() + KPopupList::textOffset());
        return metrics.horizontalAdvance(label(index)) > available;
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        const QRect row = option.rect;
        painter->save();
        switch (kindOf(index)) {
        case RowKind::Separator: {
            // The menu's divider: a Border hairline, inset so it reads as a break between groups.
            QRectF line(row.left() + px(Spacing::MenuItemX), row.center().y(),
                        row.width() - 2 * px(Spacing::MenuItemX), kHairlineWidth);
            painter->fillRect(snappedToDevicePixels(*painter, line), color(Color::Border));
            painter->restore();
            return;
        }
        case RowKind::Heading: {
            // A heading labels the rows under it: Faint, in the small role, at the row text inset.
            const QFont small = font(TypeRole::UiSmall);
            painter->setFont(small);
            painter->setPen(color(Color::Faint));
            const QRect text(row.left() + KPopupList::textOffset(), row.top(),
                             std::max(0, textRight(row) - row.left() - KPopupList::textOffset()),
                             row.height());
            painter->drawText(text, Qt::AlignVCenter | Qt::AlignLeft,
                              QFontMetrics(small).elidedText(label(index), Qt::ElideRight,
                                                              text.width()));
            painter->restore();
            return;
        }
        case RowKind::Entry:
            break;
        }

        const bool enabled = index.flags().testFlag(Qt::ItemIsEnabled) && view_.isEnabled();
        const bool current = enabled && view_.currentIndex() == index;
        const bool committed = index.row() == surface_.committedRow();
        const QIcon glyph = index.data(Qt::DecorationRole).value<QIcon>();
        // A committed row that carries its own icon (the panel switcher) is marked by one surface
        // step across the whole row, so its icon stays unframed; the Accent bar still wins.
        const bool committedPlate = committed && !glyph.isNull() && !current;
        if (current || committedPlate) {
            // One full-width bar, clipped to the frame's rounded inner edge by an antialiased
            // path -- the corner pixels blend instead of stepping.
            const QPoint offset = view_.viewport()->mapTo(&surface_, QPoint(0, 0));
            QPainterPath bar;
            bar.addRect(QRectF(row));
            const QPainterPath clip = surface_.rowClip().translated(-offset);
            painter->setRenderHint(QPainter::Antialiasing, true);
            painter->setPen(Qt::NoPen);
            painter->setBrush(color(current ? Color::Accent : Color::Field));
            painter->drawPath(bar.intersected(clip));
        }
        using kit::State;
        const QColor ink = current ? color(Color::OnAccent)
                                   : inkForState(Color::Foreground,
                                                 enabled ? State::Normal : State::Disabled);
        const int column = menuIconColumnWidth();
        const QRectF iconBox(row.left() + px(Spacing::MenuItemX),
                             row.top() + (row.height() - column) / 2, column, column);
        const qreal ratio = painter->device()->devicePixelRatio();
        if (!glyph.isNull()) {
            const QIcon::Mode mode =
                !enabled ? QIcon::Disabled : (current ? QIcon::Selected : QIcon::Normal);
            drawCentred(*painter, iconBox, glyph.pixmap(QSize(column, column), ratio, mode));
        } else if (committed) {
            drawCentred(*painter, iconBox,
                        iconPixmap(IconId::Check, Size::IconMedium, ink, ratio,
                                   iconWeight(IconRole::Chrome)));
        }
        painter->setFont(option.font);
        painter->setPen(ink);
        const int left = row.left() + KPopupList::textOffset();
        const QRect text(left, row.top(), std::max(0, textRight(row) - left), row.height());
        painter->drawText(text, Qt::AlignVCenter | Qt::AlignLeft | Qt::TextSingleLine,
                          option.fontMetrics.elidedText(label(index), Qt::ElideRight,
                                                        text.width()));
        painter->restore();
    }

    bool helpEvent(QHelpEvent* event, QAbstractItemView* view, const QStyleOptionViewItem& option,
                   const QModelIndex& index) override {
        if (event == nullptr || event->type() != QEvent::ToolTip || !index.isValid())
            return QStyledItemDelegate::helpEvent(event, view, option, index);
        // A row's own tooltip (a refusal, a file path) wins; otherwise an elided row shows its
        // whole label, and a row that is shown whole needs no tooltip at all.
        QString tip = index.data(Qt::ToolTipRole).toString();
        if (tip.isEmpty() && kindOf(index) != RowKind::Separator && elided(option, index))
            tip = label(index);
        if (tip.isEmpty()) {
            QToolTip::hideText();
            event->ignore();
            return true;
        }
        QToolTip::showText(event->globalPos(), tip, view->viewport(), option.rect);
        return true;
    }

  private:
    KPopupList& surface_;
    QListView& view_;
};

} // namespace

KPopupList::KPopupList(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("kPopupList"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    filterBox_ = new QWidget(this);
    auto* filterLayout = new QVBoxLayout(filterBox_);
    // The filter sits in its own band above the rows; the rows themselves still meet the frame.
    filterLayout->setContentsMargins(px(Spacing::XS), px(Spacing::XS), px(Spacing::XS),
                                     px(Spacing::XS));
    filter_ = new KSearchField(filterBox_);
    filter_->setObjectName(QStringLiteral("kListFilter"));
    filter_->setPlaceholderText(tr("Search"));
    filter_->setClearButtonEnabled(true);
    filterLayout->addWidget(filter_);
    filterBox_->hide();
    layout->addWidget(filterBox_);

    view_ = new QListView(this);
    view_->setObjectName(QStringLiteral("kDropdownList"));
    view_->setFrameShape(QFrame::NoFrame);
    view_->setMouseTracking(true);
    view_->setSelectionMode(QAbstractItemView::NoSelection);
    view_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    view_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    view_->setTextElideMode(Qt::ElideRight);
    view_->setFont(kit::font(TypeRole::Ui));
    view_->setFocusPolicy(Qt::StrongFocus);
    // The frame behind is the popup's own SurfaceRaised; the list adds no fill and no padding, so
    // its rows meet the frame edge to edge and nothing is left under the last one.
    view_->setAutoFillBackground(false);
    view_->viewport()->setAutoFillBackground(false);
    view_->setStyleSheet(QStringLiteral("QListView { background: transparent; border: none; }"));
    view_->setMinimumSize(0, 0);
    view_->setItemDelegate(new ListDelegate(*this, *view_));
    layout->addWidget(view_, 1);

    view_->installEventFilter(this);
    view_->viewport()->installEventFilter(this);
    filter_->installEventFilter(this);
    connect(filter_, &QLineEdit::textChanged, this, &KPopupList::filterChanged);
}

void KPopupList::setModel(QAbstractItemModel* model) {
    if (auto* previous = view_->model(); previous != nullptr)
        disconnect(previous, nullptr, this, nullptr);
    view_->setModel(model);
    if (model == nullptr)
        return;
    connect(model, &QAbstractItemModel::modelReset, this, &KPopupList::rowsChanged);
    connect(model, &QAbstractItemModel::layoutChanged, this, &KPopupList::rowsChanged);
    connect(model, &QAbstractItemModel::rowsInserted, this, &KPopupList::rowsChanged);
    connect(model, &QAbstractItemModel::rowsRemoved, this, &KPopupList::rowsChanged);
}

QAbstractItemModel* KPopupList::model() const { return view_->model(); }

void KPopupList::setUniformRows(const bool uniform) { view_->setUniformItemSizes(uniform); }

void KPopupList::setFilterVisible(const bool visible) { filterBox_->setVisible(visible); }

bool KPopupList::filterVisible() const noexcept { return !filterBox_->isHidden(); }

void KPopupList::setCommittedRow(const int row) {
    if (committedRow_ == row)
        return;
    committedRow_ = row;
    view_->viewport()->update();
}

int KPopupList::currentRow() const {
    const QModelIndex index = view_->currentIndex();
    return index.isValid() ? index.row() : -1;
}

bool KPopupList::isChoosable(const int row) const {
    const auto* source = view_->model();
    return source != nullptr && row >= 0 && row < source->rowCount() &&
           choosable(source->index(row, 0));
}

void KPopupList::setCurrentRow(const int row) {
    if (!isChoosable(row)) {
        view_->setCurrentIndex({});
        view_->viewport()->update();
        return;
    }
    const QModelIndex index = view_->model()->index(row, 0);
    if (view_->currentIndex() != index)
        view_->setCurrentIndex(index);
    view_->viewport()->update();
}

void KPopupList::stepCurrent(const int direction) {
    const auto* source = view_->model();
    if (source == nullptr || direction == 0)
        return;
    const int count = source->rowCount();
    int row = currentRow();
    if (row < 0)
        row = direction > 0 ? -1 : count;
    for (row += direction; row >= 0 && row < count; row += direction) {
        if (isChoosable(row)) {
            setCurrentRow(row);
            scrollToCurrent();
            return;
        }
    }
}

void KPopupList::stepToEnd(const bool last) {
    const auto* source = view_->model();
    if (source == nullptr)
        return;
    const int count = source->rowCount();
    for (int step = 0; step < count; ++step) {
        const int row = last ? count - 1 - step : step;
        if (isChoosable(row)) {
            setCurrentRow(row);
            scrollToCurrent();
            return;
        }
    }
}

void KPopupList::stepPage(const int direction) {
    const auto* source = view_->model();
    if (source == nullptr || direction == 0)
        return;
    const int count = source->rowCount();
    const int page = std::max(1, view_->viewport()->height() / std::max(1, rowHeight()) - 1);
    const int from = std::max(currentRow(), 0);
    const int target = std::clamp(from + direction * page, 0, std::max(0, count - 1));
    // The nearest choosable row at or past the page target, else the nearest one before it.
    for (int row = target; row >= 0 && row < count; row += direction)
        if (isChoosable(row)) {
            setCurrentRow(row);
            scrollToCurrent();
            return;
        }
    for (int row = target; row >= 0 && row < count; row -= direction)
        if (isChoosable(row)) {
            setCurrentRow(row);
            scrollToCurrent();
            return;
        }
}

int KPopupList::rowHeight() const { return menuRowHeight(view_->font()); }

int KPopupList::headingHeight() const {
    return QFontMetrics(kit::font(TypeRole::UiSmall)).height() + 2 * px(Spacing::XS);
}

int KPopupList::separatorHeight() const { return 2 * px(Spacing::MenuItemY); }

int KPopupList::fittedListHeight() const {
    const auto* source = view_->model();
    if (source == nullptr)
        return 0;
    const int cap = kListPopupMaxRows * rowHeight();
    const int count = source->rowCount();
    if (view_->uniformItemSizes())
        return std::min(cap, count * rowHeight());
    int height = 0;
    for (int row = 0; row < count && height < cap; ++row) {
        const QModelIndex index = source->index(row, 0);
        height += kindOf(index) == RowKind::Separator ? separatorHeight()
                  : kindOf(index) == RowKind::Heading ? headingHeight()
                                                      : rowHeight();
    }
    return std::min(cap, height);
}

int KPopupList::fittedHeight() const {
    return fittedListHeight() + (filterVisible() ? filterBox_->sizeHint().height() : 0);
}

int KPopupList::textOffset() { return menuItemTextOffset(); }

int KPopupList::rowWidthFor(const QString& text) const {
    return textOffset() + QFontMetrics(view_->font()).horizontalAdvance(text) +
           px(Spacing::MenuItemX);
}

void KPopupList::setFrameCorner(const qreal corner) {
    frameCorner_ = std::max<qreal>(0.0, corner);
    view_->viewport()->update();
}

QPainterPath KPopupList::rowClip() const {
    QPainterPath path;
    path.addRoundedRect(QRectF(rect()), frameCorner_, frameCorner_);
    return path;
}

void KPopupList::prepareForOpen() {
    if (filterVisible() && !filter_->text().isEmpty())
        filter_->clear();
    typed_.clear();
    setCurrentRow(committedRow_);
}

void KPopupList::scrollToCurrent() {
    const QModelIndex index = view_->currentIndex();
    if (index.isValid())
        view_->scrollTo(index, QAbstractItemView::EnsureVisible);
}

void KPopupList::chooseCurrent() {
    const int row = currentRow();
    if (isChoosable(row))
        Q_EMIT rowChosen(row);
}

void KPopupList::typeAhead(const QString& text) {
    const auto* source = view_->model();
    if (source == nullptr)
        return;
    if (!typedClock_.isValid() || typedClock_.elapsed() > QApplication::keyboardInputInterval())
        typed_.clear();
    typedClock_.start();
    typed_ += text;
    const int count = source->rowCount();
    // A fresh letter moves on from the current row, so repeating it cycles through the rows that
    // start with it; a longer prefix keeps refining from where it is.
    const int start = std::max(0, currentRow() + (typed_.size() == 1 ? 1 : 0));
    for (int step = 0; step < count; ++step) {
        const int row = (start + step) % count;
        if (isChoosable(row) &&
            source->index(row, 0).data().toString().startsWith(typed_, Qt::CaseInsensitive)) {
            setCurrentRow(row);
            scrollToCurrent();
            return;
        }
    }
}

bool KPopupList::handleKey(QKeyEvent* key, const bool fromFilter) {
    const bool plain = (key->modifiers() & (Qt::ControlModifier | Qt::AltModifier |
                                            Qt::MetaModifier)) == 0;
    switch (key->key()) {
    case Qt::Key_Up:
        stepCurrent(-1);
        return true;
    case Qt::Key_Down:
        stepCurrent(1);
        return true;
    case Qt::Key_PageUp:
        stepPage(-1);
        return true;
    case Qt::Key_PageDown:
        stepPage(1);
        return true;
    case Qt::Key_Home:
    case Qt::Key_End:
        // In the filter these edit the text.
        if (fromFilter)
            return false;
        stepToEnd(key->key() == Qt::Key_End);
        return true;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        chooseCurrent();
        return true;
    case Qt::Key_Tab:
    case Qt::Key_Backtab: {
        const bool forward =
            key->key() == Qt::Key_Tab && !key->modifiers().testFlag(Qt::ShiftModifier);
        chooseCurrent();
        Q_EMIT tabbed(forward);
        return true;
    }
    case Qt::Key_Escape:
        // The search field's two Escapes: the first clears a typed filter, the second closes.
        if (fromFilter && !filter_->text().isEmpty()) {
            filter_->clear();
            return true;
        }
        Q_EMIT dismissed();
        return true;
    default:
        break;
    }
    if (!fromFilter && plain && !key->text().isEmpty() && key->text().at(0).isPrint()) {
        typeAhead(key->text());
        return true;
    }
    return false;
}

bool KPopupList::eventFilter(QObject* watched, QEvent* event) {
    const bool fromFilter = watched == filter_;
    if ((watched == view_ || fromFilter) &&
        (event->type() == QEvent::KeyPress || event->type() == QEvent::ShortcutOverride)) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (event->type() == QEvent::ShortcutOverride) {
            // Claim the keys this list answers, so a window shortcut cannot take them first.
            switch (key->key()) {
            case Qt::Key_Up:
            case Qt::Key_Down:
            case Qt::Key_PageUp:
            case Qt::Key_PageDown:
            case Qt::Key_Return:
            case Qt::Key_Enter:
            case Qt::Key_Tab:
            case Qt::Key_Backtab:
            case Qt::Key_Escape:
                event->accept();
                return true;
            default:
                // Type-ahead letters belong to the list, not to a window shortcut.
                if (!fromFilter && !key->text().isEmpty() && key->text().at(0).isPrint() &&
                    (key->modifiers() & (Qt::ControlModifier | Qt::AltModifier)) == 0) {
                    event->accept();
                    return true;
                }
                return false;
            }
        }
        return handleKey(key, fromFilter);
    }
    if (watched == view_->viewport()) {
        switch (event->type()) {
        case QEvent::MouseMove: {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            const QModelIndex index = view_->indexAt(mouse->position().toPoint());
            // The bar follows the pointer onto any row that can be chosen; over a disabled row, a
            // heading or a separator there is no bar at all.
            setCurrentRow(choosable(index) ? index.row() : -1);
            return false;
        }
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonDblClick:
            // The press is the popup's to keep: no selection, no drag, no edit.
            return true;
        case QEvent::MouseButtonRelease: {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            const QModelIndex index = view_->indexAt(mouse->position().toPoint());
            // Only the left button chooses. A disabled row swallows the release and leaves the
            // popup open on a value the artist cannot have.
            if (mouse->button() == Qt::LeftButton && choosable(index))
                Q_EMIT rowChosen(index.row());
            return true;
        }
        default:
            break;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void KPopupList::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    view_->viewport()->update();
}

} // namespace bloom::ui::kit
