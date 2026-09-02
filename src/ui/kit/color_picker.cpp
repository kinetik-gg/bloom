#include <bloom/ui/kit/color_picker.hpp>

#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/painting.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/kit/value_field.hpp>

#include <QLineEdit>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>

#include <algorithm>
#include <array>
#include <cmath>

namespace bloom::ui::kit {
namespace {

// The custom-painted canvas (SV square + hue bar + alpha bar) has a fixed geometry rather than one
// that grows with the widget: every drag test can then assert against a known rect, and the
// picker's overall size is simply whatever that fixed canvas plus its token-spaced chrome rows
// need. Every number below reads off an existing Size/Spacing token, never a bare pixel count.
[[nodiscard]] int squareSidePx() { return px(Size::ControlRoomy) * 5; }
[[nodiscard]] int hueBarWidthPx() { return px(Size::ControlCompact); }
[[nodiscard]] int alphaBarHeightPx() { return px(Size::ControlCompact); }
[[nodiscard]] int barGapPx() { return px(Spacing::S); }
[[nodiscard]] int contentWidthPx() { return squareSidePx() + barGapPx() + hueBarWidthPx(); }
[[nodiscard]] int checkerCellPx() { return px(Spacing::XXS); }
[[nodiscard]] qreal markerRadiusPx() { return static_cast<qreal>(px(Spacing::S)) / 2.0; }

[[nodiscard]] QString hexFieldStyleSheet() {
    return expandTokens(QStringLiteral(R"(
QLineEdit#kColorHexField {
    background: {color.Field};
    border: {border.Hairline}px solid {color.Border};
    border-radius: {radius.Small}px;
    color: {color.Foreground};
    padding: {space.XXS}px {space.S}px;
}
QLineEdit#kColorHexField:focus {
    border-color: {color.Accent};
}
QLineEdit#kColorHexField:disabled {
    color: {color.Muted};
}
)"));
}

// A double-ring dot: an outer Background-colored halo so the marker reads against a light patch of
// the gradient, and an inner Foreground ring so it reads against a dark one. Same idea as the focus
// ring's "state is never color alone" rule, just applied to a draggable dot instead of a border.
void paintMarkerDot(QPainter& painter, const QPointF& center) {
    painter.save();
    painter.setBrush(Qt::NoBrush);
    QPen halo(color(Color::Background));
    halo.setWidthF(kFocusRingWidth * 2.0);
    painter.setPen(halo);
    painter.drawEllipse(center, markerRadiusPx(), markerRadiusPx());
    QPen ring(color(Color::Foreground));
    ring.setWidthF(kFocusRingWidth);
    painter.setPen(ring);
    painter.drawEllipse(center, markerRadiusPx(), markerRadiusPx());
    painter.restore();
}

// KColorPicker declares its own color() accessor, which hides the free bloom::ui::kit::color(
// Color) token lookup inside every one of its member functions (plain C++ name hiding: a member
// found by unqualified lookup stops the search before it reaches the enclosing namespace). This
// thin wrapper is a free function, so it is never hidden, and is what KColorPicker's own methods
// call instead of `color(...)` directly.
[[nodiscard]] QColor tokenColor(const Color role) { return color(role); }

void strokeHairline(QPainter& painter, const QRectF& rect) {
    painter.save();
    applyHairlinePen(painter, color(Color::Border));
    painter.setBrush(Qt::NoBrush);
    const qreal inset = painter.pen().widthF() / 2.0;
    painter.drawRect(rect.adjusted(inset, inset, -inset, -inset));
    painter.restore();
}

} // namespace

KColorPicker::KColorPicker(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("kColorPicker"));
    buildChrome();
}

KColorPicker::~KColorPicker() = default;

