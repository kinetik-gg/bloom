#pragma once
#include <QEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWidget>
#include <bloom/ui/kit/icons.hpp>
#include <bloom/ui/kit/painting.hpp>
namespace bloom::ui::kit {
// The rounded frame of a panel, painted LAST. One transparent overlay the size of the panel that
// (a) fills the four wedges outside the Radius::Panel curve with Color::Background -- the one
// colour every rounded corner reveals -- so a header, footer or content widget's square corner
// can never show past the curve, and (b) draws the hairline border along the curve on top of
// every child, so the arc is never cut by a child painted after it. It watches its panel: a
// resize re-fits it and any child added later is stacked beneath it again (the header and footer
// are built by the chrome rebuild, after the panel's constructor -- exactly what let four
// small corner masks fall behind them; owner, 2026-09-15: "I asked you to fix this multiple
// times but no results so far").
class KPanelFrame final : public QWidget {
  public:
    explicit KPanelFrame(QWidget* panel);
    void setActive(bool active);
    [[nodiscard]] static int radiusPx() noexcept;

  protected:
    void paintEvent(QPaintEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void fit();
    QWidget* panel_;
    bool active_ = false;
};
class KDiamond : public QWidget {
  public:
    using QWidget::QWidget;
    enum class Fill { None, Half, Full };
    void setIndicator(bool animated, Fill fill) {
        animated_ = animated;
        fill_ = fill;
        update();
    }

  protected:
    void paintEvent(QPaintEvent*) override;

  private:
    bool animated_ = false;
    Fill fill_ = Fill::None;
};
class KAnchorGrid : public QWidget {
  public:
    explicit KAnchorGrid(QWidget* parent = nullptr);
    virtual int selectedPoint() const = 0;
    QRect pointRect(int index) const;

  protected:
    void paintEvent(QPaintEvent*) override;
};
class KListSurface : public QWidget {
  public:
    using QWidget::QWidget;
    void setGridOffset(int offset) {
        offset_ = offset;
        update();
    }

  protected:
    void paintEvent(QPaintEvent*) override;

  private:
    int offset_ = 0;
};
class KSurface : public QWidget {
  public:
    explicit KSurface(QWidget* parent = nullptr);
};
} // namespace bloom::ui::kit
