#include <bloom/ui/kit/search_popup.hpp>

#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/mnemonic_style.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <QSignalBlocker>
#include <QSize>
#include <QStandardItemModel>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <map>
#include <utility>
#include <vector>

namespace bloom::ui::kit {
namespace {

// The Add search's fixed width: wide enough for a node name and its section, so the popup does not
// change width as the filter narrows it.
[[nodiscard]] int searchFrameWidth() { return px(Size::ControlRoomy) * 16; }

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

KSearchPopup::KSearchPopup(QWidget* parent) : KPopupWindow(parent, Elevation::Popup) {
    ensureKeyboardFocusTracking(*this);
    setObjectName(QStringLiteral("kSearchPopup"));
    setAccessibleName(tr("Add node search"));
    list_ = new KPopupList(this);
    list_->setObjectName(QStringLiteral("kDropdownSurface"));
    // Headings are a different height from results, so the rows are not uniform.
    list_->setUniformRows(false);
    list_->setFilterVisible(true);
    auto* field = list_->filterField();
    field->setObjectName(QStringLiteral("kSearchFilter"));
    field->setAccessibleName(tr("Filter nodes by name or socket kind"));
    field->setPlaceholderText(tr("Search nodes…"));
    model_ = new QStandardItemModel(this);
    list_->setModel(model_);
    setContent(list_, field);
    connect(list_, &KPopupList::filterChanged, this, &KSearchPopup::filter);
    connect(list_, &KPopupList::rowChosen, this, &KSearchPopup::choose);
    connect(list_, &KPopupList::dismissed, this, &QWidget::close);
    connect(list_, &KPopupList::tabbed, this, &QWidget::close);
}

QSize KSearchPopup::wantedFrameSize() const {
    const int border = std::max(1, static_cast<int>(std::lround(kHairlineWidth)));
    return {searchFrameWidth() + 2 * border, list_->fittedHeight() + 2 * border};
}

void KSearchPopup::setEntries(std::vector<SearchEntry> entries) {
    entries_ = std::move(entries);
    const QSignalBlocker blocker(list_);
    list_->filterField()->clear();
    filter();
}

void KSearchPopup::filter() {
    model_->clear();
    const QString query = list_->filterField()->text().simplified();
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
    for (const auto& match : matches) {
        const auto& entry = *match.entry;
        // A heading is emitted only once the section has something to head, so a filter that
        // matches nothing in a section leaves no empty heading behind.
        if (!entry.section.isEmpty() && (!sectionOpen || entry.section != openSection)) {
            auto* heading = new QStandardItem(entry.section);
            heading->setData(true, kSearchSectionRole);
            // No flags at all: a heading is neither selectable nor choosable, which is also what
            // keeps keyboard navigation -- which steps over anything not enabled -- unchanged.
            heading->setFlags(Qt::NoItemFlags);
            model_->appendRow(heading);
            openSection = entry.section;
            sectionOpen = true;
        }
        // The label alone. A refused result's reason is its tooltip and nothing else.
        auto* item = new QStandardItem(entry.label);
        item->setData(entry.key, Qt::UserRole);
        item->setToolTip(entry.refusal);
        item->setEnabled(entry.refusal.isEmpty());
        model_->appendRow(item);
    }
    if (model_->rowCount() == 0) {
        auto* item = new QStandardItem(tr("No matching nodes"));
        item->setEnabled(false);
        model_->appendRow(item);
    }
    // The best match is current, so Enter adds what the artist was naming.
    list_->setCurrentRow(-1);
    list_->stepCurrent(1);
    if (isVisible())
        refit(wantedFrameSize());
}

void KSearchPopup::openAt(const QPoint globalPosition) {
    KPopupWindow::openAt(globalPosition, wantedFrameSize());
    list_->setFrameCorner(translucent()
                              ? static_cast<qreal>(radiusPx(Radius::Small, 0)) - kHairlineWidth
                              : 0.0);
    list_->scrollToCurrent();
}

void KSearchPopup::choose(const int row) {
    const auto index = model_->index(row, 0);
    if (!index.isValid() || !index.flags().testFlag(Qt::ItemIsEnabled))
        return;
    const QString key = index.data(Qt::UserRole).toString();
    close();
    Q_EMIT entryChosen(key);
}
} // namespace bloom::ui::kit
