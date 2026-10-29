#include <bloom/ui/kit/accessibility.hpp>

#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/radio_group.hpp>

#include <QAccessible>
#include <QAccessibleWidget>
#include <QHash>
#include <QPointer>

namespace bloom::ui::kit {
namespace {

// KDropdown as a combo box: its name, its value, whether its list is open, and Press/ShowMenu to
// open it.
class DropdownAccessible final : public QAccessibleWidget {
  public:
    explicit DropdownAccessible(KDropdown* dropdown)
        : QAccessibleWidget(dropdown, QAccessible::ComboBox) {
        addControllingSignal(QStringLiteral("currentIndexChanged(int)"));
    }

    [[nodiscard]] KDropdown* dropdown() const { return static_cast<KDropdown*>(widget()); }

    [[nodiscard]] QString text(const QAccessible::Text type) const override {
        if (type == QAccessible::Value)
            return dropdown()->currentText();
        if (type == QAccessible::Name) {
            const QString name = QAccessibleWidget::text(type);
            return name.isEmpty() ? dropdown()->currentText() : name;
        }
        return QAccessibleWidget::text(type);
    }

    [[nodiscard]] QAccessible::State state() const override {
        QAccessible::State value = QAccessibleWidget::state();
        value.expandable = true;
        value.expanded = dropdown()->isPopupVisible();
        value.collapsed = !value.expanded;
        value.hasPopup = true;
        return value;
    }

    [[nodiscard]] QStringList actionNames() const override {
        return {showMenuAction(), pressAction()};
    }

    void doAction(const QString& action) override {
        if (action == showMenuAction() || action == pressAction())
            dropdown()->showPopup();
        else
            QAccessibleWidget::doAction(action);
    }
};

// One option of a KRadioGroup. It has no QObject of its own; the group's interface owns it.
class RadioOptionAccessible final : public QAccessibleInterface {
  public:
    RadioOptionAccessible(KRadioGroup* group, const int index) : group_(group), index_(index) {}

    [[nodiscard]] bool isValid() const override {
        return group_ != nullptr && index_ < group_->count();
    }
    [[nodiscard]] QObject* object() const override { return nullptr; }
    [[nodiscard]] QWindow* window() const override {
        return group_ == nullptr ? nullptr : group_->window()->windowHandle();
    }
    [[nodiscard]] QAccessibleInterface* parent() const override {
        return QAccessible::queryAccessibleInterface(group_.data());
    }
    [[nodiscard]] QAccessibleInterface* child(int) const override { return nullptr; }
    [[nodiscard]] int childCount() const override { return 0; }
    [[nodiscard]] int indexOfChild(const QAccessibleInterface*) const override { return -1; }
    [[nodiscard]] QAccessibleInterface* childAt(int, int) const override { return nullptr; }
    [[nodiscard]] QAccessibleInterface* focusChild() const override { return nullptr; }

    [[nodiscard]] QString text(const QAccessible::Text type) const override {
        if (!isValid())
            return {};
        return type == QAccessible::Name ? group_->optionText(index_) : QString{};
    }
    void setText(QAccessible::Text, const QString&) override {}

    [[nodiscard]] QRect rect() const override {
        if (!isValid())
            return {};
        const QRect local = group_->optionRect(index_);
        return {group_->mapToGlobal(local.topLeft()), local.size()};
    }

    [[nodiscard]] QAccessible::Role role() const override { return QAccessible::RadioButton; }

    [[nodiscard]] QAccessible::State state() const override {
        QAccessible::State value;
        if (!isValid())
            return value;
        value.checkable = true;
        value.checked = group_->currentIndex() == index_;
        value.disabled = !group_->isEnabled() || !group_->isOptionEnabled(index_);
        value.focusable = !value.disabled;
        value.invisible = !group_->isVisible();
        return value;
    }

  private:
    QPointer<KRadioGroup> group_;
    int index_ = 0;
};

// KRadioGroup as a grouping of radio buttons, one child per option.
class RadioGroupAccessible final : public QAccessibleWidget {
  public:
    explicit RadioGroupAccessible(KRadioGroup* group)
        : QAccessibleWidget(group, QAccessible::Grouping) {}

    ~RadioGroupAccessible() override {
        for (const auto id : std::as_const(children_))
            QAccessible::deleteAccessibleInterface(id);
    }

    [[nodiscard]] KRadioGroup* group() const { return static_cast<KRadioGroup*>(widget()); }

    [[nodiscard]] int childCount() const override { return group()->count(); }

    [[nodiscard]] QAccessibleInterface* child(const int index) const override {
        if (index < 0 || index >= group()->count())
            return nullptr;
        if (const auto found = children_.constFind(index); found != children_.constEnd())
            return QAccessible::accessibleInterface(*found);
        auto* option = new RadioOptionAccessible(group(), index);
        children_.insert(index, QAccessible::registerAccessibleInterface(option));
        return option;
    }

    [[nodiscard]] int indexOfChild(const QAccessibleInterface* child) const override {
        for (auto it = children_.constBegin(); it != children_.constEnd(); ++it)
            if (QAccessible::accessibleInterface(it.value()) == child)
                return it.key();
        return -1;
    }

    [[nodiscard]] QAccessibleInterface* childAt(const int x, const int y) const override {
        for (int index = 0; index < group()->count(); ++index) {
            const QRect local = group()->optionRect(index);
            if (QRect(group()->mapToGlobal(local.topLeft()), local.size()).contains(x, y))
                return child(index);
        }
        return nullptr;
    }

  private:
    mutable QHash<int, QAccessible::Id> children_;
};

} // namespace

QAccessibleInterface* kitAccessibleFactory(const QString& className, QObject* object) {
    if (object == nullptr || !object->isWidgetType())
        return nullptr;
    if (className == QLatin1String(KDropdown::staticMetaObject.className()))
        return new DropdownAccessible(static_cast<KDropdown*>(object));
    if (className == QLatin1String(KRadioGroup::staticMetaObject.className()))
        return new RadioGroupAccessible(static_cast<KRadioGroup*>(object));
    return nullptr;
}

void installKitAccessibility() {
    static const bool installed = [] {
        QAccessible::installFactory(&kitAccessibleFactory);
        return true;
    }();
    Q_UNUSED(installed)
}

} // namespace bloom::ui::kit
