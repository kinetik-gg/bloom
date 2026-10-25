#pragma once

#include <bloom/ui/kit/tokens.hpp>

#include <QRectF>
#include <QSize>
#include <QVariantAnimation>
#include <QWidget>

#include <cstdint>

namespace bloom::ui::kit {

// A read-only progress bar (#377): an Accent fill over a rounded Field track, the KSlider track
// recipe without a handle, Size::ProgressTrack tall. A determinate bar fills completed / total.
// Busy mode (setBusy(), or a total of 0) is for work whose size is not known yet: a third-width
// segment sweeps back and forth, held centred when motion is reduced, so a busy bar is never an
// empty, static track. It never takes focus and never reports anything but what it was given.
class KProgressBar final : public QWidget {
    Q_OBJECT

  public:
    explicit KProgressBar(QWidget* parent = nullptr);

    // A total of zero is busy; completed is clamped to the total.
    void setProgress(std::uint64_t completed, std::uint64_t total);
    void setBusy() { setProgress(0, 0); }
    [[nodiscard]] bool busy() const noexcept { return total_ == 0; }
    // True while the busy segment is animating (busy, visible, motion enabled).
    [[nodiscard]] bool sweeping() const noexcept;
    [[nodiscard]] std::uint64_t completed() const noexcept { return completed_; }
    [[nodiscard]] std::uint64_t total() const noexcept { return total_; }
    [[nodiscard]] bool indeterminate() const noexcept { return total_ == 0; }
    // How full a determinate bar is, 0..1; zero while indeterminate.
    [[nodiscard]] double fraction() const noexcept;

    [[nodiscard]] QRectF trackRect() const;
    [[nodiscard]] QRectF fillRect() const;

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

  protected:
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

  private:
    void updateSweep();

    std::uint64_t completed_ = 0;
    std::uint64_t total_ = 0;
    double sweep_ = 0.5;
    QVariantAnimation animation_;
};

} // namespace bloom::ui::kit
