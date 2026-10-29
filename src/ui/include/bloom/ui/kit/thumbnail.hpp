#pragma once

#include <QPixmap>
#include <QWidget>

#include <cstdint>

namespace bloom::ui::kit {

// A read-only picture cell: the Size::ImageThumbnail square on a SurfaceSunken background, showing
// a cached image proxy (or a glyph) centred and fitted without changing its aspect ratio. It shows
// what a worker already decoded; it never loads or decodes anything itself. The Assets selection
// header is its first user (#378); a node card's 72-pixel body cell is the same contract drawn
// inside the node scene.
//
// Two cells: Card, the 72-pixel square on SurfaceSunken; and Strip, the Assets summary strip's
// StripThumbnailWidth x StripThumbnailHeight Field well, which letterboxes the picture at its own
// aspect ratio so a 2.39:1 plate and a square still both read.
class KThumbnail final : public QWidget {
    Q_OBJECT

  public:
    enum class Cell : std::uint8_t { Card, Strip };
    explicit KThumbnail(QWidget* parent = nullptr, Cell cell = Cell::Card);
    [[nodiscard]] Cell cell() const noexcept { return cell_; }
    void setPixmap(const QPixmap& pixmap);
    [[nodiscard]] const QPixmap& pixmap() const noexcept { return pixmap_; }
    [[nodiscard]] QSize sizeHint() const override;

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    Cell cell_ = Cell::Card;
    QPixmap pixmap_;
};

} // namespace bloom::ui::kit