void KColorPicker::buildChrome() {
    formSelector_ = new KDropdown(this);
    formSelector_->setControlSize(KDropdown::ControlSize::Compact);
    rebuildFormSelector();
    connect(formSelector_, &KDropdown::currentIndexChanged, this, [this](const int index) {
        const QVariant data = formSelector_->itemData(index);
        if (data.isValid()) {
            form_ = static_cast<PickerForm>(data.toInt());
        }
    });

    modelSelector_ = new KDropdown(this);
    modelSelector_->setControlSize(KDropdown::ControlSize::Compact);
    modelSelector_->addItem(colorModelLabel(ColorModel::Hsva), static_cast<int>(ColorModel::Hsva));
    modelSelector_->addItem(colorModelLabel(ColorModel::Hsla), static_cast<int>(ColorModel::Hsla));
    modelSelector_->addItem(colorModelLabel(ColorModel::Rgba), static_cast<int>(ColorModel::Rgba));
    connect(modelSelector_, &KDropdown::currentIndexChanged, this, &KColorPicker::onModelChanged);

    for (auto& field : channelFields_) {
        field = new KValueField(this);
        field->setSingleStep(1.0);
        field->setDecimals(0);
        connect(field, &KValueField::valueChanged, this,
               [this](double) { onChannelFieldEdited(); });
    }

    hexField_ = new QLineEdit(this);
    hexField_->setObjectName(QStringLiteral("kColorHexField"));
    hexField_->setFont(kit::font(TypeRole::Value));
    hexField_->setAlignment(Qt::AlignCenter);
    hexField_->setStyleSheet(hexFieldStyleSheet());
    hexField_->setAccessibleName(tr("Hex color"));
    connect(hexField_, &QLineEdit::editingFinished, this, &KColorPicker::onHexFieldEdited);

    swatchRow_ = new KColorSwatchRow(this);
    swatchRow_->setRecentColorStore(&defaultRecentColorStore_);
    connect(swatchRow_, &KColorSwatchRow::colorActivated, this, &KColorPicker::onSwatchActivated);

    rebuildChannelFields();
    refreshHexField();
    refreshSwatchPreview();
    layoutRegions();
}

void KColorPicker::rebuildFormSelector() {
    for (const PickerForm candidate : pickerForms()) {
        if (!pickerFormAvailable(candidate)) {
            // Not merely disabled: absent from the list entirely (visual-language.md: no dead
            // UI). A later slice that implements one of these adds it here by construction, the
            // moment pickerFormAvailable() reports true for it -- no call-site rewrite.
            continue;
        }
        formSelector_->addItem(pickerFormLabel(candidate), static_cast<int>(candidate));
    }
}

void KColorPicker::layoutRegions() {
    const int margin = px(Spacing::M);
    const int rowGap = px(Spacing::S);
    const int fieldGap = px(Spacing::XXS);
    int y = margin;

    formSelector_->move(margin, y);
    formSelector_->resize(formSelector_->sizeHint());
    y += formSelector_->height() + rowGap;

    svSquareRect_ = QRectF(margin, y, squareSidePx(), squareSidePx());
    hueBarRect_ = QRectF(svSquareRect_.right() + barGapPx(), y, hueBarWidthPx(), squareSidePx());
    y += squareSidePx() + barGapPx();

    alphaBarRect_ = QRectF(margin, y, contentWidthPx(), alphaBarHeightPx());
    y += alphaBarHeightPx() + rowGap;

    modelSelector_->move(margin, y);
    modelSelector_->resize(contentWidthPx(), modelSelector_->sizeHint().height());
    y += modelSelector_->height() + rowGap;

    for (auto* field : channelFields_) {
        field->move(margin, y);
        field->resize(contentWidthPx(), field->sizeHint().height());
        y += field->height() + fieldGap;
    }
    y += rowGap - fieldGap;

    hexField_->move(margin, y);
    hexField_->resize(contentWidthPx(), px(Size::Control));
    y += hexField_->height() + rowGap;

    swatchRow_->move(margin, y);
    swatchRow_->resize(swatchRow_->sizeHint());
    y += swatchRow_->height() + margin;

    setFixedSize(contentWidthPx() + margin * 2, y);
}

