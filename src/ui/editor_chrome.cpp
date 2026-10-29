#include <QDynamicPropertyChangeEvent>
#include <QHBoxLayout>
#include <QMenu>
#include <QPalette>
#include <QResizeEvent>
#include <QVBoxLayout>
#include <algorithm>
#include <bloom/ui/editor_area.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/icons.hpp>
#include <numeric>
#include <set>
namespace bloom::ui {
namespace {
EditorChromeRowSpec::Entry makeEntry(QWidget* control, const bool trailing,
                                     const EditorChromeGroup group) {
    EditorChromeRowSpec::Entry entry;
    entry.control = control;
    entry.trailing = trailing;
    entry.group = group;
    return entry;
}
} // namespace
void EditorChromeRowSpec::addWidget(QWidget* control) {
    entries.push_back(makeEntry(control, trailing, group));
}
void EditorChromeRowSpec::addExpandingWidget(QWidget* control) {
    auto entry = makeEntry(control, trailing, group);
    entry.expanding = true;
    entries.push_back(std::move(entry));
}
EditorChromeRowSpec::Entry* EditorChromeRowSpec::find(QWidget* control) {
    const auto entry = std::ranges::find(entries, control, &Entry::control);
    return entry == entries.end() ? nullptr : &*entry;
}
void EditorChromeRowSpec::setCollapse(QWidget* control, const int rank, QAction* overflow) {
    if (auto* entry = find(control)) {
        entry->collapseRank = rank;
        entry->overflow = overflow;
    }
}
void EditorChromeRowSpec::setCompactForm(QWidget* control, const int rank, const int width,
                                         std::function<void(bool)> compact) {
    if (auto* entry = find(control)) {
        entry->compactRank = rank;
        entry->compactWidth = width;
        entry->setCompact = std::move(compact);
    }
}
void EditorChromeRowSpec::setElideFloor(QWidget* control, const int floor) {
    if (auto* entry = find(control))
        entry->elideFloor = floor;
}
QToolButton* EditorChromeRowSpec::addMenuButton(const QString& title, QMenu* menu,
                                                const QString& name, bool visible) {
    auto* button = new kit::KMenuButton(owner);
    button->setText(title);
    button->setMenu(menu);
    button->setObjectName(name);
    button->setAccessibleName(title);
    button->setProperty("headerMenuButton", true);
    button->setProperty("headerMenuVisible", visible);
    auto entry = makeEntry(button, trailing, group);
    entry.menu = true;
    entry.visible = visible;
    entries.push_back(std::move(entry));
    return button;
}
QToolButton* EditorChromeRowSpec::addMenu(QMenu* menu, const QString& name, bool visible) {
    return addMenuButton(menu->title(), menu, name, visible);
}
QMenu* EditorChromeRowSpec::addMenu(const QString& title) {
    auto* menu = kit::makeMenu(title, owner);
    addMenuButton(title, menu);
    return menu;
}
namespace {
using Group = EditorChromeGroup;
class ChromeRow final : public QWidget {
  public:
    ChromeRow(const EditorChromeRowSpec& spec, QWidget* parent, bool footer)
        : QWidget(parent), entries_(spec.entries),
          padding_(footer ? kit::px(kit::Spacing::ChromePadding) : 0) {
        setObjectName(spec.objectName);
        setFixedHeight(kit::px(footer ? kit::Size::FooterRow : kit::Size::Control));
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setAutoFillBackground(true);
        auto colors = palette();
        colors.setColor(QPalette::Window, kit::color(kit::Color::Surface));
        setPalette(colors);
        // A priority row (groups, ranks, compact forms) has its own layout; a plain row keeps the
        // measure-only menu collapse every other panel uses.
        prioritized_ = std::ranges::any_of(entries_, [](const auto& entry) {
            return entry.group == Group::Centre || entry.collapseRank > 0 || entry.compactRank > 0;
        });
        for (auto& entry : entries_) {
            entry.control->setParent(this);
            entry.control->installEventFilter(this);
            if (entry.menu) {
                if (auto* button = qobject_cast<QToolButton*>(entry.control);
                    button && button->menu())
                    button->menu()->setParent(this, button->menu()->windowFlags());
            }
            if (auto* dropdown = qobject_cast<kit::KDropdown*>(entry.control)) {
                dropdown->setControlSize(kit::KDropdown::ControlSize::Default);
                // A priority row's dropdowns keep the width their panel gave them; an elidable
                // one may shrink to its floor. Any other dropdown is never narrower than its
                // widest item.
                if (entry.elideFloor > 0) {
                    dropdown->setMinimumWidth(entry.elideFloor);
                } else if (!prioritized_) {
                    const auto minimum = dropdown->minimumSizeHint().width();
                    dropdown->setMaximumWidth(std::max(minimum, dropdown->maximumWidth()));
                    dropdown->setMinimumWidth(minimum);
                }
            }
            entry.control->setFixedHeight(kit::px(qobject_cast<kit::KIconToggle*>(entry.control)
                                                      ? kit::Size::ToggleCell
                                                      : kit::Size::Control));
            entry.control->setProperty("chromeControl", true);
            entry.control->setVisible(entry.visible);
            if (entry.collapseRank > 0)
                ranks_.insert(entry.collapseRank);
            if (entry.compactRank > 0)
                ranks_.insert(entry.compactRank);
        }
        compact_.assign(entries_.size(), false);
        fullExtents_.assign(entries_.size(), {0, 0});
        collapsedEntries_.assign(entries_.size(), false);
        if (prioritized_ && std::ranges::any_of(entries_, [](const auto& entry) {
                return entry.collapseRank > 0;
            })) {
            auto* button = new kit::KIconButton(this);
            button->setObjectName(spec.overflowButtonName);
            if (spec.overflowIcon)
                button->setIcon(kit::icon(*spec.overflowIcon, kit::IconRole::Control));
            const int box = kit::px(kit::iconSize(kit::IconRole::Control));
            button->setIconSize(QSize(box, box));
            button->setToolTip(spec.overflowToolTip);
            button->setAccessibleName(spec.overflowToolTip);
            button->setPopupMode(QToolButton::InstantPopup);
            button->setProperty("chromeControl", true);
            auto* menu = kit::makeMenu(this);
            menu->setObjectName(spec.overflowMenuName);
            button->setMenu(menu);
            button->hide();
            overflow_ = button;
        } else if (!prioritized_ &&
                   std::ranges::any_of(entries_, [](const auto& entry) { return entry.menu; })) {
            auto* button = new kit::KMenuButton(this);
            button->setObjectName(spec.overflowButtonName);
            button->setText(QStringLiteral("…"));
            button->setAccessibleName(tr("More panel menus"));
            auto* menu = kit::makeMenu(this);
            menu->setObjectName(spec.overflowMenuName);
            for (const auto& entry : entries_)
                if (entry.menu) {
                    auto* menuButton = qobject_cast<QToolButton*>(entry.control);
                    menu->addMenu(menuButton->menu());
                }
            button->setMenu(menu);
            button->hide();
            overflow_ = button;
        }
        arrange();
    }
    QSize minimumSizeHint() const override { return {0, height()}; }
    QSize sizeHint() const override { return {prioritized_ ? neededAt(0) : measure(), height()}; }

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() == QEvent::DynamicPropertyChange &&
            static_cast<QDynamicPropertyChangeEvent*>(event)->propertyName() == "chromeSuppressed")
            arrange();
        return QWidget::eventFilter(watched, event);
    }
    // A control whose width changed (new items, a longer label) asks its parent to lay out again.
    bool event(QEvent* event) override {
        if (event->type() == QEvent::LayoutRequest)
            arrange();
        return QWidget::event(event);
    }
    void resizeEvent(QResizeEvent*) override { arrange(); }
    void showEvent(QShowEvent*) override { arrange(); }

