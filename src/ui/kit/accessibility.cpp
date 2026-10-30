#include <bloom/ui/kit/accessibility.hpp>

#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/progress_bar.hpp>
#include <bloom/ui/kit/radio_group.hpp>
#include <bloom/ui/kit/range_selector.hpp>
#include <bloom/ui/kit/slider.hpp>
#include <bloom/ui/kit/value_field.hpp>

#include <bloom/ui/kit/tab_bar.hpp>

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

    // The value is the selected option's label; a group reflecting objects that disagree reads
    // "Mixed", which no option claims.
    [[nodiscard]] QString text(const QAccessible::Text type) const override {
        if (type == QAccessible::Value) {
            if (group()->currentIndex() < 0 || group()->currentIndex() >= group()->count())
                return group()->count() > 0 ? QObject::tr("Mixed") : QString{};
            return group()->optionText(group()->currentIndex());
        }
        return QAccessibleWidget::text(type);
    }

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

// KProgressBar as a progress bar: a determinate bar reports completed over total through the value
// interface; a busy bar reports the busy state and no number.
class ProgressBarAccessible final : public QAccessibleWidget, public QAccessibleValueInterface {
  public:
    explicit ProgressBarAccessible(KProgressBar* bar)
        : QAccessibleWidget(bar, QAccessible::ProgressBar) {}

    [[nodiscard]] KProgressBar* bar() const { return static_cast<KProgressBar*>(widget()); }

    void* interface_cast(const QAccessible::InterfaceType type) override {
        if (type == QAccessible::ValueInterface)
            return static_cast<QAccessibleValueInterface*>(this);
        return QAccessibleWidget::interface_cast(type);
    }

    [[nodiscard]] QAccessible::State state() const override {
        QAccessible::State value = QAccessibleWidget::state();
        value.busy = bar()->busy();
        return value;
    }

    [[nodiscard]] QString text(const QAccessible::Text type) const override {
        if (type == QAccessible::Value)
            return bar()->busy() ? QObject::tr("Busy")
                                 : QObject::tr("%1%").arg(qRound(bar()->fraction() * 100.0));
        return QAccessibleWidget::text(type);
    }

    [[nodiscard]] QVariant currentValue() const override {
        return bar()->busy() ? QVariant{} : QVariant::fromValue(bar()->completed());
    }
    void setCurrentValue(const QVariant&) override {}
    [[nodiscard]] QVariant maximumValue() const override {
        return bar()->busy() ? QVariant{} : QVariant::fromValue(bar()->total());
    }
    [[nodiscard]] QVariant minimumValue() const override {
        return QVariant::fromValue(std::uint64_t{0});
    }
    [[nodiscard]] QVariant minimumStepSize() const override {
        return QVariant::fromValue(std::uint64_t{1});
    }
};

// One tab of a KTabBar, a page tab; selected while current.
class TabAccessible final : public QAccessibleInterface {
  public:
    TabAccessible(KTabBar* bar, const int index) : bar_(bar), index_(index) {}
    [[nodiscard]] bool isValid() const override {
        return bar_ != nullptr && index_ < bar_->count();
    }
    [[nodiscard]] QObject* object() const override { return nullptr; }
    [[nodiscard]] QWindow* window() const override {
        return bar_ == nullptr ? nullptr : bar_->window()->windowHandle();
    }
    [[nodiscard]] QAccessibleInterface* parent() const override {
        return QAccessible::queryAccessibleInterface(bar_.data());
    }
    [[nodiscard]] QAccessibleInterface* child(int) const override { return nullptr; }
    [[nodiscard]] int childCount() const override { return 0; }
    [[nodiscard]] int indexOfChild(const QAccessibleInterface*) const override { return -1; }
    [[nodiscard]] QAccessibleInterface* childAt(int, int) const override { return nullptr; }
    [[nodiscard]] QAccessibleInterface* focusChild() const override { return nullptr; }
    [[nodiscard]] QString text(const QAccessible::Text type) const override {
        return isValid() && type == QAccessible::Name ? bar_->tabText(index_) : QString{};
    }
    void setText(QAccessible::Text, const QString&) override {}
    [[nodiscard]] QRect rect() const override {
        if (!isValid())
            return {};
        const QRect local = bar_->tabRect(index_);
        return {bar_->mapToGlobal(local.topLeft()), local.size()};
    }
    [[nodiscard]] QAccessible::Role role() const override { return QAccessible::PageTab; }
    [[nodiscard]] QAccessible::State state() const override {
        QAccessible::State value;
        if (!isValid())
            return value;
        value.selectable = true;
        value.selected = bar_->currentIndex() == index_;
        value.disabled = !bar_->isEnabled();
        value.invisible = !bar_->isVisible();
        return value;
    }