void KColorPicker::rebuildChannelFields() {
    struct FieldSpec {
        QString label;
        QString unit;
        double minimum;
        double maximum;
    };
    std::array<FieldSpec, 4> specs{};
    switch (model_) {
    case ColorModel::Hsva:
        specs = {FieldSpec{QStringLiteral("H"), QStringLiteral("°"), 0.0, 360.0},
                FieldSpec{QStringLiteral("S"), QStringLiteral("%"), 0.0, 100.0},
                FieldSpec{QStringLiteral("V"), QStringLiteral("%"), 0.0, 100.0},
                FieldSpec{QStringLiteral("A"), QStringLiteral("%"), 0.0, 100.0}};
        break;
    case ColorModel::Hsla:
        specs = {FieldSpec{QStringLiteral("H"), QStringLiteral("°"), 0.0, 360.0},
                FieldSpec{QStringLiteral("S"), QStringLiteral("%"), 0.0, 100.0},
                FieldSpec{QStringLiteral("L"), QStringLiteral("%"), 0.0, 100.0},
                FieldSpec{QStringLiteral("A"), QStringLiteral("%"), 0.0, 100.0}};
        break;
    case ColorModel::Rgba:
        specs = {FieldSpec{QStringLiteral("R"), QString(), 0.0, 255.0},
                FieldSpec{QStringLiteral("G"), QString(), 0.0, 255.0},
                FieldSpec{QStringLiteral("B"), QString(), 0.0, 255.0},
                FieldSpec{QStringLiteral("A"), QString(), 0.0, 255.0}};
        break;
    }
    for (std::size_t index = 0; index < channelFields_.size(); ++index) {
        channelFields_[index]->setLabel(specs[index].label);
        channelFields_[index]->setUnit(specs[index].unit);
        channelFields_[index]->setRange(specs[index].minimum, specs[index].maximum);
    }
    refreshChannelFields();
}

void KColorPicker::refreshChannelFields() {
    updatingChrome_ = true;
    switch (model_) {
    case ColorModel::Hsva:
        // Read straight from the picker's own sticky HSVA state, not by round-tripping through
        // KColor: that is what keeps the displayed hue exactly where the artist left it while
        // saturation or value passes through a gray.
        channelFields_[0]->setValue(static_cast<double>(hue_));
        channelFields_[1]->setValue(static_cast<double>(saturation_) * 100.0);
        channelFields_[2]->setValue(static_cast<double>(value_) * 100.0);
        channelFields_[3]->setValue(static_cast<double>(alpha_) * 100.0);
        break;
    case ColorModel::Hsla: {
        const auto hsla = currentColor().toHsla();
        channelFields_[0]->setValue(static_cast<double>(hsla[0]));
        channelFields_[1]->setValue(static_cast<double>(hsla[1]) * 100.0);
        channelFields_[2]->setValue(static_cast<double>(hsla[2]) * 100.0);
        channelFields_[3]->setValue(static_cast<double>(hsla[3]) * 100.0);
        break;
    }
    case ColorModel::Rgba: {
        const QColor rgb = currentColor().toQColor();
        channelFields_[0]->setValue(rgb.red());
        channelFields_[1]->setValue(rgb.green());
        channelFields_[2]->setValue(rgb.blue());
        channelFields_[3]->setValue(rgb.alpha());
        break;
    }
    }
    updatingChrome_ = false;
}

void KColorPicker::refreshHexField() {
    hexField_->setText(currentColor().toHex(alpha_ < 1.0F));
}

void KColorPicker::refreshSwatchPreview() { swatchRow_->setCurrentColor(currentColor()); }

KColor KColorPicker::currentColor() const {
    return KColor::fromHsva(hue_, saturation_, value_, alpha_);
}

void KColorPicker::setColor(const KColor& color) {
    const KColor before = currentColor();
    const auto hsva = color.toHsva();
    hue_ = hsva[0];
    saturation_ = hsva[1];
    value_ = hsva[2];
    alpha_ = hsva[3];
    refreshChannelFields();
    refreshHexField();
    refreshSwatchPreview();
    update();
    const KColor after = currentColor();
    if (!(before == after)) {
        Q_EMIT colorChanged(after);
    }
}

KColor KColorPicker::color() const { return currentColor(); }

void KColorPicker::setColorModel(const ColorModel model) {
    if (model_ == model) {
        return;
    }
    model_ = model;
    for (int index = 0; index < modelSelector_->count(); ++index) {
        if (modelSelector_->itemData(index).toInt() == static_cast<int>(model)) {
            modelSelector_->setCurrentIndex(index);
            break;
        }
    }
    // setCurrentIndex() above already ran onModelChanged() via the signal when the index actually
    // moved; rebuild directly too so a caller that sets the same index a dropdown already shows
    // (nothing to signal) still gets fields relabeled for the new model.
    rebuildChannelFields();
}