  private:
    [[nodiscard]] static bool shown(const EditorChromeRowSpec::Entry& entry) {
        return entry.visible && !entry.control->property("chromeSuppressed").toBool();
    }
    int preferred(QWidget* control) const {
        return std::clamp(control->sizeHint().width(), control->minimumWidth(),
                          std::max(control->minimumWidth(), control->maximumWidth()));
    }
    // The narrowest an entry asks for before the menus collapse: its elide floor, if it has one.
    int floorWidth(const EditorChromeRowSpec::Entry& entry) const {
        return entry.elideFloor > 0 ? std::min(entry.elideFloor, preferred(entry.control))
                                    : preferred(entry.control);
    }
    int measure() const {
        int needed = 2 * padding_;
        for (const auto& entry : entries_)
            if (shown(entry))
                needed += floorWidth(entry) + kit::px(kit::Spacing::ChromeGap);
        return std::max(2 * padding_, needed - kit::px(kit::Spacing::ChromeGap));
    }
    void arrange() {
        if (arranging_)
            return;
        arranging_ = true;
        if (prioritized_)
            arrangePrioritized();
        else
            arrangeMenuCollapse();
        arranging_ = false;
    }

    // ---- Priority rows -------------------------------------------------------------------------
    // Level 0 shows everything at full size; level n applies every compact form and collapse whose
    // rank is at most the n-th smallest rank in the row.
    [[nodiscard]] int rankAt(const int level) const {
        if (level <= 0)
            return 0;
        auto rank = ranks_.begin();
        std::advance(rank, std::min<int>(level, static_cast<int>(ranks_.size())) - 1);
        return *rank;
    }
    [[nodiscard]] bool collapsesAt(const EditorChromeRowSpec::Entry& entry, const int level) const {
        return entry.collapseRank > 0 && entry.collapseRank <= rankAt(level);
    }
    [[nodiscard]] bool compactAt(const EditorChromeRowSpec::Entry& entry, const int level) const {
        return entry.compactRank > 0 && entry.compactRank <= rankAt(level) &&
               !collapsesAt(entry, level);
    }
    [[nodiscard]] int widthAt(const EditorChromeRowSpec::Entry& entry, const int level) const {
        if (compactAt(entry, level) && entry.compactWidth > 0)
            return entry.compactWidth;
        // A control in its compact form is measured at the width it had before it.
        const auto index = static_cast<std::size_t>(&entry - entries_.data());
        if (compact_[index]) {
            const auto& [minimum, maximum] = fullExtents_[index];
            return std::clamp(entry.control->sizeHint().width(), minimum,
                              std::max(minimum, maximum));
        }
        return preferred(entry.control);
    }
    // The three groups' widths at `level`, the leading one including the overflow button when
    // anything shown has collapsed into it.
    [[nodiscard]] std::array<int, 3> groupWidths(const int level) const {
        std::array<int, 3> widths{};
        std::array<int, 3> counts{};
        bool overflowing = false;
        for (const auto& entry : entries_) {
            if (!shown(entry))
                continue;
            if (collapsesAt(entry, level)) {
                overflowing = overflowing || entry.overflow != nullptr;
                continue;
            }
            const auto group = static_cast<std::size_t>(entry.group);
            widths[group] += widthAt(entry, level);
            ++counts[group];
        }
        if (overflowing && overflow_ != nullptr) {
            widths[0] += preferred(overflow_);
            ++counts[0];
        }
        for (std::size_t group = 0; group < widths.size(); ++group)
            widths[group] += std::max(0, counts[group] - 1) * kit::px(kit::Spacing::ChromeGap);
        return widths;
    }
    [[nodiscard]] int neededAt(const int level) const {
        const auto widths = groupWidths(level);
        const auto groups = std::ranges::count_if(widths, [](const int width) { return width > 0; });
        return 2 * padding_ + std::accumulate(widths.begin(), widths.end(), 0) +
               static_cast<int>(std::max<std::ptrdiff_t>(0, groups - 1)) *
                   kit::px(kit::Spacing::ChromeGroupGap);
    }
    void arrangePrioritized() {
        const int levels = static_cast<int>(ranks_.size());
        // The first level that fits; returning to a level below the current one needs a little
        // more room than leaving it did, so a width on the boundary never flickers.
        int chosen = levels;
        for (int level = 0; level <= levels; ++level) {
            const int hysteresis = level < level_ ? kit::px(kit::Spacing::XS) : 0;
            if (neededAt(level) + hysteresis <= width()) {
                chosen = level;
                break;
            }
        }
        level_ = chosen;
        setProperty("collapseThreshold", neededAt(0));
        setProperty("collapsed", level_ > 0);
        setProperty("collapseLevel", level_);

        // Compact forms and the overflow menu follow the level.
        bool overflowChanged = false;
        bool overflowing = false;
        for (std::size_t index = 0; index < entries_.size(); ++index) {
            auto& entry = entries_[index];
            const bool compact = compactAt(entry, level_);
            if (compact != static_cast<bool>(compact_[index])) {
                // The row owns the compact width: it narrows the control and later restores the
                // extents the control had, so a fixed-width control can take its compact form.
                if (compact) {
                    fullExtents_[index] = {entry.control->minimumWidth(),
                                           entry.control->maximumWidth()};
                    if (entry.compactWidth > 0)
                        entry.control->setFixedWidth(entry.compactWidth);
                } else {
                    entry.control->setMinimumWidth(fullExtents_[index].first);
                    entry.control->setMaximumWidth(fullExtents_[index].second);
                }
                compact_[index] = compact;
                if (entry.setCompact)
                    entry.setCompact(compact);
            }
            const bool collapsed = collapsesAt(entry, level_) && shown(entry);
            overflowChanged = overflowChanged || collapsed != collapsedEntries_[index];
            collapsedEntries_[index] = collapsed;
            overflowing = overflowing || (collapsed && entry.overflow != nullptr);
        }
        if (overflow_ != nullptr && overflowChanged) {
            auto* menu = overflow_->menu();
            menu->clear();
            std::set<QAction*> offered;
            for (std::size_t index = 0; index < entries_.size(); ++index) {
                auto* action = entries_[index].overflow;
                if (collapsedEntries_[index] && action != nullptr && offered.insert(action).second)
                    menu->addAction(action);
            }
        }

        // Leading from the start, Trailing against the end, Centre centred in the row and kept
        // clear of both by the group gap.
        const auto widths = groupWidths(level_);
        const int gap = kit::px(kit::Spacing::ChromeGap);
        const int groupGap = kit::px(kit::Spacing::ChromeGroupGap);
        const int leadingEnd = padding_ + widths[0];
        const int trailingStart = width() - padding_ - widths[2];
        int centre = (width() - widths[1]) / 2;
        const int lowest = widths[0] > 0 ? leadingEnd + groupGap : padding_;
        const int highest =
            (widths[2] > 0 ? trailingStart - groupGap : width() - padding_) - widths[1];
        centre = std::max(lowest, std::min(centre, highest));
        std::array<int, 3> cursor{padding_, centre, trailingStart};
        const auto place = [&](QWidget* control, const int extent, const std::size_t group) {
            const int x = cursor[group];
            cursor[group] += extent + gap;
            // The last resort below the narrowest level: a control that does not fit entirely is
            // hidden rather than clipped.
            const bool fits = x >= 0 && x + extent <= width() - padding_;
            control->setVisible(fits);
            if (!fits)
                return;
            const int height = std::min(kit::px(kit::Size::Control), control->maximumHeight());
            control->setGeometry(x, std::midpoint(0, this->height() - height), extent, height);
        };
        bool overflowPlaced = false;
        for (std::size_t index = 0; index < entries_.size(); ++index) {
            auto& entry = entries_[index];
            if (!shown(entry) || collapsesAt(entry, level_)) {
                entry.control->setVisible(false);
                continue;
            }
            const auto group = static_cast<std::size_t>(entry.group);
            // The overflow button closes the leading group, where the collapsed controls were.
            if (group != 0 && overflowing && !overflowPlaced && overflow_ != nullptr) {
                place(overflow_, preferred(overflow_), 0);
                overflowPlaced = true;
            }
            place(entry.control, widthAt(entry, level_), group);
        }
        if (overflow_ != nullptr) {
            if (overflowing && !overflowPlaced)
                place(overflow_, preferred(overflow_), 0);
            else if (!overflowing)
                overflow_->hide();
        }
    }

