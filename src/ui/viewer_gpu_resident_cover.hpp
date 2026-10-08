#pragma once

// Private output-only CPU cover. Its raster QWindow owns a QBackingStore rather than sharing the
// decorated parent's QWidget backing store: Qt Wayland's native-widget child flush crops that raw
// parent buffer without its decoration margins. An independent content buffer avoids displaced
// pixels, copied chrome and incomplete cover extents using public portable Qt APIs.

#include <QBackingStore>
#include <QEvent>
#include <QExposeEvent>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QResizeEvent>
#include <QShowEvent>
#include <QString>
#include <QSurfaceFormat>
#include <QWidget>
#include <QWindow>

#include <cstddef>

#include <bloom/ui/kit/tokens.hpp>

namespace bloom::ui {

// Alien host whose only widget child is the CPU-cover wrapper.
//
// Qt's QWidget::setAttribute(WA_NativeWindow) calls parentWidget()->
// d_func()->enforceNativeChildren(), which marks EVERY child of that parent
// native (and every child added later), unless the application-wide
// Qt::AA_DontCreateNativeWidgetSiblings is set. Parenting the native cover
// directly to the editor therefore promotes every editor child to a native
// Wayland subsurface. Because a native QWidget is flushed through its parent's
// SHM backing store, and the first flush of each newly promoted child is a fresh
// "wayland-shm" memfd, that promotion multiplied the resident footprint.
//
// The former native-QWidget cover used this host to fence that promotion. The current alien
// wrapper instead owns an independent raster QWindow, parented to the GPU QWindow while a target
// exists. It covers its native parent instead of depending on undefined overlapping container
// stacking. Neither wrapper nor host enables WA_NativeWindow; editor siblings stay alien.
class ViewerGpuCpuCoverHost final : public QWidget {
  public:
    explicit ViewerGpuCpuCoverHost(QWidget* parent) : QWidget(parent) {
        setObjectName(QStringLiteral("bloomViewerGpuCpuCoverHost"));
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
        setAttribute(Qt::WA_NoSystemBackground, true);
        setFocusPolicy(Qt::NoFocus);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    }
};

class ViewerGpuCpuCoverWindow final : public QWindow {
  public:
    explicit ViewerGpuCpuCoverWindow(QWindow* parent) : QWindow(parent), backingStore_(this) {
        setObjectName(QStringLiteral("bloomViewerGpuCpuCoverWindow"));
        setSurfaceType(QSurface::RasterSurface);
        QSurfaceFormat format;
        format.setAlphaBufferSize(8);
        setFormat(format);
        setFlags(Qt::SubWindow | Qt::FramelessWindowHint | Qt::WindowTransparentForInput |
                 Qt::WindowDoesNotAcceptFocus);
    }
    ~ViewerGpuCpuCoverWindow() override { destroy(); }

    void setSnapshot(const QPixmap& snapshot) {
        if (snapshot_.cacheKey() == snapshot.cacheKey())
            return;
        snapshot_ = snapshot; // implicitly shared; no new capture or pixel processing
        requestUpdate();
    }
    void setCovered(bool covered) {
        if (covered_ == covered)
            return;
        covered_ = covered;
        requestUpdate();
    }
    [[nodiscard]] QPixmap snapshot() const { return snapshot_; }
    [[nodiscard]] std::size_t paintCount() const noexcept { return paintCount_; }

  protected:
    bool event(QEvent* event) override {
        if (event->type() == QEvent::UpdateRequest) {
            paintNow();
            return true;
        }
        if (event->type() == QEvent::DevicePixelRatioChange)
            requestUpdate();
        return QWindow::event(event);
    }
    void exposeEvent(QExposeEvent*) override {
        if (isExposed() && !wasExposed_)
            forcePaint_ = true;
        wasExposed_ = isExposed();
        requestUpdate();
    }
    void resizeEvent(QResizeEvent*) override { requestUpdate(); }

  private:
    void paintNow() {
        if (!isExposed() || size().isEmpty())
            return;
        if (!forcePaint_ && paintedSize_ == size() && paintedRatio_ == devicePixelRatio() &&
            paintedCovered_ == covered_ &&
            (!covered_ || paintedSnapshotKey_ == snapshot_.cacheKey()))
            return;
        const QRect rect(QPoint{}, size());
        backingStore_.resize(size());
        backingStore_.beginPaint(rect);
        QPainter painter(backingStore_.paintDevice());
        if (!painter.isActive()) {
            backingStore_.endPaint();
            return;
        }
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.fillRect(rect, covered_ ? kit::color(kit::Color::Canvas) : QColor(Qt::transparent));
        if (covered_ && !snapshot_.isNull()) {
            painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
            painter.drawPixmap(rect, snapshot_);
        }
        painter.end();
        backingStore_.endPaint();
        backingStore_.flush(rect);
        ++paintCount_;
        paintedSize_ = size();
        paintedRatio_ = devicePixelRatio();
        paintedCovered_ = covered_;
        paintedSnapshotKey_ = snapshot_.cacheKey();
        forcePaint_ = false;
    }
    QBackingStore backingStore_;
    QPixmap snapshot_;
    std::size_t paintCount_ = 0;
    bool covered_ = true;
    bool wasExposed_ = false;
    bool forcePaint_ = true;
    QSize paintedSize_;
    double paintedRatio_ = 0;
    bool paintedCovered_ = false;
    qint64 paintedSnapshotKey_ = 0;
};

class ViewerGpuCpuCover final : public QWidget {
  public:
    explicit ViewerGpuCpuCover(QWidget* parent) : QWidget(parent) {
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
        setFocusPolicy(Qt::NoFocus);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    }
    ~ViewerGpuCpuCover() override {
        delete initialCover_.data();
        delete window_.data();
    }