ColorModel KColorPicker::colorModel() const noexcept { return model_; }

void KColorPicker::setPickerForm(const PickerForm form) {
    if (!pickerFormAvailable(form)) {
        return;
    }
    form_ = form;
    for (int index = 0; index < formSelector_->count(); ++index) {
        if (formSelector_->itemData(index).toInt() == static_cast<int>(form)) {
            formSelector_->setCurrentIndex(index);
            break;
        }
    }
}

PickerForm KColorPicker::pickerForm() const noexcept { return form_; }

void KColorPicker::setRecentColorStore(KRecentColorStore* store) {
    swatchRow_->setRecentColorStore(store != nullptr ? store : &defaultRecentColorStore_);
}

KRecentColorStore* KColorPicker::recentColorStore() const noexcept {
    return swatchRow_->recentColorStore();
}

void KColorPicker::openBelow(const QWidget& anchor) {
    if ((windowFlags() & Qt::Popup) == 0) {
        setParent(nullptr);
        setWindowFlags(Qt::Popup);
        applyElevation(*this, Elevation::Dialog);
    }
    const QPoint anchorBottomLeft = anchor.mapToGlobal(QPoint(0, anchor.height()));
    move(anchorBottomLeft + QPoint(0, px(Spacing::XXS)));
    show();
    raise();
    activateWindow();
}

QRectF KColorPicker::svSquareRect() const noexcept { return svSquareRect_; }
QRectF KColorPicker::hueBarRect() const noexcept { return hueBarRect_; }
QRectF KColorPicker::alphaBarRect() const noexcept { return alphaBarRect_; }

QPointF KColorPicker::svMarkerPosition() const noexcept {
    return {svSquareRect_.left() + static_cast<qreal>(saturation_) * svSquareRect_.width(),
           svSquareRect_.top() + static_cast<qreal>(1.0F - value_) * svSquareRect_.height()};
}

qreal KColorPicker::huePosition() const noexcept { return static_cast<qreal>(hue_) / 360.0; }

qreal KColorPicker::alphaPosition() const noexcept { return static_cast<qreal>(alpha_); }

KDropdown* KColorPicker::formSelector() const noexcept { return formSelector_; }
KDropdown* KColorPicker::modelSelector() const noexcept { return modelSelector_; }

KValueField* KColorPicker::channelField(const int index) const {
    if (index < 0 || index >= static_cast<int>(channelFields_.size())) {
        return nullptr;
    }
    return channelFields_[static_cast<std::size_t>(index)];
}

QLineEdit* KColorPicker::hexField() const noexcept { return hexField_; }
KColorSwatchRow* KColorPicker::swatchRow() const noexcept { return swatchRow_; }

QSize KColorPicker::sizeHint() const { return size(); }
QSize KColorPicker::minimumSizeHint() const { return size(); }

void KColorPicker::onModelChanged(const int index) {
    const QVariant data = modelSelector_->itemData(index);
    if (!data.isValid()) {
        return;
    }
    model_ = static_cast<ColorModel>(data.toInt());
    rebuildChannelFields();
}

void KColorPicker::onChannelFieldEdited() {
    if (updatingChrome_) {
        return;
    }
    switch (model_) {
    case ColorModel::Hsva: {
        const auto h = static_cast<float>(channelFields_[0]->value());
        const auto s = static_cast<float>(channelFields_[1]->value()) / 100.0F;
        const auto v = static_cast<float>(channelFields_[2]->value()) / 100.0F;
        const auto a = static_cast<float>(channelFields_[3]->value()) / 100.0F;
        applyHsva(h, s, v, a);
        break;
    }
    case ColorModel::Hsla: {
        const auto h = static_cast<float>(channelFields_[0]->value());
        const auto s = static_cast<float>(channelFields_[1]->value()) / 100.0F;
        const auto l = static_cast<float>(channelFields_[2]->value()) / 100.0F;
        const auto a = static_cast<float>(channelFields_[3]->value()) / 100.0F;
        const auto hsva = KColor::fromHsla(h, s, l, a).toHsva();
        applyHsva(hsva[0], hsva[1], hsva[2], hsva[3]);
        break;
    }
    case ColorModel::Rgba: {
        const auto r = static_cast<float>(channelFields_[0]->value()) / 255.0F;
        const auto g = static_cast<float>(channelFields_[1]->value()) / 255.0F;
        const auto b = static_cast<float>(channelFields_[2]->value()) / 255.0F;
        const auto a = static_cast<float>(channelFields_[3]->value()) / 255.0F;
        const auto hsva = KColor::fromRgba(r, g, b, a).toHsva();
        applyHsva(hsva[0], hsva[1], hsva[2], hsva[3]);
        break;
    }
    }
    commitToRecents();
}