  private:
    QPointer<KTabBar> bar_;
    int index_ = 0;
};

// KTabBar as a page tab list, one child per tab.
class TabBarAccessible final : public QAccessibleWidget {
  public:
    explicit TabBarAccessible(KTabBar* bar) : QAccessibleWidget(bar, QAccessible::PageTabList) {}
    ~TabBarAccessible() override {
        for (const auto id : std::as_const(children_))
            QAccessible::deleteAccessibleInterface(id);
    }
    [[nodiscard]] KTabBar* bar() const { return static_cast<KTabBar*>(widget()); }
    [[nodiscard]] int childCount() const override { return bar()->count(); }
    [[nodiscard]] QAccessibleInterface* child(const int index) const override {
        if (index < 0 || index >= bar()->count())
            return nullptr;
        if (const auto found = children_.constFind(index); found != children_.constEnd())
            return QAccessible::accessibleInterface(*found);
        auto* tab = new TabAccessible(bar(), index);
        children_.insert(index, QAccessible::registerAccessibleInterface(tab));
        return tab;
    }
    [[nodiscard]] int indexOfChild(const QAccessibleInterface* child) const override {
        for (auto it = children_.constBegin(); it != children_.constEnd(); ++it)
            if (QAccessible::accessibleInterface(it.value()) == child)
                return it.key();
        return -1;
    }

  private:
    mutable QHash<int, QAccessible::Id> children_;
};

// KValueField as a spin box: its name, its value with the unit, and the value interface (current,
// minimum, maximum, step). Setting the value through assistive technology is one complete edit.
class ValueFieldAccessible final : public QAccessibleWidget, public QAccessibleValueInterface {
  public:
    explicit ValueFieldAccessible(KValueField* field)
        : QAccessibleWidget(field, QAccessible::SpinBox) {}

    [[nodiscard]] KValueField* field() const { return static_cast<KValueField*>(widget()); }

    void* interface_cast(const QAccessible::InterfaceType type) override {
        if (type == QAccessible::ValueInterface)
            return static_cast<QAccessibleValueInterface*>(this);
        return QAccessibleWidget::interface_cast(type);
    }

    [[nodiscard]] QString text(const QAccessible::Text type) const override {
        if (type == QAccessible::Value) {
            const auto unit = field()->unit();
            const auto value = field()->displayedValue();
            return unit.isEmpty() || field()->isMixed() ? value : value + QLatin1Char(' ') + unit;
        }
        if (type == QAccessible::Name) {
            const QString name = QAccessibleWidget::text(type);
            return name.isEmpty() ? field()->label() : name;
        }
        return QAccessibleWidget::text(type);
    }

    [[nodiscard]] QVariant currentValue() const override {
        if (field()->isIntegerMode())
            return QVariant::fromValue(static_cast<qlonglong>(field()->integerValue()));
        return field()->value();
    }
    void setCurrentValue(const QVariant& value) override {
        bool ok = false;
        const double number = value.toDouble(&ok);
        if (!ok || field()->hasLiveEdit())
            return;
        Q_EMIT field()->editStarted();
        field()->setValue(number);
        Q_EMIT field()->editFinished();
    }
    [[nodiscard]] QVariant maximumValue() const override { return field()->maximum(); }
    [[nodiscard]] QVariant minimumValue() const override { return field()->minimum(); }
    [[nodiscard]] QVariant minimumStepSize() const override { return field()->singleStep(); }
};

// KSlider as a slider with the value interface.
class SliderAccessible final : public QAccessibleWidget, public QAccessibleValueInterface {
  public:
    explicit SliderAccessible(KSlider* slider) : QAccessibleWidget(slider, QAccessible::Slider) {}

    [[nodiscard]] KSlider* slider() const { return static_cast<KSlider*>(widget()); }

