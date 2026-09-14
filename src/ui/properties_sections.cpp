#include "properties_sections.hpp"
#include <bloom/ui/kit/row.hpp>

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
#include <QVariant>

#include <algorithm>
#include <array>
#include <utility>

namespace bloom::ui::properties {
namespace {} // namespace
int labelColumnWidth() { return kit::px(kit::Size::PropertiesLabelWidth); }
QLabel* makeRowLabel(const QString& text, QWidget* parent) {
    return kit::makePropertyRowLabel(text, parent);
}
QWidget* addRow(QVBoxLayout* section, QWidget* sectionParent, QLabel* label, QWidget* indicator,
                const std::initializer_list<QWidget*> values) {
    auto* row = new kit::KPropertyRow(label, indicator, values, sectionParent);
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
    layout->setSpacing(kit::px(kit::Spacing::XXS));
    for (auto* cell : cells) {
        layout->addWidget(cell);
    }
    const int width = static_cast<int>(cells.size()) * kit::px(kit::Size::PropertiesFieldWidth) +
                      (static_cast<int>(cells.size()) - 1) * kit::px(kit::Spacing::XS);
    group->setMaximumWidth(width);
    return group;
}

QWidget* addColorRow(QVBoxLayout* rows, QWidget* parent, kit::KColorChip* chip, QWidget* diamond,
                     const std::initializer_list<QWidget*> fields, const QString& expandName,
                     const QString& groupName) {
    auto* expand = new kit::KButton(parent);
    expand->setObjectName(expandName);
    expand->setIconId(kit::IconId::CaretRight);
    expand->setVariant(kit::KButton::Variant::Ghost);
    expand->setCheckable(true);
    expand->setFixedSize(kit::px(kit::Size::ControlCompact), kit::px(kit::Size::ControlCompact));
    expand->setToolTip(QObject::tr("Show RGBA components"));
    expand->setAccessibleName(QObject::tr("Show RGBA components"));
    auto* row =
        addRow(rows, parent, makeRowLabel(QObject::tr("Color"), parent), diamond, {chip, expand});
    auto* details = makeCellGroup(groupName, fields, parent);
    details->setProperty("disclosureFor", QObject::tr("Color"));
    details->setProperty("expanded", false);
    // Four channels share the full card width below the swatch.
    for (auto* field : fields)
        field->setMinimumWidth(kit::px(kit::Size::PropertiesColorMinWidth));
    details->setProperty("colorOwner", QVariant::fromValue(static_cast<QObject*>(row)));
    for (auto* field : fields) {
        field->setContextMenuPolicy(Qt::CustomContextMenu);
        field->setProperty("rowMenuForwarded", true);
        QObject::connect(field, &QWidget::customContextMenuRequested, row,
                         [row, field](const QPoint& point) {
                             Q_EMIT row->customContextMenuRequested(
                                 row->mapFromGlobal(field->mapToGlobal(point)));
                         });
    }
    rows->addWidget(details);
    details->hide();
    QObject::connect(expand, &kit::KButton::toggled, details, [details, expand](bool on) {
        details->setProperty("expanded", on);
        details->setVisible(on);
        expand->setIconId(on ? kit::IconId::CaretDown : kit::IconId::CaretRight);
    });
    return row;
}

QWidget* makeLinkToggle(const QString& objectName, const QString& tooltip, QWidget* parent) {
    auto* toggle = new kit::KButton(parent);
    toggle->setObjectName(objectName);
    toggle->setVariant(kit::KButton::Variant::Ghost);
    toggle->setIconId(kit::IconId::Link);
    toggle->setFixedSize(kit::px(kit::Size::ControlCompact), kit::px(kit::Size::ControlCompact));
    toggle->setCheckable(true);
    QObject::connect(toggle, &kit::KButton::toggled, toggle, [toggle](bool linked) {
        toggle->setVariant(linked ? kit::KButton::Variant::Primary : kit::KButton::Variant::Ghost);
    });
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
