#pragma once

#include <QRectF>
#include <QWidget>

namespace bloom::core {
class PixelAspectRatio;
}

namespace bloom::render {
class ImageExtent;
}

namespace bloom::ui {

class CompositionPreviewController;
class CompositionSession;

[[nodiscard]] QRectF fitDisplayRect(const QRectF& available, render::ImageExtent extent,
                                    core::PixelAspectRatio pixelAspect) noexcept;

class ViewerEditor final : public QWidget {
    Q_OBJECT

  public:
    ViewerEditor(CompositionSession& session, CompositionPreviewController& previewController,
                 QWidget* parent = nullptr);

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    void updatePreviewAccessibility();

    CompositionSession& session_;
    CompositionPreviewController& previewController_;
};

} // namespace bloom::ui
