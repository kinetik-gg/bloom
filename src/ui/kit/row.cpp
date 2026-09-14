#include <bloom/ui/kit/row.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/color_chip.hpp>
#include <bloom/ui/kit/slider.hpp>
#include <QResizeEvent>
#include <QPainter>
namespace bloom::ui::kit {
namespace {
class PropertyRowLabel final : public KLabel {
  public:
    PropertyRowLabel(QString fullText, const int preferredWidth, QWidget* parent)
        : KLabel(parent), fullText_(std::move(fullText)), preferredWidth_(preferredWidth) {
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


}
QLabel* makePropertyRowLabel(const QString& text, QWidget* parent) {
    auto* label = new PropertyRowLabel(text, px(Size::PropertiesLabelWidth), parent);
    label->setObjectName(QStringLiteral("propertiesRowLabel"));
    auto labelFont = kit::font(kit::TypeRole::UiSmall);
    labelFont.setCapitalization(QFont::MixedCase);
    labelFont.setLetterSpacing(QFont::PercentageSpacing, 100.0);
    label->setFont(labelFont);
    QPalette palette = label->palette();
    palette.setColor(QPalette::WindowText, kit::color(kit::Color::Muted));
    label->setPalette(palette);
    label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    return label;
}

KPropertyRow::KPropertyRow(QLabel* label, QWidget* indicator, std::initializer_list<QWidget*> values, QWidget* parent) : QWidget(parent), label_(label) {
    this->setObjectName(QStringLiteral("propertiesRow"));
    this->setProperty("rowLabel", label->toolTip());
    setFixedHeight(px(Size::PropertyRow));
    this->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* layout = new QHBoxLayout(this);
    layout->setSizeConstraint(QLayout::SetNoConstraint);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(kit::px(kit::Spacing::XXS));
    label->setFixedWidth(px(Size::PropertiesLabelWidth));
    layout->addWidget(label);
    bool expanding = false;
    for (auto* value : values) {
        if (auto* dropdown = qobject_cast<kit::KDropdown*>(value)) {
            dropdown->setControlSize(kit::KDropdown::ControlSize::Compact);
            dropdown->setFont(label->font());
            dropdown->setFixedWidth(kit::px(kit::Size::PropertiesDropdownWidth));
            dropdown->setFixedHeight(kit::px(kit::Size::Control));
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
    auto* slot = indicator ? indicator : new QWidget(this);
    slot->setFixedWidth(kit::px(kit::Size::PropertiesDiamondColumn));
    auto policy = slot->sizePolicy();
    policy.setRetainSizeWhenHidden(true);
    slot->setSizePolicy(policy);
    layout->addWidget(slot, 0, Qt::AlignVCenter);
}
QSize KPropertyRow::minimumSizeHint() const {
    auto size = QWidget::minimumSizeHint();
    size.setWidth(size.width() - label_->width() + px(Size::PropertiesLabelMinWidth));
    return size;
}
void KPropertyRow::resizeEvent(QResizeEvent* event) {
    label_->setFixedWidth(px(width() < px(Size::PanelMinWidth) ? Size::PropertiesLabelMinWidth : Size::PropertiesLabelWidth));
    QWidget::resizeEvent(event);
}
KRow::KRow(QWidget* parent) : QWidget(parent), row_(new QHBoxLayout(this)) {
    setFixedHeight(px(Size::ListRow));
    row_->setContentsMargins(0, 0, 0, 0);
    row_->setSpacing(0);
}
void KRow::setCells(const QList<QWidget*>& toggles, QWidget* name, const QList<QWidget*>& columns, QWidget* trailing) {
    for (auto* cell : toggles) { cell->setFixedSize(px(Size::ToggleCell), px(Size::Control)); row_->addWidget(cell, 0, Qt::AlignVCenter); }
    row_->addWidget(name, 1);
    for (auto* cell : columns) { cell->setFixedSize(px(Size::DropdownWidth), px(Size::Control)); row_->addWidget(cell, 0, Qt::AlignVCenter); }
    if (trailing) { trailing->setFixedWidth(px(Size::ToggleCell)); row_->addWidget(trailing); }
}
void KRow::setRowState(int index, bool selected) { index_ = index; selected_ = selected; update(); }
void KRow::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), color(index_ % 2 == 0 ? Color::Surface : Color::SurfaceRaised));
    if (selected_) painter.fillRect(QRect(0, 0, px(Spacing::XXS), height()), color(Color::Accent));
    painter.setPen(color(Color::Border)); painter.drawLine(rect().bottomLeft(), rect().bottomRight());
}
} // namespace bloom::ui::kit
