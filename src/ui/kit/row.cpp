#include <QPainter>
#include <QResizeEvent>
#include <bloom/ui/kit/color_chip.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/row.hpp>
#include <bloom/ui/kit/slider.hpp>
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
        KLabel::resizeEvent(event);
        const QFontMetrics metrics(font());
        setText(metrics.elidedText(fullText_, Qt::ElideRight, width()));
    }

  private:
    QString fullText_;
    int preferredWidth_;
};

} // namespace
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

KPropertyRow::KPropertyRow(QLabel* label, QWidget* indicator,
                           std::initializer_list<QWidget*> values, QWidget* parent,
                           bool leadingIndicator)
    : QWidget(parent), label_(label), leadingIndicator_(leadingIndicator) {
    this->setObjectName(QStringLiteral("propertiesRow"));
    this->setProperty("rowLabel", label->toolTip());
    setFixedHeight(px(Size::PropertyRow));
    this->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* layout = new QHBoxLayout(this);
    layout->setSizeConstraint(QLayout::SetNoConstraint);
    layout->setContentsMargins(px(Spacing::RowPadding), px(Spacing::RowPadding),
                               px(Spacing::RowPadding), px(Spacing::RowPadding));
    layout->setSpacing(kit::px(kit::Spacing::XXS));
    label->setFixedWidth(
        px(leadingIndicator_ ? Size::PropertyLabelCompact : Size::PropertiesLabelWidth));
    label->setFixedHeight(px(Size::Control));
    if (leadingIndicator_ && indicator) {
        indicator->setFixedWidth(px(Size::ToggleCell));
        layout->addWidget(indicator, 0, Qt::AlignVCenter);
        layout->addSpacing(px(Spacing::XS));
    }
    layout->addWidget(label);
    layout->addSpacing(px(Spacing::PropertyGutter));
    bool expanding = false;
    for (auto* value : values) {
        if (auto* dropdown = qobject_cast<kit::KDropdown*>(value)) {
            dropdown->setControlSize(kit::KDropdown::ControlSize::Compact);
            dropdown->setFont(label->font());
            dropdown->setWidthFloor(kit::px(kit::Size::PropertiesDropdownWidth));
            dropdown->setFixedHeight(kit::px(kit::Size::Control));
        }
        if (auto* chip = qobject_cast<kit::KColorChip*>(value))
            chip->setFixedSize(kit::px(kit::Size::PropertiesFieldWidth),
                               kit::px(kit::Size::PropertiesSwatchHeight));
        const bool flexible = !leadingIndicator_ && (qobject_cast<kit::KSlider*>(value) ||
                                                     value->maximumWidth() == QWIDGETSIZE_MAX);
        expanding = expanding || flexible;
        layout->addWidget(value, flexible ? 1 : 0, Qt::AlignVCenter);
    }
    if (!expanding)
        layout->addStretch(1);
    if (leadingIndicator_)
        return;
    auto* slot = indicator ? indicator : new QWidget(this);
    slot->setFixedWidth(kit::px(kit::Size::PropertiesDiamondColumn));
    auto policy = slot->sizePolicy();
    policy.setRetainSizeWhenHidden(true);
    slot->setSizePolicy(policy);
    layout->addWidget(slot, 0, Qt::AlignVCenter);
}
void KPropertyRow::setLineCount(int lines) {
    const auto count = std::max(1, lines);
    setProperty("rowLines", count);
    setFixedHeight(px(Size::PropertyRow) * count);
}
QSize KPropertyRow::minimumSizeHint() const {
    auto size = QWidget::minimumSizeHint();
    size.setWidth(size.width() - label_->width() + px(Size::PropertiesLabelMinWidth) -
                  layout()->spacing() * (layout()->count() - 1));
    return size;
}
void KPropertyRow::resizeEvent(QResizeEvent* event) {
    layout()->setSpacing(px(Spacing::XXS));
    if (leadingIndicator_) {
        QWidget::resizeEvent(event);
        return;
    }
    label_->setFixedWidth(px(width() < px(Size::PanelMinWidth) ? Size::PropertiesLabelMinWidth
                                                               : Size::PropertiesLabelWidth));
    QWidget::resizeEvent(event);
}
KRow::KRow(QWidget* parent) : QWidget(parent), row_(new QHBoxLayout(this)) {
    setFixedHeight(px(Size::ListRow));
    row_->setContentsMargins(leadingInset_, px(Spacing::RowPadding), px(Spacing::RowPadding),
                             px(Spacing::RowPadding));
    row_->setSpacing(0);
    row_->setSizeConstraint(QLayout::SetNoConstraint);
    nameCell_ = new QWidget(this);
    nameCell_->setMinimumWidth(0);
    nameCell_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto* nameLayout = new QHBoxLayout(nameCell_);
    nameLayout->setContentsMargins(px(Spacing::XS), 0, px(Spacing::XS), 0);
    nameLayout->setSpacing(px(Spacing::XS));
    disclosure_ = new KIconButton(nameCell_);
    disclosure_->setProperty("disclosure", true);
    disclosure_->setFixedSize(px(Size::ToggleCell), px(Size::ToggleCell));
    disclosure_->hide();
    name_ = new KLabel(nameCell_);
    name_->setAttribute(Qt::WA_TransparentForMouseEvents);
    name_->setElidedText({});
    nameLayout->addWidget(disclosure_, 0, Qt::AlignVCenter);
    nameLayout->addWidget(name_, 1, Qt::AlignVCenter);
}
void KRow::setCells(const QList<QWidget*>& toggles, QWidget* name, const QList<QWidget*>& columns,
                    QWidget* trailing) {
    for (auto* cell : toggles) {
        cell->setFixedSize(px(Size::ToggleCell), px(Size::ToggleCell));
        cell->setProperty("rowCell", "toggle");
        row_->addWidget(cell, 0, Qt::AlignVCenter);
    }
    row_->addWidget(name ? name : nameCell_, 1);
    for (auto* cell : columns) {
        const auto* dropdown = qobject_cast<KDropdown*>(cell);
        cell->setFixedSize(
            std::max(px(Size::DropdownWidth), dropdown ? dropdown->minimumSizeHint().width() : 0),
            px(Size::Control));
        cell->setProperty("rowCell", "column");
        cell->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        row_->addWidget(cell, 0, Qt::AlignVCenter);
    }
    if (trailing) {
        trailing->setFixedWidth(px(Size::ToggleCell));
        row_->addWidget(trailing);
    }
}
void KRow::setName(const QString& text, std::optional<IconId> disclosure) {
    name_->setElidedText(text);
    disclosure_->setVisible(disclosure.has_value());
    if (disclosure)
        disclosure_->setIcon(icon(*disclosure, IconRole::Chrome));
}
void KRow::setRowState(int index, bool selected) {
    index_ = index;
    selected_ = selected;
    update();
}
void KRow::resizeEvent(QResizeEvent* event) {
    const auto vertical = height() == px(Size::Control) ? 0 : px(Spacing::RowPadding);
    row_->setContentsMargins(leadingInset_, vertical, px(Spacing::RowPadding), vertical);
    QWidget::resizeEvent(event);
}
void KRow::setLeadingInset(int inset) {
    leadingInset_ = inset;
    auto margins = row_->contentsMargins();
    margins.setLeft(inset);
    row_->setContentsMargins(margins);
}
void KRow::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), color(selected_ ? Color::SurfaceRaised : Color::Surface));
    painter.setPen(color(Color::Border));
    painter.drawLine(rect().bottomLeft(), rect().bottomRight());
}
} // namespace bloom::ui::kit