    // Only the raster child is replaced. Its GPU parent remains owned/mapped by the presenter;
    // QPointer also clears if a genuinely retired parent deletes its children before this wrapper.
    [[nodiscard]] bool bindNativeParent(QWindow* gpuParent) {
        if (window_ && gpuParent_ == gpuParent) {
            syncNativeGeometry();
            return false;
        }
        if (gpuParent_)
            gpuParent_->removeEventFilter(this);
        delete initialCover_.data();
        delete window_.data();
        gpuParent_ = gpuParent;
        static_cast<void>(this->window()->winId());
        QWindow* topLevel = this->window()->windowHandle();
        window_ = new ViewerGpuCpuCoverWindow(gpuParent ? gpuParent : topLevel);
        // A Vulkan parent without its first buffer cannot display a subsurface. This temporary
        // top-level child covers first attach; both windows share the same snapshot. The GPU child
        // is already established when that first parent commit arrives, then stays mapped for the
        // target's lifetime so later refusal needs only its own desynchronized raster commit.
        if (gpuParent)
            initialCover_ = new ViewerGpuCpuCoverWindow(topLevel);
        if (gpuParent_)
            gpuParent_->installEventFilter(this);
        syncNativeGeometry();
        window_->setSnapshot(snapshot_);
        if (initialCover_)
            initialCover_->setSnapshot(snapshot_);
        if (isVisible()) {
            window_->show();
            if (initialCover_)
                initialCover_->show();
        }
        return true;
    }
    void setSnapshot(const QPixmap& snapshot) {
        snapshot_ = snapshot;
        if (window_)
            window_->setSnapshot(snapshot);
        if (initialCover_)
            initialCover_->setSnapshot(snapshot);
    }
    [[nodiscard]] QPixmap snapshot() const { return snapshot_; }
    [[nodiscard]] QWindow* nativeWindow() const noexcept {
        return initialCover_ ? initialCover_.data() : window_.data();
    }
    [[nodiscard]] QWindow* persistentWindow() const noexcept { return window_; }
    [[nodiscard]] std::size_t paintCount() const noexcept {
        return window_ ? window_->paintCount() : 0;
    }

  protected:
    void showEvent(QShowEvent*) override {
        syncNativeGeometry();
        if (window_) {
            window_->setCovered(true);
            window_->show();
        }
        if (initialCover_)
            initialCover_->show();
    }
    void hideEvent(QHideEvent*) override {
        if (gpuParent_) {
            // Hide only the temporary attach surface. Conceal the persistent child by transparent
            // pixels: destroying/recreating its role requires another parent commit, which a
            // refused/idle GPU target may never produce.
            delete initialCover_.data();
            if (window_)
                window_->setCovered(false);
        } else {
            if (window_)
                window_->hide();
            if (initialCover_)
                initialCover_->hide();
        }
    }
    void resizeEvent(QResizeEvent*) override { syncNativeGeometry(); }
    void moveEvent(QMoveEvent*) override { syncNativeGeometry(); }
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == gpuParent_ && event->type() == QEvent::Resize)
            syncNativeGeometry();
        return QWidget::eventFilter(watched, event);
    }

  private:
    void syncNativeGeometry() {
        if (!window_)
            return;
        const QRect target = gpuParent_ ? QRect(QPoint{}, gpuParent_->size())
                                        : QRect(mapTo(this->window(), QPoint{}), size());
        if (window_->geometry() != target)
            window_->setGeometry(target);
        if (initialCover_) {
            const QRect initialTarget(mapTo(this->window(), QPoint{}), size());
            if (initialCover_->geometry() != initialTarget)
                initialCover_->setGeometry(initialTarget);
        }
    }
    QPointer<ViewerGpuCpuCoverWindow> window_;
    QPointer<ViewerGpuCpuCoverWindow> initialCover_;
    QPointer<QWindow> gpuParent_;
    QPixmap snapshot_;
};

} // namespace bloom::ui
