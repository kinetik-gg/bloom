#pragma once

#include <bloom/ui/kit/color.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <QRectF>
#include <QSize>
#include <QWidget>

#include <cstdint>

namespace bloom::ui::kit {

class KColorPicker;

// Task U6 (issue #121), decision 2: the selector field every color-valued control ultimately
// composes. A Radius::Small swatch button that paints the alpha checkerboard under a translucent
// color and opens a self-contained KColorPicker popup on click -- the same "closed field owns its
// popup" shape KDropdown/KDropdownPopup already establish, so a chip behaves the way every other
// Kinetik field that opens something does.
class KColorChip final : public QWidget {
    Q_OBJECT

  public:
    enum class Shape : std::uint8_t { Square, Circle };
    enum class ControlSize : std::uint8_t { Compact, Default, Roomy };

    explicit KColorChip(QWidget* parent = nullptr);
    ~KColorChip() override;

    void setColor(const KColor& color);
    [[nodiscard]] KColor color() const noexcept;

    void setShape(Shape shape);
    [[nodiscard]] Shape shape() const noexcept;

    void setControlSize(ControlSize size);
    [[nodiscard]] ControlSize controlSize() const noexcept;

    // The popup this chip opens on click. Exists for the lifetime of the chip (lazily created on
    // first open) so a test can drive it directly rather than simulating a click-and-wait.
    [[nodiscard]] KColorPicker* picker();

    void openPicker();
    void closePicker();
    [[nodiscard]] bool isPickerOpen() const;

    [[nodiscard]] State visualState() const;

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

  Q_SIGNALS:
    void colorChanged(const KColor& color);

  protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void changeEvent(QEvent* event) override;

  private:
    void ensurePicker();
    [[nodiscard]] int controlExtent() const;

    KColor color_{};
    Shape shape_ = Shape::Square;
    ControlSize controlSize_ = ControlSize::Default;
    KColorPicker* picker_ = nullptr;
    bool hovered_ = false;
};

// Task U6, decision 2: two chips (A/B) and a swap control -- the foreground/background pairing
// pattern every paint-style tool needs. Owns its two KColorChip instances rather than composing
// them externally so "swap" can be one atomic operation a test can assert against directly.
class KColorABPair final : public QWidget {
    Q_OBJECT

  public:
    explicit KColorABPair(QWidget* parent = nullptr);

    void setColorA(const KColor& color);
    [[nodiscard]] KColor colorA() const noexcept;
    void setColorB(const KColor& color);
    [[nodiscard]] KColor colorB() const noexcept;

    void swap();

    [[nodiscard]] KColorChip* chipA() const noexcept;
    [[nodiscard]] KColorChip* chipB() const noexcept;
    [[nodiscard]] QRectF swapButtonRect() const;

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

  Q_SIGNALS:
    void colorAChanged(const KColor& color);
    void colorBChanged(const KColor& color);
    void swapped();

  protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

  private:
    void layoutChips();

    KColorChip* chipA_ = nullptr;
    KColorChip* chipB_ = nullptr;
};

} // namespace bloom::ui::kit
