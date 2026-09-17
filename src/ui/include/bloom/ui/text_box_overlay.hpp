#pragma once

#include <bloom/ui/kit/tokens.hpp>

#include <QPointF>
#include <QRectF>

#include <array>
#include <functional>
#include <optional>

class QPainter;

namespace bloom::ui {

enum class TextBoxHandle : unsigned char {
    TopLeft,
    Top,
    TopRight,
    Right,
    BottomRight,
    Bottom,
    BottomLeft,
    Left,
};

struct TextBoxOverlayGeometry final {
    QRectF box;
    std::array<QRectF, 8> handles{};
};

[[nodiscard]] TextBoxOverlayGeometry
textBoxOverlayGeometry(QRectF box, qreal handleSize = kit::px(kit::Size::PropertiesAnchorDot));
[[nodiscard]] std::optional<TextBoxHandle>
hitTestTextBoxHandle(const TextBoxOverlayGeometry& geometry, QPointF point);
void paintTextBoxOverlay(QPainter& painter, QRectF box, bool active = true);

// Session-side geometry for a text-box resize. The viewer owns the selected layer and supplies a
// commit callback, so this helper does not mutate document truth or depend on DM-1's viewer files.
// begin() captures the original rectangle; release() invokes the callback exactly once when the
// rectangle changed. Shift preserves the corner-drag aspect ratio and all handles snap to the
// composition edges within the supplied snap distance.
class TextBoxResizeInteraction final {
  public:
    using Commit = std::function<void(QRectF)>;

    void begin(QRectF box, TextBoxHandle handle, QPointF press, QRectF composition, Commit commit,
               bool shift = false, qreal snapDistance = 8.0);
    void update(QPointF cursor, bool shift = false);
    void release();
    void cancel() noexcept;

    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] QRectF box() const noexcept { return box_; }

  private:
    [[nodiscard]] QPointF snapped(QPointF point) const noexcept;
    [[nodiscard]] QRectF resized(QPointF cursor, bool shift) const noexcept;

    QRectF original_;
    QRectF box_;
    QRectF composition_;
    TextBoxHandle handle_ = TextBoxHandle::TopLeft;
    Commit commit_;
    qreal snapDistance_ = 8.0;
    bool active_ = false;
};

} // namespace bloom::ui