    // ---- Menu-collapse rows --------------------------------------------------------------------
    void arrangeMenuCollapse() {
        const int needed = measure();
        const int hysteresis = collapsed_ ? kit::px(kit::Spacing::XS) : 0;
        collapsed_ = overflow_ && needed + hysteresis > width();
        setProperty("collapseThreshold", needed);
        setProperty("collapsed", collapsed_);
        QList<QWidget*> visible;
        for (auto& entry : entries_) {
            const bool show = shown(entry) && !(collapsed_ && entry.menu);
            entry.control->setVisible(show);
            if (show)
                visible.push_back(entry.control);
        }
        if (overflow_) {
            overflow_->setVisible(collapsed_);
            if (collapsed_)
                visible.push_back(overflow_);
        }
        int remaining =
            width() - padding_ - padding_ -
            std::max(0, static_cast<int>(visible.size()) - 1) * kit::px(kit::Spacing::ChromeGap);
        int total = 0;
        int expandingCount = 0;
        for (auto* control : visible)
            total += preferred(control);
        for (const auto& entry : entries_)
            if (entry.expanding && visible.contains(entry.control))
                ++expandingCount;
        int x = padding_;
        bool trailingPlaced = false;
        for (auto* control : visible) {
            const bool trailing = std::ranges::any_of(entries_, [control](const auto& entry) {
                return entry.control == control && entry.trailing;
            });
            if (trailing && !trailingPlaced && remaining > total) {
                x += remaining - total;
                remaining = total;
                trailingPlaced = true;
            }
            const int preferredWidth = preferred(control);
            int extent = preferredWidth;
            const bool expanding = std::ranges::any_of(entries_, [control](const auto& entry) {
                return entry.control == control && entry.expanding;
            });
            if (expanding && expandingCount > 0) {
                extent += std::max(0, remaining - total) / expandingCount;
                --expandingCount;
            }
            if (total > remaining && !qobject_cast<QToolButton*>(control)) {
                extent = std::max(control->minimumWidth(), preferredWidth - (total - remaining));
            }
            total -= preferredWidth;
            const int reserved = collapsed_ && control != overflow_ ? preferred(overflow_) : 0;
            extent = std::min(extent, std::max(0, remaining - reserved));
            const bool fits = extent > 0 && extent >= control->minimumWidth();
            if (!fits)
                extent = 0;
            control->setVisible(fits);
            if (fits)
                control->setGeometry(x, std::midpoint(0, height() - kit::px(kit::Size::Control)),
                                     extent, kit::px(kit::Size::Control));
            x += extent + kit::px(kit::Spacing::ChromeGap);
            remaining -= extent;
        }
    }
    std::vector<EditorChromeRowSpec::Entry> entries_;
    QToolButton* overflow_ = nullptr;
    std::set<int> ranks_;
    std::vector<char> compact_;
    std::vector<std::pair<int, int>> fullExtents_;
    std::vector<char> collapsedEntries_;
    int padding_ = 0;
    int level_ = 0;
    bool prioritized_ = false;
    bool collapsed_ = false;
    bool arranging_ = false;
};
} // namespace
QWidget* EditorArea::buildCanvasChrome(const EditorCanvasChromeSpec& spec, QWidget* parent) {
    auto* row = new QWidget(parent);
    row->setObjectName(spec.objectName);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    if (spec.leadingWidth) {
        auto* leading = new QWidget(row);
        leading->setObjectName(spec.leadingName);
        leading->setFixedWidth(spec.leadingWidth);
        layout->addWidget(leading);
    }
    auto* column = new QWidget(row);
    auto* stack = new QVBoxLayout(column);
    stack->setContentsMargins(0, 0, 0, 0);
    stack->setSpacing(0);
    if (spec.strip)
        stack->addWidget(spec.strip);
    stack->addWidget(spec.canvas, 1);
    layout->addWidget(column, 1);
    auto* gutter = new QWidget(row);
    gutter->setObjectName(spec.gutterName);
    gutter->setFixedWidth(spec.gutterWidth);
    layout->addWidget(gutter);
    row->setFixedHeight(kit::px(kit::Size::HeaderRow));
    return row;
}
QWidget* EditorArea::buildSplitChrome(QWidget* left, QWidget* right, int split, QWidget* parent) {
    auto* row = new QWidget(parent);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto* leading = new QWidget(row);
    // task TL-FIX2: named so a live split (the timeline's draggable layer-table/lanes divider) can
    // find and resize this cell without a full rebuild -- see
    // TimelineEditor::setLayerColumnWidth().
    leading->setObjectName(QStringLiteral("timelineHeaderFallbackSplit"));
    leading->setFixedWidth(split);
    auto* leadingLayout = new QHBoxLayout(leading);
    leadingLayout->setContentsMargins(0, 0, 0, 0);
    leadingLayout->addWidget(left);
    layout->addWidget(leading);
    // task TL-FIX2: SplitHandle, matching the timeline body's own widened divider so the ruler
    // this fallback header carries never drifts out of alignment with the lanes below it.
    layout->addSpacing(kit::px(kit::Size::SplitHandle));
    layout->addWidget(right, 1);
    return row;
}
QWidget* EditorArea::buildChromeRow(EditorChromeRowSpec& spec, QWidget* parent, bool footer) {
    if (spec.entries.empty())
        return nullptr;
    if (spec.host) {
        spec.host->setParent(parent);
        return spec.host;
    }
    spec.host = new ChromeRow(spec, parent, footer);
    return spec.host;
}
} // namespace bloom::ui
