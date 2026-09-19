#include "properties_sections.hpp"
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/row.hpp>
#include <memory>

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
namespace {

QString sectionFilterGroup(const QString& id) {
    if (id == QStringLiteral("object") || id == QStringLiteral("composition"))
        return QStringLiteral("object");
    if (id == QStringLiteral("transform"))
        return QStringLiteral("transform");
    if (id == QStringLiteral("merge-inputs") || id.startsWith(QStringLiteral("upstream-")))
        return QStringLiteral("graph");
    return QStringLiteral("source");
}

} // namespace

void refreshColor(CompositionSession& session, const std::string_view schemaKey,
                  const core::Color4d value, kit::KColorChip* chip,
                  const std::initializer_list<kit::KValueField*> fields) {
    const auto converter = session.colorConverter(schemaKey);
    const auto reference =
        kit::KColor::fromRgba(static_cast<float>(value.red), static_cast<float>(value.green),
                              static_cast<float>(value.blue), static_cast<float>(value.alpha),
                              kit::ColorSpace::Reference);
    const auto display = reference.converted(kit::ColorSpace::Display, converter);
    const QSignalBlocker blocker(chip);
    chip->setColorConverter(converter);
    chip->setColor(reference);
    chip->setEnabled(chip->isEnabled() && display.has_value());
    const bool extended = value.red > 1 || value.green > 1 || value.blue > 1 || value.red < 0 ||
                          value.green < 0 || value.blue < 0 || !display;
    const std::array channels =
        extended
            ? std::array{value.red, value.green, value.blue, value.alpha}
            : std::array{static_cast<double>(display->red), static_cast<double>(display->green),
                         static_cast<double>(display->blue), value.alpha};
    const std::array referenceChannels{value.red, value.green, value.blue, value.alpha};
    std::size_t index = 0;
    for (auto* field : fields) {
        const QSignalBlocker fieldBlocker(field);
        field->setUnit(extended ? QStringLiteral("reference") : QString{});
        field->setProperty("colorReferenceValue", referenceChannels[index]);
        field->setProperty("colorDisplayValue", channels[index]);
        field->setValue(channels[index++]);
    }
    if (!display)
        chip->setToolTip(QObject::tr("Colour conversion unavailable or preparing"));
}

std::optional<core::Color4d>
colorFromFields(CompositionSession& session, const std::string_view schemaKey,
                const std::initializer_list<kit::KValueField*> fields) {
    if (fields.size() != 4)
        return std::nullopt;
    std::array<double, 4> channels{};
    std::size_t index = 0;
    bool reference = false;
    for (const auto* field : fields) {
        channels[index++] = field->value();
        reference = reference || field->unit() == QStringLiteral("reference");
    }
    if (reference)
        return core::Color4d{channels[0], channels[1], channels[2], channels[3]};
    const auto display =
        kit::KColor::fromRgba(static_cast<float>(channels[0]), static_cast<float>(channels[1]),
                              static_cast<float>(channels[2]), static_cast<float>(channels[3]));
    const auto converted =
        display.converted(kit::ColorSpace::Reference, session.colorConverter(schemaKey));
    if (!converted)
        return std::nullopt;
    std::array result{static_cast<double>(converted->red), static_cast<double>(converted->green),
                      static_cast<double>(converted->blue), channels[3]};
    index = 0;
    for (const auto* field : fields) {
        // Retain untouched reference channels exactly; repeated alpha/RGB edits must not
        // accumulate the prepared float transform's round-trip error in other channels.
        if (field->value() == field->property("colorDisplayValue").toDouble())
            result[index] = field->property("colorReferenceValue").toDouble();
        else if (channels[index] < 0 || channels[index] > 1)
            result[index] = channels[index];
        ++index;
    }
    return core::Color4d{result[0], result[1], result[2], result[3]};
}

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
    auto* label = new kit::KLabel(parent);
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

QWidget* makeComponentCell(CompositionSession& session, const std::string_view role,
                           const document::AnimationComponent component, QWidget* field,
                           QWidget* parent, const document::ParameterId parameter) {
    field->setMinimumWidth(kit::px(kit::Size::PropertiesComponentMinWidth));
    auto* cell = new QWidget(parent);
    auto* layout = new QHBoxLayout(cell);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(kit::px(kit::Spacing::XXS));
    auto* diamond = new KeyframeDiamond(session, std::string(role), cell);
    diamond->setObjectName("propertiesComponentDiamond");
    diamond->setParameterId(parameter);
    diamond->setComponent(component);
    layout->addWidget(field, 1);
    layout->addWidget(diamond);
    QObject::connect(&session, &CompositionSession::snapshotChanged, diamond,
                     &KeyframeDiamond::refresh);
    QObject::connect(&session, &CompositionSession::selectionChanged, diamond,
                     &KeyframeDiamond::refresh);
    QObject::connect(&session, &CompositionSession::currentTimeChanged, diamond,
                     &KeyframeDiamond::refresh);
    return cell;
}

QWidget* addColorRow(CompositionSession& session, const std::string_view role, QVBoxLayout* rows,
                     QWidget* parent, kit::KColorChip* chip, QWidget* diamond,
                     const std::initializer_list<QWidget*> fields, const QString& expandName,
                     const QString& groupName, const document::ParameterId parameter) {
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
    auto* details = new QWidget(parent);
    details->setObjectName(groupName);
    auto* grid = new QGridLayout(details);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(kit::px(kit::Spacing::XXS));
    int channel = 0;
    for (auto* field : fields) {
        const std::array channels{
            document::AnimationComponent::Red, document::AnimationComponent::Green,
            document::AnimationComponent::Blue, document::AnimationComponent::Alpha};
        grid->addWidget(makeComponentCell(session, role,
                                          channels[static_cast<std::size_t>(channel)], field,
                                          details, parameter),
                        channel / 2, channel % 2);
        ++channel;
    }
    details->setMaximumWidth(
        (kit::px(kit::Size::PropertiesFieldWidth) + kit::px(kit::Size::IconSmall)) * 2 +
        kit::px(kit::Spacing::XXS));
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
    auto* detailRow = addRow(rows, parent, makeRowLabel({}, parent), nullptr, details);
    qobject_cast<kit::KPropertyRow*>(detailRow)->setLineCount(2);
    detailRow->hide();
    details->hide();
    QObject::connect(expand, &kit::KButton::toggled, details,
                     [details, detailRow, expand](bool on) {
                         details->setProperty("expanded", on);
                         details->setVisible(on);
                         detailRow->setVisible(on);
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

kit::KSection* makeSection(QVBoxLayout* layout, QWidget* parent, const QString& objectName,
                           const QString& title, const QString& persistenceKey) {
    auto* section = new kit::KSection(title, parent);
    section->setObjectName(objectName);
    section->setPersistenceKey(persistenceKey);
    layout->addWidget(section);
    return section;
}

kit::KSection* addSection(QVBoxLayout* layout, QWidget* parent, const QString& id,
                          const QString& title) {
    auto* section = makeSection(layout, parent, QStringLiteral("propertiesSection_") + id, title,
                                QStringLiteral("properties/sections/%1/collapsed").arg(id));
    section->setProperty("propertiesSectionGroup", sectionFilterGroup(id));
    return section;
}

} // namespace bloom::ui::properties
