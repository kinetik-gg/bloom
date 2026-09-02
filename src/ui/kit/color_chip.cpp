#include <bloom/ui/kit/color_chip.hpp>

#include <bloom/ui/kit/color_picker.hpp>
#include <bloom/ui/kit/painting.hpp>

#include <QEnterEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>

namespace bloom::ui::kit {
namespace {

// A larger cell than the token-minimum spacing step: at these control sizes (a ~26px chip,
// a 22px-tall alpha bar) a 2px checker reads as noise once anti-aliased, and pixel-sampling
// it in a test would be sampling blend artifacts rather than the pattern itself.
[[nodiscard]] int checkerCellPx() { return px(Spacing::S); }

// KColorChip and KColorABPair both declare their own color()/colorA()/colorB() accessors, which
// hides the free bloom::ui::kit::color(Color) token lookup inside their member functions (plain
// C++ name-hiding: a member found by unqualified lookup stops the search before it ever reaches
// the enclosing namespace). This thin wrapper -- itself a free function, so it is never hidden --
// is what those member functions call instead.
[[nodiscard]] QColor tokenColor(const Color role) { return color(role); }

} // namespace

KColorChip::KColorChip(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("kColorChip"));
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_Hover, true);
    setCursor(Qt::PointingHandCursor);
}

KColorChip::~KColorChip() = default;

void KColorChip::setColor(const KColor& newColor) {
    if (color_ == newColor) {
        return;
    }
    color_ = newColor;
    update();
    Q_EMIT colorChanged(color_);
}

KColor KColorChip::color() const noexcept { return color_; }

void KColorChip::setShape(const Shape shape) {
    if (shape_ == shape) {
        return;
    }
    shape_ = shape;
    update();
}

KColorChip::Shape KColorChip::shape() const noexcept { return shape_; }

void KColorChip::setControlSize(const ControlSize size) {
    if (controlSize_ == size) {
        return;
    }
    controlSize_ = size;
    updateGeometry();
    update();
}

KColorChip::ControlSize KColorChip::controlSize() const noexcept { return controlSize_; }

void KColorChip::ensurePicker() {
    if (picker_ != nullptr) {
        return;
    }
    picker_ = new KColorPicker();
    picker_->setColor(color_);
    connect(picker_, &KColorPicker::colorChanged, this, &KColorChip::setColor);
}

KColorPicker* KColorChip::picker() {
    ensurePicker();
    return picker_;
}

void KColorChip::openPicker() {
    if (!isEnabled()) {
        return;
    }
    ensurePicker();
    picker_->setColor(color_);
    picker_->openBelow(*this);
}

void KColorChip::closePicker() {
    if (picker_ != nullptr) {
        picker_->close();
    }
}

bool KColorChip::isPickerOpen() const { return picker_ != nullptr && picker_->isVisible(); }

State KColorChip::visualState() const {
    if (!isEnabled()) {
        return State::Disabled;
    }
    if (isPickerOpen()) {
        return State::Pressed;
    }
    if (hovered_) {
        return State::Hover;
    }
    if (hasFocus()) {
        return State::Focused;
    }
    return State::Normal;
}

int KColorChip::controlExtent() const {
    switch (controlSize_) {
    case ControlSize::Compact:
        return px(Size::ControlCompact);
    case ControlSize::Default:
        return px(Size::Control);
    case ControlSize::Roomy:
        return px(Size::ControlRoomy);
    }
    return px(Size::Control);
}

QSize KColorChip::sizeHint() const {
    const auto ringMargin = static_cast<int>(std::lround(kFocusRingWidth)) * 2;
    const int extent = controlExtent() + ringMargin;
    return {extent, extent};
}

QSize KColorChip::minimumSizeHint() const { return sizeHint(); }

void KColorChip::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && isEnabled()) {
        setFocus(Qt::MouseFocusReason);
        isPickerOpen() ? closePicker() : openPicker();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void KColorChip::keyPressEvent(QKeyEvent* event) {
    switch (event->key()) {
    case Qt::Key_Space:
    case Qt::Key_Return:
    case Qt::Key_Enter:
        openPicker();
        event->accept();
        return;
    case Qt::Key_Escape:
        if (isPickerOpen()) {
            closePicker();
            event->accept();
            return;
        }
        break;
    default:
        break;
    }
    QWidget::keyPressEvent(event);
}

void KColorChip::enterEvent(QEnterEvent* event) {
    if (isEnabled()) {
        hovered_ = true;
        update();
    }
    QWidget::enterEvent(event);
}

void KColorChip::leaveEvent(QEvent* event) {
    hovered_ = false;
    update();
    QWidget::leaveEvent(event);
}

void KColorChip::changeEvent(QEvent* event) {
    if (event->type() == QEvent::EnabledChange && !isEnabled()) {
        hovered_ = false;
    }
    QWidget::changeEvent(event);
}

void KColorChip::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event)
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const State state = visualState();
    const auto ringMargin = kFocusRingWidth;
    const QRectF bounds = QRectF(rect()).adjusted(ringMargin, ringMargin, -ringMargin, -ringMargin);

    const QColor swatch = color_.toQColor();
    if (swatch.alpha() < 255) {
        drawAlphaCheckerboard(painter, bounds, checkerCellPx(),
                              shape_ == Shape::Circle ? Radius::Full : Radius::Small);
    }

    if (shape_ == Shape::Circle) {
        painter.save();
        painter.setBrush(swatch);
        painter.setPen(Qt::NoPen);
        painter.drawEllipse(bounds);
        painter.restore();
        applyHairlinePen(painter, tokenColor(borderForState(state)));
        painter.setBrush(Qt::NoBrush);
        const qreal inset = painter.pen().widthF() / 2.0;
        painter.drawEllipse(bounds.adjusted(inset, inset, -inset, -inset));
    } else {
        fillRoundedSurface(painter, bounds, swatch, tokenColor(borderForState(state)),
                           Radius::Small);
    }

    if (hasFocus() && state != State::Disabled) {
        drawFocusRing(painter, bounds, shape_ == Shape::Circle ? Radius::Full : Radius::Small);
    }
}

