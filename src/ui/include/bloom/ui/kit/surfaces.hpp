#pragma once
#include <QEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWidget>
#include <bloom/ui/kit/icons.hpp>
#include <bloom/ui/kit/painting.hpp>
#include <vector>
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
        if (animated_ == animated && fill_ == fill)
            return;
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
// The 3 x 3 anchor-point grid: nine round dots, the selected one filled. A click on a dot or the
// arrow keys choose a point (0..8, row by row from top left); a subclass says which point is
// selected and what choosing one does. Properties' Anchor Point and the Composition dialog's
// resize anchor are the two users.
class KAnchorGrid : public QWidget {
  public:
    explicit KAnchorGrid(QWidget* parent = nullptr);
    virtual int selectedPoint() const = 0;
    virtual void choosePoint(int index) = 0;
    QRect pointRect(int index) const;

  protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
};
class KListSurface : public QWidget {
  public:
    using QWidget::QWidget;
    void setGridOffset(int offset) {
        offset_ = offset;
        update();
    }
    // The rows this surface paints in the list hover step and in the selected-row recipe (by
    // row index from the top of the list, -1 for none). A child row widget that paints nothing
    // of its own shows them through; KRow paints the same two states itself.
    void setRowStates(int hovered, std::vector<int> selected) {
        hovered_ = hovered;
        selected_ = std::move(selected);
        update();
    }
    [[nodiscard]] int hoveredRow() const noexcept { return hovered_; }
    [[nodiscard]] const std::vector<int>& selectedRows() const noexcept { return selected_; }

  protected:
    void paintEvent(QPaintEvent*) override;

  private:
    int offset_ = 0;
    int hovered_ = -1;
    std::vector<int> selected_;
};
// A plain widget filled with one colour token: neutral panel surfaces by default, or a marker
// such as an Accent insertion line. The one way product code fills a widget background.
class KSurface : public QWidget {
  public:
    explicit KSurface(QWidget* parent = nullptr, Color fill = Color::Surface);
    void setFill(Color fill);
};
} // namespace bloom::ui::kit
