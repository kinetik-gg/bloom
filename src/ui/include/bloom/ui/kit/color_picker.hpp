#pragma once

#include <bloom/ui/kit/color.hpp>
#include <bloom/ui/kit/color_swatches.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <QPointF>
#include <QRectF>
#include <QSize>
#include <QWidget>

#include <array>
#include <cstdint>

class QLineEdit;

namespace bloom::ui::kit {

class KDropdown;
class KValueField;

// Task U6 (issue #121), decision 3: the picker popup. Version 1 ships PickerForm::Square in full --
// an SV square, a hue bar, and an alpha bar, each with a draggable marker -- plus a color-model
// dropdown that switches what the four KValueField rows below mean (HSVA/HSLA/RGBA), a hex field,
// and the decision-4 swatch row. The other PickerForm members are architecture-present: see
// color.hpp's pickerFormAvailable() and this class's formSelector(), which lists only the forms
// that are actually available rather than showing a disabled placeholder for the rest.
//
// Internally the picker is authoritative in HSVA, not in KColor's RGBA: a saturation/value drag at
// zero saturation (a gray) has no hue information to recover from RGB alone (color.hpp's
// hueFromRgb() reports 0 there by convention), so the picker keeps its own live hue rather than
// re-deriving it from KColor on every paint. That is what lets an SV-square drag "preserve hue"
// exactly through a gray, and it is a picker-session concern, not a value-model one -- KColor
// itself stays a pure, memoryless conversion type.
class KColorPicker final : public QWidget {
    Q_OBJECT

  public:
    explicit KColorPicker(QWidget* parent = nullptr);
    ~KColorPicker() override;

    void setColor(const KColor& color);
    [[nodiscard]] KColor color() const;

    void setColorModel(ColorModel model);
    [[nodiscard]] ColorModel colorModel() const noexcept;

    // Ignored (a no-op) for a form pickerFormAvailable() reports false for.
    void setPickerForm(PickerForm form);
    [[nodiscard]] PickerForm pickerForm() const noexcept;

    // Not owned. Shared with the swatch row so an artist's recent colors survive this popup
    // closing and a sibling chip's popup opening. Pass nullptr to fall back to the row's own
    // default store.
    void setRecentColorStore(KRecentColorStore* store);
    [[nodiscard]] KRecentColorStore* recentColorStore() const noexcept;

    // Opens as a top-level Qt::Popup positioned below `anchor`, SurfaceRaised + Elevation::Dialog
    // per decision 3. The same popup instance is reused across opens (KColorChip owns one).
    void openBelow(const QWidget& anchor);

    [[nodiscard]] QRectF svSquareRect() const noexcept;
    [[nodiscard]] QRectF hueBarRect() const noexcept;
    [[nodiscard]] QRectF alphaBarRect() const noexcept;

    // Where the SV marker sits (within svSquareRect()) and where the hue/alpha markers sit as a
    // 0..1 fraction along their bars -- exposed so a test can assert the drag geometry directly
    // rather than re-deriving it from paint output.
    [[nodiscard]] QPointF svMarkerPosition() const noexcept;
    [[nodiscard]] qreal huePosition() const noexcept;
    [[nodiscard]] qreal alphaPosition() const noexcept;

    [[nodiscard]] KDropdown* formSelector() const noexcept;
    [[nodiscard]] KDropdown* modelSelector() const noexcept;
    // index in [0, 3]; which channel each index names depends on colorModel().
    [[nodiscard]] KValueField* channelField(int index) const;
    [[nodiscard]] QLineEdit* hexField() const noexcept;
    [[nodiscard]] KColorSwatchRow* swatchRow() const noexcept;

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

  Q_SIGNALS:
    // Emitted for every committed change: a drag step, a channel field edit, a hex entry, a
    // swatch activation. There is no separate "live preview" signal -- direct editor feedback is
    // never eased or throttled (visual-language.md's Motion::None rule), so the value the signal
    // carries is always exactly where the pointer or field is right now.
    void colorChanged(const KColor& color);

  protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

  private:
    enum class DragRegion : std::uint8_t { None, SvSquare, Hue, Alpha };

    void layoutRegions();
    void buildChrome();
    void rebuildFormSelector();
    void rebuildChannelFields();
    void refreshChannelFields();
    void refreshHexField();
    void refreshSwatchPreview();
    void applyHsva(float hue, float saturation, float value, float alpha);
    [[nodiscard]] KColor currentColor() const;
    void onChannelFieldEdited();
    void onHexFieldEdited();
    void onModelChanged(int index);
    void onSwatchActivated(const KColor& color);
    void updateDragFromPoint(const QPointF& point);
    // Pushes the current color into the bound recent-color store and refreshes the swatch row's
    // "what would 'set' write here" preview. Called only at discrete commit points (a drag
    // release, a finished field/hex edit) -- never every drag frame, or scrubbing would flood the
    // MRU list with in-between values nobody asked to remember.
    void commitToRecents();

    float hue_ = 0.0F;
    float saturation_ = 0.0F;
    float value_ = 1.0F;
    float alpha_ = 1.0F;

    ColorModel model_ = ColorModel::Hsva;
    PickerForm form_ = PickerForm::Square;
    DragRegion dragRegion_ = DragRegion::None;

    QRectF svSquareRect_;
    QRectF hueBarRect_;
    QRectF alphaBarRect_;

    KDropdown* formSelector_ = nullptr;
    KDropdown* modelSelector_ = nullptr;
    std::array<KValueField*, 4> channelFields_{};
    QLineEdit* hexField_ = nullptr;
    KColorSwatchRow* swatchRow_ = nullptr;
    // Owned default so the swatch row always has somewhere to read/write recents even when no
    // caller injects a shared one (setRecentColorStore()).
    KRecentColorStore defaultRecentColorStore_;
    bool updatingChrome_ = false;
};

} // namespace bloom::ui::kit
