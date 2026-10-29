// The Assets search row: collapsed until asked for, so the tree keeps the height; open while it
// filters, so a filter never hides assets out of sight.
#include "assets_editor_internal.hpp"
#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/icons.hpp>

namespace bloom::ui::assets {

SearchBar::SearchBar(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("assetsSearchBar"));
    setFixedHeight(kit::px(kit::Size::Control));
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(kit::px(kit::Spacing::Gutter));
    auto* field = new kit::KSearchField(this);
    field_ = field;
    field_->setObjectName(QStringLiteral("assetsSearchField"));
    field_->setAccessibleName(tr("Search assets"));
    field_->setPlaceholderText(tr("Search names and tags…"));
    field_->setToolTip(tr("Matches names and tags; tag:name matches tags only. Esc clears, then "
                          "closes"));
    field_->installEventFilter(this);
    row->addWidget(field_, 1);
    count_ = new kit::KLabel(QString{}, this, kit::TypeRole::UiSmall);
    count_->setObjectName(QStringLiteral("assetsSearchCount"));
    count_->setTone(kit::Color::Muted);
    count_->setAccessibleName(tr("Matching assets"));
    row->addWidget(count_);
    auto* close = new kit::KIconButton(this);
    close->setObjectName(QStringLiteral("assetsSearchCloseButton"));
    close->setIcon(kit::icon(kit::IconId::Close, kit::IconRole::Chrome));
    close->setToolTip(tr("Clear and Close Search (Esc)"));
    close->setAccessibleName(tr("Clear and close search"));
    connect(close, &kit::KIconButton::clicked, this, [this] { this->close(); });
    row->addWidget(close);
    connect(field_, &QLineEdit::textChanged, this, [this](const QString& text) {
        count_->setVisible(!text.trimmed().isEmpty());
        // Text set from anywhere (a Tags cell, a test, an automation) opens the row: a filter is
        // never applied out of sight.
        if (!text.isEmpty() && isHidden()) {
            show();
            if (visibilityChanged)
                visibilityChanged(true);
        }
    });
    count_->hide();
    hide();
}

void SearchBar::open() {
    const bool opening = isHidden();
    show();
    field_->setFocus(Qt::ShortcutFocusReason);
    field_->selectAll();
    if (opening && visibilityChanged)
        visibilityChanged(true);
}

void SearchBar::close() {
    field_->clear();
    const bool closing = !isHidden();
    hide();
    if (closing && visibilityChanged)
        visibilityChanged(false);
    if (closed)
        closed();
}

void SearchBar::setMatches(const int matches, const int total) {
    count_->setText(tr("%1 of %2").arg(matches).arg(total));
    count_->setToolTip(tr("%1 of %2 items match").arg(matches).arg(total));
}

bool SearchBar::eventFilter(QObject* watched, QEvent* event) {
    if (watched == field_ && (event->type() == QEvent::ShortcutOverride ||
                              event->type() == QEvent::KeyPress)) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape && key->modifiers() == Qt::NoModifier) {
            // The field owns Escape over the window's own Escape shortcut (Cancel RAM Preview).
            event->accept();
            if (event->type() == QEvent::KeyPress) {
                if (!field_->text().isEmpty())
                    field_->clear();
                else
                    close();
            }
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace bloom::ui::assets