void KColorPicker::onHexFieldEdited() {
    const auto parsed = KColor::fromHex(hexField_->text(), alpha_);
    if (!parsed.has_value()) {
        // An unparsable entry reverts to the last valid text rather than silently doing nothing:
        // the artist gets their eye told the edit did not take, instead of a field that looks
        // committed but is not.
        refreshHexField();
        return;
    }
    const auto hsva = parsed->toHsva();
    applyHsva(hsva[0], hsva[1], hsva[2], hsva[3]);
    commitToRecents();
}

void KColorPicker::onSwatchActivated(const KColor& swatchColor) {
    setColor(swatchColor);
    commitToRecents();
}

void KColorPicker::commitToRecents() {
    swatchRow_->setCurrentColor(currentColor());
    if (KRecentColorStore* store = swatchRow_->recentColorStore(); store != nullptr) {
        store->push(currentColor());
    }
}

void KColorPicker::applyHsva(const float hue, const float saturation, const float value,
                             const float alpha) {
    const KColor before = currentColor();
    float normalizedHue = std::fmod(hue, 360.0F);
    if (normalizedHue < 0.0F) {
        normalizedHue += 360.0F;
    }
    hue_ = normalizedHue;
    saturation_ = std::clamp(saturation, 0.0F, 1.0F);
    value_ = std::clamp(value, 0.0F, 1.0F);
    alpha_ = std::clamp(alpha, 0.0F, 1.0F);
    refreshChannelFields();
    refreshHexField();
    refreshSwatchPreview();
    update();
    const KColor after = currentColor();
    if (!(before == after)) {
        Q_EMIT colorChanged(after);
    }
}

void KColorPicker::updateDragFromPoint(const QPointF& point) {
    switch (dragRegion_) {
    case DragRegion::SvSquare: {
        const qreal sx =
            std::clamp((point.x() - svSquareRect_.left()) / svSquareRect_.width(), 0.0, 1.0);
        const qreal sy =
            std::clamp((point.y() - svSquareRect_.top()) / svSquareRect_.height(), 0.0, 1.0);
        applyHsva(hue_, static_cast<float>(sx), static_cast<float>(1.0 - sy), alpha_);
        break;
    }
    case DragRegion::Hue: {
        const qreal t =
            std::clamp((point.y() - hueBarRect_.top()) / hueBarRect_.height(), 0.0, 1.0);
        applyHsva(static_cast<float>(t * 360.0), saturation_, value_, alpha_);
        break;
    }
    case DragRegion::Alpha: {
        const qreal t =
            std::clamp((point.x() - alphaBarRect_.left()) / alphaBarRect_.width(), 0.0, 1.0);
        applyHsva(hue_, saturation_, value_, static_cast<float>(t));
        break;
    }
    case DragRegion::None:
        break;
    }
}

void KColorPicker::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || !isEnabled()) {
        QWidget::mousePressEvent(event);
        return;
    }
    const QPointF pos = event->position();
    if (svSquareRect_.contains(pos)) {
        dragRegion_ = DragRegion::SvSquare;
    } else if (hueBarRect_.contains(pos)) {
        dragRegion_ = DragRegion::Hue;
    } else if (alphaBarRect_.contains(pos)) {
        dragRegion_ = DragRegion::Alpha;
    } else {
        QWidget::mousePressEvent(event);
        return;
    }
    updateDragFromPoint(pos);
    event->accept();
}

