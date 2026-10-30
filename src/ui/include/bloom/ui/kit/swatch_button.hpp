#pragma once

#include <bloom/ui/kit/controls.hpp>

#include <QColor>
#include <QIcon>

namespace bloom::ui::kit {

// A colour shown as a small square: a Radius::Small square of Size::IconSmall in the colour, with
// the colour's own edge ink as a hairline so a dark label still reads on a dark plate.
//
// KSwatchButton is the one control look (Secondary plate, hover rung, focus border) carrying such
// a square instead of a glyph -- the timeline's label cell. It never tints the square the way an
// icon button tints its glyph. swatchIcon() is the same square for a menu row's icon column,
// painted at the painter's own device pixel ratio so it stays crisp at 1.5x.
class KSwatchButton final : public KIconButton {
    Q_OBJECT
  public:
    explicit KSwatchButton(QWidget* parent = nullptr);
    void setSwatch(const QColor& color);
    [[nodiscard]] QColor swatch() const noexcept { return swatch_; }

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    QColor swatch_;
};

[[nodiscard]] QIcon swatchIcon(const QColor& color);

// Paints the swatch square centred in `bounds`.
void paintSwatch(QPainter& painter, const QRectF& bounds, const QColor& color);

} // namespace bloom::ui::kit