    void* interface_cast(const QAccessible::InterfaceType type) override {
        if (type == QAccessible::ValueInterface)
            return static_cast<QAccessibleValueInterface*>(this);
        return QAccessibleWidget::interface_cast(type);
    }

    [[nodiscard]] QString text(const QAccessible::Text type) const override {
        if (type == QAccessible::Value)
            return QString::number(slider()->value());
        return QAccessibleWidget::text(type);
    }

    [[nodiscard]] QVariant currentValue() const override { return slider()->value(); }
    void setCurrentValue(const QVariant& value) override {
        bool ok = false;
        const double number = value.toDouble(&ok);
        if (!ok || slider()->hasLiveEdit())
            return;
        Q_EMIT slider()->editStarted();
        slider()->setValue(number);
        Q_EMIT slider()->editFinished();
    }
    [[nodiscard]] QVariant maximumValue() const override { return slider()->maximum(); }
    [[nodiscard]] QVariant minimumValue() const override { return slider()->minimum(); }
    [[nodiscard]] QVariant minimumStepSize() const override {
        return (slider()->maximum() - slider()->minimum()) * kKeyStepFraction;
    }
};

// KRangeSelector as a slider whose value is the keyboard's active handle; its value text names
// both ends.
class RangeSelectorAccessible final : public QAccessibleWidget, public QAccessibleValueInterface {
  public:
    explicit RangeSelectorAccessible(KRangeSelector* range)
        : QAccessibleWidget(range, QAccessible::Slider) {}

    [[nodiscard]] KRangeSelector* range() const { return static_cast<KRangeSelector*>(widget()); }
    [[nodiscard]] bool upper() const {
        return range()->activeHandle() == KRangeSelector::Handle::Upper;
    }

    void* interface_cast(const QAccessible::InterfaceType type) override {
        if (type == QAccessible::ValueInterface)
            return static_cast<QAccessibleValueInterface*>(this);
        return QAccessibleWidget::interface_cast(type);
    }

    [[nodiscard]] QString text(const QAccessible::Text type) const override {
        if (type == QAccessible::Value)
            return QObject::tr("%1 to %2")
                .arg(QString::number(range()->lowerValue()),
                     QString::number(range()->upperValue()));
        return QAccessibleWidget::text(type);
    }

    [[nodiscard]] QVariant currentValue() const override {
        return upper() ? range()->upperValue() : range()->lowerValue();
    }
    void setCurrentValue(const QVariant& value) override {
        bool ok = false;
        const double number = value.toDouble(&ok);
        if (!ok || range()->hasLiveEdit())
            return;
        Q_EMIT range()->editStarted();
        upper() ? range()->setUpperValue(number) : range()->setLowerValue(number);
        Q_EMIT range()->editFinished();
    }
    [[nodiscard]] QVariant maximumValue() const override {
        return upper() ? range()->maximum() : range()->upperValue();
    }
    [[nodiscard]] QVariant minimumValue() const override {
        return upper() ? range()->lowerValue() : range()->minimum();
    }
    [[nodiscard]] QVariant minimumStepSize() const override {
        return (range()->maximum() - range()->minimum()) * kKeyStepFraction;
    }
};

} // namespace

QAccessibleInterface* kitAccessibleFactory(const QString& className, QObject* object) {
    if (object == nullptr || !object->isWidgetType())
        return nullptr;
    if (className == QLatin1String(KDropdown::staticMetaObject.className()))
        return new DropdownAccessible(static_cast<KDropdown*>(object));
    if (className == QLatin1String(KRadioGroup::staticMetaObject.className()))
        return new RadioGroupAccessible(static_cast<KRadioGroup*>(object));
    if (className == QLatin1String(KProgressBar::staticMetaObject.className()))
        return new ProgressBarAccessible(static_cast<KProgressBar*>(object));
    if (className == QLatin1String(KTabBar::staticMetaObject.className()))
        return new TabBarAccessible(static_cast<KTabBar*>(object));
    if (className == QLatin1String(KValueField::staticMetaObject.className()))
        return new ValueFieldAccessible(static_cast<KValueField*>(object));
    if (className == QLatin1String(KSlider::staticMetaObject.className()))
        return new SliderAccessible(static_cast<KSlider*>(object));
    if (className == QLatin1String(KRangeSelector::staticMetaObject.className()))
        return new RangeSelectorAccessible(static_cast<KRangeSelector*>(object));
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