void KColorPicker::mouseMoveEvent(QMouseEvent* event) {
    if (dragRegion_ == DragRegion::None) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    updateDragFromPoint(event->position());
    event->accept();
}

void KColorPicker::mouseReleaseEvent(QMouseEvent* event) {
    if (dragRegion_ != DragRegion::None) {
        dragRegion_ = DragRegion::None;
        commitToRecents();
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void KColorPicker::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event)
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    fillRoundedSurface(painter, QRectF(rect()), tokenColor(Color::SurfaceRaised),
                       tokenColor(Color::Border), Radius::Medium);

    // SV square: a solid hue fill, a white-to-transparent gradient left to right, and a
    // transparent-to-black gradient top to bottom -- the standard three-layer composition, so the
    // corners land exactly on white/hue/black/full-shade without sampling a synthesized bitmap.
    const QColor hueColor = KColor::fromHsva(hue_, 1.0F, 1.0F).toQColor();
    painter.fillRect(svSquareRect_, hueColor);
    QLinearGradient whiteFade(svSquareRect_.topLeft(), svSquareRect_.topRight());
    whiteFade.setColorAt(0.0, QColor(255, 255, 255, 255));
    whiteFade.setColorAt(1.0, QColor(255, 255, 255, 0));
    painter.fillRect(svSquareRect_, whiteFade);
    QLinearGradient blackFade(svSquareRect_.topLeft(), svSquareRect_.bottomLeft());
    blackFade.setColorAt(0.0, QColor(0, 0, 0, 0));
    blackFade.setColorAt(1.0, QColor(0, 0, 0, 255));
    painter.fillRect(svSquareRect_, blackFade);
    strokeHairline(painter, svSquareRect_);
    paintMarkerDot(painter, svMarkerPosition());

    // Hue bar: the full spectrum top to bottom, stopped at every 60-degree primary/secondary so
    // the gradient is exact rather than merely close.
    QLinearGradient hueGradient(hueBarRect_.topLeft(), hueBarRect_.bottomLeft());
    for (int stop = 0; stop <= 6; ++stop) {
        const float stopHue = static_cast<float>(stop) * 60.0F;
        hueGradient.setColorAt(static_cast<qreal>(stop) / 6.0,
                               KColor::fromHsva(stopHue, 1.0F, 1.0F).toQColor());
    }
    painter.fillRect(hueBarRect_, hueGradient);
    strokeHairline(painter, hueBarRect_);
    {
        const qreal y = hueBarRect_.top() + huePosition() * hueBarRect_.height();
        painter.save();
        QPen pen(tokenColor(Color::Foreground));
        pen.setWidthF(kFocusRingWidth);
        painter.setPen(pen);
        painter.drawLine(QPointF(hueBarRect_.left(), y), QPointF(hueBarRect_.right(), y));
        painter.restore();
    }

    // Alpha bar: the checkerboard ground, then a transparent-to-opaque fade of the fully-opaque
    // current hue/sat/val -- alpha itself is what the gradient sweeps, not the RGB underneath it.
    drawAlphaCheckerboard(painter, alphaBarRect_, checkerCellPx(), Radius::Small);
    const QColor opaqueCurrent = KColor::fromHsva(hue_, saturation_, value_, 1.0F).toQColor();
    QColor faded = opaqueCurrent;
    faded.setAlpha(0);
    QLinearGradient alphaGradient(alphaBarRect_.topLeft(), alphaBarRect_.topRight());
    alphaGradient.setColorAt(0.0, faded);
    alphaGradient.setColorAt(1.0, opaqueCurrent);
    painter.fillRect(alphaBarRect_, alphaGradient);
    strokeHairline(painter, alphaBarRect_);
    {
        const qreal x = alphaBarRect_.left() + alphaPosition() * alphaBarRect_.width();
        painter.save();
        QPen pen(tokenColor(Color::Foreground));
        pen.setWidthF(kFocusRingWidth);
        painter.setPen(pen);
        painter.drawLine(QPointF(x, alphaBarRect_.top()), QPointF(x, alphaBarRect_.bottom()));
        painter.restore();
    }
}

} // namespace bloom::ui::kit
