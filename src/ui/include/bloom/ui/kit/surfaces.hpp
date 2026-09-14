#pragma once
#include <QPainter>
#include <QPainterPath>
#include <QWidget>
#include <bloom/ui/kit/icons.hpp>
#include <bloom/ui/kit/painting.hpp>
namespace bloom::ui::kit {
class PanelCornerMask final : public QWidget {
  public:
    enum class Corner : std::uint8_t { TopLeft, TopRight, BottomLeft, BottomRight };

    PanelCornerMask(const Corner corner, QWidget* parent) : QWidget(parent), corner_(corner) {
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
        setFocusPolicy(Qt::NoFocus);
        const int extent = kit::radiusPx(kit::Radius::Panel, 0);
        setFixedSize(extent, extent);
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const qreal r = width();
        QPointF center;
        switch (corner_) {
        case Corner::TopLeft:
            center = QPointF(r, r);
            break;
        case Corner::TopRight:
            center = QPointF(0.0, r);
            break;
        case Corner::BottomLeft:
            center = QPointF(r, 0.0);
            break;
        case Corner::BottomRight:
            center = QPointF(0.0, 0.0);
            break;
        }
        QPainterPath square;
        square.addRect(rect());
        QPainterPath arc;
        arc.addEllipse(center, r, r);
        painter.fillPath(square.subtracted(arc), kit::color(kit::Color::Background));
    }

  private:
    Corner corner_;
};
class KDiamond : public QWidget {
  public:
    using QWidget::QWidget;
    void setIndicator(bool animated, bool keyed) {
        animated_ = animated;
        keyed_ = keyed;
        update();
    }

  protected:
    void paintEvent(QPaintEvent*) override;

  private:
    bool animated_ = false, keyed_ = false;
};
class KAnchorGrid : public QWidget {
  public:
    explicit KAnchorGrid(QWidget* parent = nullptr);
    virtual int selectedPoint() const = 0;
    QRect pointRect(int index) const;

  protected:
    void paintEvent(QPaintEvent*) override;
};
class KSurface : public QWidget {
  public:
    explicit KSurface(QWidget* parent = nullptr);
};
} // namespace bloom::ui::kit
