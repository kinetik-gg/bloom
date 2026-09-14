#include "properties_sections.hpp"

#include "node_editor_items.hpp"

#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/icons.hpp>
#include <bloom/ui/kit/section.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/kit/value_field.hpp>

#include <QFontMetrics>
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

} // namespace

int labelColumnWidth() {
    // Every label the panel's hand-crafted rows can show. A generic registry row (deliverable 3)
    // measures nothing new here on purpose: its label is whatever the registry named the parameter,
    // and PropertyRowLabel elides anything wider than this column rather than widening the panel.
    static const std::array<QString, 21> kLabels{
        QObject::tr("Position"), QObject::tr("Anchor"),       QObject::tr("Scale"),
        QObject::tr("Rotation"), QObject::tr("Opacity"),      QObject::tr("Blending"),
        QObject::tr("RGBA"),     QObject::tr("Alpha"),        QObject::tr("Encoding"),
        QObject::tr("Name"),     QObject::tr("Format"),       QObject::tr("Frame Rate"),
        QObject::tr("Duration"), QObject::tr("Pixel Aspect"), QObject::tr("Content"),
        QObject::tr("Size"),     QObject::tr("Color"),        QObject::tr("Font"),
        QObject::tr("Visible"),  QObject::tr("Solo"),         QObject::tr("Locked"),
    };
    const QFontMetrics metrics(kit::font(kit::TypeRole::Ui));
    int widest = 0;
    for (const auto& label : kLabels) {
        widest = std::max(widest, metrics.horizontalAdvance(label));
    }
    return widest;
}

QLabel* makeRowLabel(const QString& text, QWidget* parent) {
    auto* label = new PropertyRowLabel(text, labelColumnWidth(), parent);
    label->setObjectName(QStringLiteral("propertiesRowLabel"));
    label->setFont(kit::font(kit::TypeRole::Ui));
    QPalette palette = label->palette();
    palette.setColor(QPalette::WindowText, kit::color(kit::Color::Muted));
    label->setPalette(palette);
    label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    return label;
}

QWidget* addRow(QVBoxLayout* section, QWidget* sectionParent, QLabel* label, QWidget* indicator,
                const std::initializer_list<QWidget*> values) {
    auto* row = new QWidget(sectionParent);
    row->setObjectName(QStringLiteral("propertiesRow"));
    row->setMinimumHeight(kit::px(kit::Size::Control));
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(kit::px(kit::Spacing::XS), kit::px(kit::Spacing::XXS),
                               kit::px(kit::Spacing::XS), kit::px(kit::Spacing::XXS));
    layout->setSpacing(kit::px(kit::Spacing::S));
    layout->addWidget(label);
    if (indicator != nullptr) {
        layout->addWidget(indicator);
    }
    // Task P2 (owner review 2026-09-12): the row is a plain, non-painting container -- every hover
    // and focus affordance comes from the kit control itself, never from this widget.
    for (auto* value : values) {
        layout->addWidget(value, 1);
    }
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
    QPalette palette = label->palette();
    palette.setColor(QPalette::WindowText, kit::color(kit::Color::Foreground));
    label->setPalette(palette);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    return label;
}

kit::KValueField* makeValueCell(const ValueCellSpec& spec, QWidget* parent) {
    auto* field = new kit::KValueField(parent);
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
    layout->setSpacing(kit::px(kit::Spacing::S));
    for (auto* cell : cells) {
        layout->addWidget(cell);
    }
    return group;
}

QWidget* makeLinkToggle(const QString& objectName, const QString& tooltip, QWidget* parent) {
    auto* toggle = new kit::KButton(parent);
    toggle->setObjectName(objectName);
    toggle->setVariant(kit::KButton::Variant::Ghost);
    toggle->setControlSize(kit::KButton::ControlSize::Compact);
    toggle->setIconId(kit::IconId::Link);
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
