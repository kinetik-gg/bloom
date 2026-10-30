#include <bloom/ui/kit/dropdown_popup.hpp>

#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/popup_list.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <QAbstractItemModel>
#include <QListView>
#include <QRegularExpression>
#include <QSortFilterProxyModel>

#include <algorithm>
#include <cmath>

namespace bloom::ui::kit {
namespace {

[[nodiscard]] int frameBorderWidth() {
    return std::max(1, static_cast<int>(std::lround(kHairlineWidth)));
}

} // namespace

KDropdownPopup::KDropdownPopup(QWidget* parent) : KPopupWindow(parent, Elevation::Popup) {
    setObjectName(QStringLiteral("kDropdownPopup"));
    list_ = new KPopupList(this);
    list_->setObjectName(QStringLiteral("kDropdownSurface"));
    list_->setUniformRows(true);
    setContent(list_, list_->view());

    connect(list_, &KPopupList::rowChosen, this, [this](const int row) {
        const int index = sourceRow(row);
        close();
        Q_EMIT itemChosen(index);
    });
    connect(list_, &KPopupList::tabbed, this, [this](const bool forward) {
        close();
        Q_EMIT focusMoveRequested(forward);
    });
    connect(list_, &KPopupList::dismissed, this, &QWidget::close);
    // Rows rebuilt under an open popup (a vocabulary refreshed by the document) re-fit it rather
    // than leaving it at the height of the rows it had.
    connect(list_, &KPopupList::rowsChanged, this, [this] {
        list_->setCommittedRow(viewRow(committedSource_));
        if (isVisible())
            refitToRows();
    });
}

void KDropdownPopup::setModel(QAbstractItemModel* model) {
    sourceModel_ = model;
    if (proxy_ != nullptr)
        proxy_->setSourceModel(model);
    list_->setModel(searchable_ && proxy_ != nullptr ? static_cast<QAbstractItemModel*>(proxy_)
                                                     : model);
}

void KDropdownPopup::setSearchable(const bool searchable) {
    if (searchable_ == searchable)
        return;
    searchable_ = searchable;
    list_->setFilterVisible(searchable_);
    if (searchable_ && proxy_ == nullptr) {
        // One proxy for the popup's life, however often searchability is switched.
        proxy_ = new QSortFilterProxyModel(this);
        proxy_->setFilterCaseSensitivity(Qt::CaseInsensitive);
        proxy_->setFilterRole(Qt::DisplayRole);
        connect(list_, &KPopupList::filterChanged, this, [this](const QString& text) {
            proxy_->setFilterRegularExpression(QRegularExpression(
                QRegularExpression::escape(text), QRegularExpression::CaseInsensitiveOption));
            list_->setCommittedRow(viewRow(committedSource_));
            // Typing keeps a row current, so Enter always has something to choose.
            if (!list_->isChoosable(list_->currentRow()))
                list_->stepCurrent(1);
            if (isVisible())
                refitToRows();
        });
    }
    if (proxy_ != nullptr)
        proxy_->setSourceModel(sourceModel_);
    list_->setModel(searchable_ ? static_cast<QAbstractItemModel*>(proxy_) : sourceModel_);
}

void KDropdownPopup::setUniformRows(const bool uniform) { list_->setUniformRows(uniform); }

QListView* KDropdownPopup::view() const noexcept { return list_->view(); }

QWidget* KDropdownPopup::surface() const noexcept { return list_; }

int KDropdownPopup::sourceRow(const int row) const {
    if (!searchable_ || proxy_ == nullptr || row < 0)
        return row;
    return proxy_->mapToSource(proxy_->index(row, 0)).row();
}

int KDropdownPopup::viewRow(const int source) const {
    if (!searchable_ || proxy_ == nullptr || source < 0 || sourceModel_ == nullptr)
        return source;
    const QModelIndex mapped = proxy_->mapFromSource(sourceModel_->index(source, 0));
    return mapped.isValid() ? mapped.row() : -1;
}

void KDropdownPopup::setWidestRowText(const int widestTextWidth) { widestText_ = widestTextWidth; }

QSize KDropdownPopup::wantedFrameSize(const int anchorWidth) const {
    const int border = frameBorderWidth();
    const int rows = KPopupList::textOffset() + widestText_ + px(Spacing::MenuItemX);
    // Exactly the rows (and the filter, when searchable) plus the frame's own hairlines: nothing
    // under the last row.
    return {std::max(anchorWidth, rows) + 2 * border, list_->fittedHeight() + 2 * border};
}

void KDropdownPopup::refitToRows() { refit(wantedFrameSize(anchorWidth_)); }

void KDropdownPopup::openFor(const QWidget& anchor, const int committedRow,
                             const int anchorTextOffset) {
    committedSource_ = committedRow;
    list_->prepareForOpen();
    list_->setCommittedRow(viewRow(committedRow));
    list_->setCurrentRow(viewRow(committedRow));
    const QRect visual = visualGlobalRect(anchor);
    // A zoomed node card draws its field smaller than its widget; its text offset scales with it.
    const qreal scale =
        anchor.width() > 0 ? static_cast<qreal>(visual.width()) / anchor.width() : 1.0;
    const int shift =
        std::max(0, KPopupList::textOffset() + frameBorderWidth() -
                        static_cast<int>(std::lround(anchorTextOffset * scale)));
    anchorWidth_ = visual.width() + shift - 2 * frameBorderWidth();
    KPopupWindow::openFor(anchor, wantedFrameSize(anchorWidth_), shift);
    list_->setFrameCorner(translucent() ? radiusPx(Radius::Small, 0) - frameBorderWidth() : 0.0);
    list_->scrollToCurrent();
}

} // namespace bloom::ui::kit
