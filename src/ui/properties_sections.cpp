#include "properties_sections.hpp"

#include "node_editor_items.hpp"

#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/color_chip.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/icons.hpp>
#include <bloom/ui/kit/section.hpp>
#include <bloom/ui/kit/slider.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/kit/value_field.hpp>

#include <QFontMetrics>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPalette>
#include <QResizeEvent>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <utility>

namespace bloom::ui::properties {
namespace {

// task WIDTH-1: the row's outer label keeps the shared column width as its PREFERRED width (so a
// roomy panel still lines every row's value column up at the same x, decision 1's whole point) but
// lets the layout shrink it, eliding the live text with Qt::ElideRight down to whatever width it
// actually gets and always carrying the untruncated name in the tooltip -- a narrow panel degrades
// "Pixel Aspect" to "Pixel A..." rather than silently forcing the row wider.
class PropertyRowLabel final : public QLabel {
  public:
    PropertyRowLabel(QString fullText, const int preferredWidth, QWidget* parent)
        : QLabel(parent), fullText_(std::move(fullText)), preferredWidth_(preferredWidth) {
        setText(fullText_);
        setToolTip(fullText_);
    }

    [[nodiscard]] QSize sizeHint() const override {
        return {preferredWidth_, QLabel::sizeHint().height()};
    }

    [[nodiscard]] QSize minimumSizeHint() const override {
        // Enough for an ellipsis plus a couple of characters -- never zero, or "Rotation" could
        // shrink to a blank column with nothing for the tooltip to explain.
        const QFontMetrics metrics(font());
        const int ellipsisFloor = metrics.horizontalAdvance(QStringLiteral("A…"));
        return {ellipsisFloor, QLabel::minimumSizeHint().height()};
    }

  protected:
    void resizeEvent(QResizeEvent* event) override {
        QLabel::resizeEvent(event);
        const QFontMetrics metrics(font());
        setText(metrics.elidedText(fullText_, Qt::ElideRight, width()));
    }

  private:
    QString fullText_;
    int preferredWidth_;
};

class PropertyRow final : public QWidget {
  public:
    PropertyRow(QLabel* label, QWidget* parent) : QWidget(parent), label_(label) {}

  protected:
    void resizeEvent(QResizeEvent* event) override {
        label_->setFixedWidth(kit::px(width() < kit::px(kit::Size::PanelMinWidth)
                                          ? kit::Size::PropertiesLabelMinWidth
                                          : kit::Size::PropertiesLabelWidth));
        QWidget::resizeEvent(event);
    }

