#include <bloom/ui/kit/search_popup.hpp>

#include <bloom/ui/kit/dropdown_popup.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <QFrame>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListView>
#include <QMouseEvent>
#include <QScreen>
#include <QSize>
#include <QStandardItemModel>
#include <QVBoxLayout>

#include <algorithm>
#include <cstddef>
#include <map>
#include <utility>
#include <vector>

namespace bloom::ui::kit {
namespace {

// A result row and a section heading. A heading is deliberately the shorter of the two: it labels
// the rows under it rather than competing with them.
[[nodiscard]] int resultRowHeight() { return px(Size::Control); }
[[nodiscard]] int sectionRowHeight() { return px(Size::ControlCompact); }

// How much of the list may be on screen before it scrolls. A floor-free height means a short list
// is exactly its own rows; this is the other end -- a long one stops growing instead of running off
// the screen.
[[nodiscard]] int maximumListHeight() { return resultRowHeight() * 12; }

// How well an entry's own NAME answers the query (task FIX1, item A). Keywords are what make an
// entry findable -- a node is listed for the socket kinds it carries -- but they must never outrank
// a name: typing "Scalar" and pressing Enter has to add the node CALLED Scalar, not the first node
// in the list that happens to carry a Scalar socket. Zero means "matched on keywords alone".
[[nodiscard]] int nameRelevance(const QString& label, const QString& query) {
    if (query.isEmpty())
        return 0;
    if (label.compare(query, Qt::CaseInsensitive) == 0)
        return 4;
    if (label.startsWith(query, Qt::CaseInsensitive))
        return 3;
    for (const auto& word : label.split(' ', Qt::SkipEmptyParts))
        if (word.startsWith(query, Qt::CaseInsensitive))
            return 2;
    if (label.contains(query, Qt::CaseInsensitive))
        return 1;
    return 0;
}

} // namespace

KSearchPopup::KSearchPopup(QWidget* parent) : QWidget(parent, Qt::Popup) {
    setObjectName(QStringLiteral("kSearchPopup"));
    setAccessibleName(tr("Add node search"));
    setAttribute(Qt::WA_TranslucentBackground);
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    dropdown_ = new KDropdownPopup(this);
    dropdown_->setWindowFlags(Qt::Widget);
    outer->addWidget(dropdown_);
    field_ = new QLineEdit(dropdown_->surface());
    field_->setObjectName(QStringLiteral("kSearchFilter"));
    field_->setAccessibleName(tr("Filter nodes by name or socket kind"));
    field_->setPlaceholderText(tr("Search nodes…"));
    field_->setFont(kit::font(TypeRole::Ui));
    field_->setStyleSheet(expandTokens(QStringLiteral(R"(
QLineEdit#kSearchFilter {
    background: {color.Surface}; color: {color.Foreground};
    border: {border.Hairline}px solid {color.Border};
    padding: {space.XS}px {space.M}px;
}
QLineEdit#kSearchFilter:focus { border-color: {color.Accent}; }
)")));
    static_cast<QVBoxLayout*>(dropdown_->surface()->layout())->insertWidget(0, field_);
    model_ = new QStandardItemModel(this);
    dropdown_->setModel(model_);
    // Headings and results are different heights, so the view may not assume one row height for all
    // of them -- and the height the list is pinned to below is the sum of the real ones.
    dropdown_->view()->setUniformItemSizes(false);
    field_->installEventFilter(this);
    dropdown_->view()->installEventFilter(this);
    connect(field_, &QLineEdit::textChanged, this, &KSearchPopup::filter);
    connect(dropdown_, &KDropdownPopup::itemChosen, this, &KSearchPopup::choose);
}

void KSearchPopup::setEntries(std::vector<SearchEntry> entries) {
    entries_ = std::move(entries);
    field_->clear();
    filter();
}

void KSearchPopup::filter() {
    model_->clear();
    const QString query = field_->text().simplified();
    const auto words = query.split(' ', Qt::SkipEmptyParts);
    // Two passes. The first keeps every entry whose name or keywords carry all the typed words and
    // scores it on its NAME alone; the second lists the surviving entries by relevance -- sections
    // ordered by their own best match, entries ordered inside them -- so the row Enter lands on is
    // the one the artist was naming. With an empty query every score is zero and the caller's own
    // category order survives untouched.
    struct Match final {
        const SearchEntry* entry = nullptr;
        int relevance = 0;
        std::size_t order = 0;
    };
    std::vector<Match> matches;
    matches.reserve(entries_.size());
    for (std::size_t index = 0; index < entries_.size(); ++index) {
        const auto& entry = entries_[index];
        const QString haystack = entry.label + ' ' + entry.keywords;
        if (!std::ranges::all_of(words, [&](const auto& word) {
                return haystack.contains(word, Qt::CaseInsensitive);
            }))
            continue;
        matches.push_back({&entry, nameRelevance(entry.label, query), index});
    }
    std::vector<QString> sectionOrder;
    std::map<QString, std::pair<int, std::size_t>> sectionRank;
    for (const auto& match : matches) {
        const auto existing = sectionRank.find(match.entry->section);
        if (existing == sectionRank.end()) {
            sectionOrder.push_back(match.entry->section);
            sectionRank.emplace(match.entry->section, std::pair{match.relevance, match.order});
        } else if (match.relevance > existing->second.first) {
            existing->second = {match.relevance, existing->second.second};
        }
    }
    std::ranges::stable_sort(sectionOrder, [&](const QString& left, const QString& right) {
        const auto& leftRank = sectionRank.at(left);
        const auto& rightRank = sectionRank.at(right);
        if (leftRank.first != rightRank.first)
            return leftRank.first > rightRank.first;
        return leftRank.second < rightRank.second;
    });
    std::ranges::stable_sort(matches, [&](const Match& left, const Match& right) {
        const auto leftSection =
            std::ranges::find(sectionOrder, left.entry->section) - sectionOrder.begin();
        const auto rightSection =
            std::ranges::find(sectionOrder, right.entry->section) - sectionOrder.begin();
        if (leftSection != rightSection)
            return leftSection < rightSection;
        if (left.relevance != right.relevance)
            return left.relevance > right.relevance;
        return left.order < right.order;
    });

    QString openSection;
    bool sectionOpen = false;
    int height = 0;
    for (const auto& match : matches) {
        const auto& entry = *match.entry;
        // A heading is emitted only once the section has something to head, so a filter that
        // matches nothing in a section leaves no empty heading behind.
        if (!entry.section.isEmpty() && (!sectionOpen || entry.section != openSection)) {
            auto* heading = new QStandardItem(entry.section);
            heading->setData(true, kSearchSectionRole);
            heading->setData(QSize(0, sectionRowHeight()), Qt::SizeHintRole);
            heading->setFont(kit::font(TypeRole::UiSmall));
            heading->setForeground(color(Color::Faint));
            // No flags at all: a heading is neither selectable nor choosable, which is also what
            // keeps keyboard navigation -- which steps over anything not enabled -- unchanged.
            heading->setFlags(Qt::NoItemFlags);
            model_->appendRow(heading);
            height += sectionRowHeight();
            openSection = entry.section;
            sectionOpen = true;
        }
        // The label alone. A refused result's reason is its tooltip and nothing else.
        auto* item = new QStandardItem(entry.label);
        item->setData(entry.key, Qt::UserRole);
        item->setData(QSize(0, resultRowHeight()), Qt::SizeHintRole);
        item->setToolTip(entry.refusal);
        item->setEnabled(entry.refusal.isEmpty());
        model_->appendRow(item);
        height += resultRowHeight();
    }
    if (model_->rowCount() == 0) {
        auto* item = new QStandardItem(tr("No matching nodes"));
        item->setData(QSize(0, resultRowHeight()), Qt::SizeHintRole);
        item->setEnabled(false);
        model_->appendRow(item);
        height = resultRowHeight();
    }
    dropdown_->view()->setCurrentIndex({});
    step(1);
    dropdown_->view()->setFixedHeight(std::min(height, maximumListHeight()));
    adjustSize();
}

void KSearchPopup::openAt(const QPoint globalPosition) {
    dropdown_->show();
    setMinimumWidth(px(Size::ControlRoomy) * 16);
    adjustSize();
    QPoint position = globalPosition;
    if (const auto* screen = QGuiApplication::screenAt(globalPosition)) {
        const auto available = screen->availableGeometry();
        position.setX(std::clamp(position.x(), available.left(),
                                 std::max(available.left(), available.right() - width())));
        position.setY(std::clamp(position.y(), available.top(),
                                 std::max(available.top(), available.bottom() - height())));
    }
    move(position);
    show();
    field_->setFocus(Qt::PopupFocusReason);
}

void KSearchPopup::choose(const int row) {
    const auto index = model_->index(row, 0);
    if (!index.isValid() || !index.flags().testFlag(Qt::ItemIsEnabled))
        return;
    const QString key = index.data(Qt::UserRole).toString();
    close();
    Q_EMIT entryChosen(key);
}

void KSearchPopup::step(const int direction) {
    int row = dropdown_->view()->currentIndex().row();
    if (row < 0)
        row = direction > 0 ? -1 : model_->rowCount();
    for (row += direction; row >= 0 && row < model_->rowCount(); row += direction) {
        const auto index = model_->index(row, 0);
        if (index.flags().testFlag(Qt::ItemIsEnabled)) {
            dropdown_->view()->setCurrentIndex(index);
            dropdown_->view()->scrollTo(index);
            return;
        }
    }
}

bool KSearchPopup::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape || key->key() == Qt::Key_Return ||
            key->key() == Qt::Key_Enter || key->key() == Qt::Key_Up || key->key() == Qt::Key_Down) {
            event->accept();
            if (event->type() == QEvent::ShortcutOverride)
                return true;
            switch (key->key()) {
            case Qt::Key_Escape:
                close();
                break;
            case Qt::Key_Return:
            case Qt::Key_Enter:
                choose(dropdown_->view()->currentIndex().row());
                break;
            case Qt::Key_Up:
                step(-1);
                break;
            case Qt::Key_Down:
                step(1);
                break;
            default:
                break;
            }
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void KSearchPopup::mousePressEvent(QMouseEvent* event) {
    const auto surfacePoint = dropdown_->surface()->mapFrom(this, event->position().toPoint());
    if (!dropdown_->surface()->rect().contains(surfacePoint)) {
        close();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}
} // namespace bloom::ui::kit
