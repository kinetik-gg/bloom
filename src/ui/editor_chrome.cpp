#include <QHBoxLayout>
#include <QPalette>
#include <QResizeEvent>
#include <QVBoxLayout>
#include <algorithm>
#include <bloom/ui/editor_area.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <numeric>
namespace bloom::ui {
void EditorChromeRowSpec::addWidget(QWidget* control) {
    entries.push_back({control, false, true, trailing});
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
    entries.push_back({button, true, visible, trailing});
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
class ChromeRow final : public QWidget {
  public:
    ChromeRow(const EditorChromeRowSpec& spec, QWidget* parent, bool footer)
        : QWidget(parent), entries_(spec.entries) {
        setObjectName(spec.objectName);
        setFixedHeight(kit::px(footer ? kit::Size::FooterRow : kit::Size::Control));
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setAutoFillBackground(true);
        auto colors = palette();
        colors.setColor(QPalette::Window, kit::color(kit::Color::Surface));
        setPalette(colors);
        for (auto& entry : entries_) {
            entry.control->setParent(this);
            if (auto* dropdown = qobject_cast<kit::KDropdown*>(entry.control))
                dropdown->setControlSize(kit::KDropdown::ControlSize::Default);
            entry.control->setFixedHeight(kit::px(kit::Size::Control));
            entry.control->setProperty("chromeControl", true);
            entry.control->setVisible(entry.visible);
        }
        if (std::ranges::any_of(entries_, [](const auto& entry) { return entry.menu; })) {
            overflow_ = new kit::KMenuButton(this);
            overflow_->setObjectName(spec.overflowButtonName);
            overflow_->setText(QStringLiteral("…"));
            overflow_->setAccessibleName(tr("More panel menus"));
            auto* menu = kit::makeMenu(this);
            menu->setObjectName(spec.overflowMenuName);
            for (const auto& entry : entries_)
                if (entry.menu) {
                    auto* button = qobject_cast<QToolButton*>(entry.control);
                    menu->addMenu(button->menu());
                }
            overflow_->setMenu(menu);
            overflow_->hide();
        }
        arrange();
    }
    QSize minimumSizeHint() const override { return {0, height()}; }
    QSize sizeHint() const override { return {measure(), height()}; }

  protected:
    void resizeEvent(QResizeEvent*) override { arrange(); }
    void showEvent(QShowEvent*) override { arrange(); }

  private:
    int preferred(QWidget* control) const {
        return std::clamp(control->sizeHint().width(), control->minimumWidth(),
                          control->maximumWidth());
    }
    int measure() const {
        int needed = 0;
        for (const auto& entry : entries_)
            if (entry.visible)
                needed += preferred(entry.control) + kit::px(kit::Spacing::XXS);
        return needed;
    }
    void arrange() {
        if (arranging_)
            return;
        arranging_ = true;
        const int needed = measure();
        const int hysteresis = collapsed_ ? kit::px(kit::Spacing::XS) : 0;
        collapsed_ = overflow_ && needed + hysteresis > width();
        setProperty("collapseThreshold", needed);
        setProperty("collapsed", collapsed_);
        QList<QWidget*> visible;
        for (auto& entry : entries_) {
            const bool show = entry.visible && !(collapsed_ && entry.menu);
            entry.control->setVisible(show);
            if (show)
                visible.push_back(entry.control);
        }
        if (overflow_) {
            overflow_->setVisible(collapsed_);
            if (collapsed_)
                visible.push_back(overflow_);
        }
        int remaining = width() - std::max(0, static_cast<int>(visible.size()) - 1) *
                                      kit::px(kit::Spacing::XXS);
        int total = 0;
        for (auto* control : visible)
            total += preferred(control);
        int x = 0;
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
            if (total > remaining && !qobject_cast<QToolButton*>(control)) {
                extent = std::max(control->minimumWidth(), preferredWidth - (total - remaining));
            }
            total -= preferredWidth;
            extent = std::min(extent, std::max(0, remaining));
            const bool fits = extent > 0;
            control->setVisible(fits);
            if (fits)
                control->setGeometry(x, std::midpoint(0, height() - kit::px(kit::Size::Control)),
                                     extent, kit::px(kit::Size::Control));
            x += extent + kit::px(kit::Spacing::XXS);
            remaining -= extent;
        }
        arranging_ = false;
    }
    std::vector<EditorChromeRowSpec::Entry> entries_;
    kit::KMenuButton* overflow_ = nullptr;
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
    leading->setFixedWidth(split);
    auto* leadingLayout = new QHBoxLayout(leading);
    leadingLayout->setContentsMargins(0, 0, 0, 0);
    leadingLayout->addWidget(left);
    layout->addWidget(leading);
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