  private:
    QLabel* label_;
};

} // namespace

int labelColumnWidth() { return kit::px(kit::Size::PropertiesLabelWidth); }

QLabel* makeRowLabel(const QString& text, QWidget* parent) {
    auto* label = new PropertyRowLabel(text, labelColumnWidth(), parent);
    label->setObjectName(QStringLiteral("propertiesRowLabel"));
    label->setFont(kit::font(kit::TypeRole::UiSmall));
    QPalette palette = label->palette();
    palette.setColor(QPalette::WindowText, kit::color(kit::Color::Muted));
    label->setPalette(palette);
    label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    return label;
}

QWidget* addRow(QVBoxLayout* section, QWidget* sectionParent, QLabel* label, QWidget* indicator,
                const std::initializer_list<QWidget*> values) {
    auto* row = new PropertyRow(label, sectionParent);
    row->setObjectName(QStringLiteral("propertiesRow"));
    row->setProperty("rowLabel", label->toolTip());
    row->setMinimumHeight(kit::px(kit::Size::ControlCompact));
    row->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(kit::px(kit::Spacing::XS));
    label->setFixedWidth(labelColumnWidth());
    layout->addWidget(label);
    bool expanding = false;
    for (auto* value : values) {
        if (auto* dropdown = qobject_cast<kit::KDropdown*>(value)) {
            dropdown->setFixedWidth(kit::px(kit::Size::PropertiesDropdownWidth));
            dropdown->setFixedHeight(kit::px(kit::Size::ControlCompact));
        }
        if (auto* chip = qobject_cast<kit::KColorChip*>(value))
            chip->setFixedSize(kit::px(kit::Size::PropertiesFieldWidth),
                               kit::px(kit::Size::PropertiesSwatchHeight));
        const bool flexible =
            qobject_cast<kit::KSlider*>(value) || value->maximumWidth() == QWIDGETSIZE_MAX;
        expanding = expanding || flexible;
        layout->addWidget(value, flexible ? 1 : 0, Qt::AlignVCenter);
    }
    if (!expanding)
        layout->addStretch(1);
    auto* slot = indicator ? indicator : new QWidget(row);
    slot->setFixedWidth(kit::px(kit::Size::PropertiesDiamondColumn));
    auto policy = slot->sizePolicy();
    policy.setRetainSizeWhenHidden(true);
    slot->setSizePolicy(policy);
    layout->addWidget(slot, 0, Qt::AlignVCenter);
    section->addWidget(row);
    return row;
}

QWidget* addRow(QVBoxLayout* section, QWidget* sectionParent, QLabel* label, QWidget* indicator,
                QWidget* value) {
    return addRow(section, sectionParent, label, indicator, {value});
}

KeyframeDiamond* makeKeyframeDiamond(CompositionSession& session, const std::string_view role,
                                     QWidget* parent) {
    // objectName "propertiesKeyframeIndicator" is deliberately unchanged -- same role, same name --
    // so every existing projection assertion keeps finding it.
    auto* diamond = new KeyframeDiamond(session, std::string(role), parent);
    diamond->setObjectName(QStringLiteral("propertiesKeyframeIndicator"));
    return diamond;
}

QLabel* makeReadOnlyValueLabel(const kit::TypeRole role, QWidget* parent) {
    auto* label = new QLabel(parent);
    label->setFont(kit::font(role));
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    QPalette palette = label->palette();
    palette.setColor(QPalette::WindowText, kit::color(kit::Color::Foreground));
    label->setPalette(palette);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    return label;
}

kit::KValueField* makeValueCell(const ValueCellSpec& spec, QWidget* parent) {
    auto* field = new kit::KValueField(parent);
    field->setCompact(true);
    field->setObjectName(spec.objectName);
    field->setAccessibleName(spec.accessibleName);
    field->setRange(spec.minimum, spec.maximum);
    field->setDecimals(spec.decimals);
    field->setSingleStep(spec.singleStep);
    if (!spec.unit.isEmpty()) {
        field->setUnit(spec.unit);
    }
    if (!spec.subLabel.isEmpty()) {
        field->setLabel(spec.subLabel);
    }
    return field;
}

QWidget* makeCellGroup(const QString& objectName, const std::initializer_list<QWidget*> cells,
                       QWidget* parent) {
    auto* group = new QWidget(parent);
    group->setObjectName(objectName);
    auto* layout = new QHBoxLayout(group);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(kit::px(kit::Spacing::XS));
    for (auto* cell : cells) {
        layout->addWidget(cell);
    }
    const int width = static_cast<int>(cells.size()) * kit::px(kit::Size::PropertiesFieldWidth) +
                      (static_cast<int>(cells.size()) - 1) * kit::px(kit::Spacing::XS);
    group->setMaximumWidth(width);
    return group;
}

QWidget* makeLinkToggle(const QString& objectName, const QString& tooltip, QWidget* parent) {
    auto* toggle = new kit::KButton(parent);
    toggle->setObjectName(objectName);
    toggle->setVariant(kit::KButton::Variant::Ghost);
    toggle->setControlSize(kit::KButton::ControlSize::Compact);
    toggle->setIconId(kit::IconId::Link);
    toggle->setFixedSize(kit::px(kit::Size::ControlCompact), kit::px(kit::Size::ControlCompact));
    toggle->setCheckable(true);
    toggle->setToolTip(tooltip);
    return toggle;
}

kit::KSection* addSection(QVBoxLayout* layout, QWidget* parent, const QString& id,
                          const QString& title) {
    auto* section = new kit::KSection(title, parent);
    section->setObjectName(QStringLiteral("propertiesSection_") + id);
    section->setPersistenceKey(QStringLiteral("properties/sections/%1/collapsed").arg(id));
    layout->addWidget(section);
    return section;
}

} // namespace bloom::ui::properties
