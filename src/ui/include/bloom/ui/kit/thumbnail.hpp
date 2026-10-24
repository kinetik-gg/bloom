#pragma once

#include <QPixmap>
#include <QWidget>

namespace bloom::ui::kit {

// A read-only picture cell: the Size::ImageThumbnail square on a SurfaceSunken background, showing
// a cached image proxy (or a glyph) centred and fitted without changing its aspect ratio. It shows
// what a worker already decoded; it never loads or decodes anything itself. The Assets selection
// header is its first user (#378); a node card's 72-pixel body cell is the same contract drawn
// inside the node scene.
class KThumbnail final : public QWidget {
    Q_OBJECT

  public:
    explicit KThumbnail(QWidget* parent = nullptr);
    void setPixmap(const QPixmap& pixmap);
    [[nodiscard]] const QPixmap& pixmap() const noexcept { return pixmap_; }
    [[nodiscard]] QSize sizeHint() const override;

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    QPixmap pixmap_;
};

} // namespace bloom::ui::kit