KColorABPair::KColorABPair(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("kColorABPair"));
    chipA_ = new KColorChip(this);
    chipA_->setObjectName(QStringLiteral("kColorABPairChipA"));
    chipB_ = new KColorChip(this);
    chipB_->setObjectName(QStringLiteral("kColorABPairChipB"));
    connect(chipA_, &KColorChip::colorChanged, this, &KColorABPair::colorAChanged);
    connect(chipB_, &KColorChip::colorChanged, this, &KColorABPair::colorBChanged);
    layoutChips();
}

void KColorABPair::setColorA(const KColor& newColor) { chipA_->setColor(newColor); }
KColor KColorABPair::colorA() const noexcept { return chipA_->color(); }
void KColorABPair::setColorB(const KColor& newColor) { chipB_->setColor(newColor); }
KColor KColorABPair::colorB() const noexcept { return chipB_->color(); }

void KColorABPair::swap() {
    const KColor a = chipA_->color();
    const KColor b = chipB_->color();
    chipA_->setColor(b);
    chipB_->setColor(a);
    Q_EMIT swapped();
}

KColorChip* KColorABPair::chipA() const noexcept { return chipA_; }
KColorChip* KColorABPair::chipB() const noexcept { return chipB_; }

QRectF KColorABPair::swapButtonRect() const {
    const auto chipExtent = static_cast<qreal>(chipA_->sizeHint().width());
    const auto overlap = chipExtent * 0.5;
    const auto swapExtent = static_cast<qreal>(px(Size::IconSmall)) + px(Spacing::XS);
    return {overlap - swapExtent / 2.0, chipExtent - overlap - swapExtent / 2.0, swapExtent,
            swapExtent};
}

void KColorABPair::layoutChips() {
    const auto chipExtent = chipA_->sizeHint().width();
    const int overlap = chipExtent / 2;
    chipA_->move(0, 0);
    chipA_->resize(chipA_->sizeHint());
    chipB_->move(overlap, overlap);
    chipB_->resize(chipB_->sizeHint());
    setFixedSize(overlap + chipExtent, overlap + chipExtent);
}

QSize KColorABPair::sizeHint() const { return size(); }
QSize KColorABPair::minimumSizeHint() const { return size(); }

void KColorABPair::resizeEvent(QResizeEvent* event) { QWidget::resizeEvent(event); }

void KColorABPair::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && swapButtonRect().contains(event->position())) {
        swap();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void KColorABPair::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event)
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    // A small swap glyph -- two counter-rotating chevrons -- drawn directly rather than through a
    // vendored icon asset: IconId has no "swap" member, and adding one means downloading and
    // pinning a new upstream SVG (visual-language.md's provenance requirement) for a single small
    // control. The geometry below is derived from IconSmall and Spacing tokens, not a literal.
    const QRectF glyphBox = swapButtonRect();
    fillRoundedSurface(painter, glyphBox, color(Color::SurfaceRaised), color(Color::Border),
                       Radius::Full);
    painter.save();
    QPen pen(color(Color::Foreground));
    pen.setWidthF(kFocusRingWidth);
    pen.setCapStyle(Qt::RoundCap);
    painter.setPen(pen);
    const qreal armLength = glyphBox.width() * 0.28;
    const QPointF topStart(glyphBox.left() + glyphBox.width() * 0.28, glyphBox.center().y());
    const QPointF topEnd(glyphBox.right() - glyphBox.width() * 0.28, glyphBox.center().y());
    painter.drawLine(topStart, topEnd);
    painter.drawLine(topEnd, topEnd + QPointF(-armLength * 0.5, -armLength * 0.5));
    painter.drawLine(topStart, topStart + QPointF(armLength * 0.5, armLength * 0.5));
    painter.restore();
}

} // namespace bloom::ui::kit
