#pragma once

#include <bloom/ui/kit/tokens.hpp>

#include <QRect>
#include <QSize>
#include <QString>
#include <QVector>
#include <QWidget>

class QStackedWidget;

namespace bloom::ui::kit {

// The kit tab bar (lane C, for the Composition dialog's Basic / Advanced, After Effects style).
// Tabs are Ui text in a row; the current tab is Foreground ink over a Size::SelectionEdge Accent
// underline, the others Muted (Foreground under the pointer). A tab can carry an alert: a small
// Error dot after its text, for a refused value the tab hides. Keyboard: the bar takes focus and
// shows the kit's keyboard-focus border around the current tab; Left/Right step, Home/End jump.
// Accessible as a page tab list with one page tab per tab.
class KTabBar final : public QWidget {
    Q_OBJECT

  public:
    explicit KTabBar(QWidget* parent = nullptr);

    int addTab(const QString& text);
    [[nodiscard]] int count() const noexcept { return static_cast<int>(tabs_.size()); }
    [[nodiscard]] QString tabText(int index) const;
    [[nodiscard]] int currentIndex() const noexcept { return current_; }
    void setCurrentIndex(int index);
    void setTabAlert(int index, bool alert);
    [[nodiscard]] bool tabAlert(int index) const;
    // The rectangle a tab occupies, in this widget's coordinates.
    [[nodiscard]] QRect tabRect(int index) const;

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

  Q_SIGNALS:
    void currentChanged(int index);

  protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void leaveEvent(QEvent* event) override;

  private:
    struct Tab {
        QString text;
        bool alert = false;
    };
    [[nodiscard]] int tabWidth(int index) const;
    [[nodiscard]] int tabAt(QPoint point) const;

    QVector<Tab> tabs_;
    int current_ = -1;
    int hovered_ = -1;
};

// A KTabBar over a page stack. The stack is as tall as its tallest page, so switching tabs never
// changes the size of what holds it. Ctrl+Tab / Ctrl+Shift+Tab from anywhere inside step the tab.
class KTabs final : public QWidget {
    Q_OBJECT

  public:
    explicit KTabs(QWidget* parent = nullptr);

    // Adds a page; returns its index. The page becomes a child of the stack.
    int addPage(const QString& title, QWidget* page);
    [[nodiscard]] KTabBar* bar() const noexcept { return bar_; }
    [[nodiscard]] QStackedWidget* stack() const noexcept { return stack_; }
    [[nodiscard]] int currentIndex() const;
    void setCurrentIndex(int index);
    // The index of the page holding `widget`, or -1.
    [[nodiscard]] int pageOf(const QWidget* widget) const;

  Q_SIGNALS:
    void currentChanged(int index);

  protected:
    void keyPressEvent(QKeyEvent* event) override;

  private:
    KTabBar* bar_ = nullptr;
    QStackedWidget* stack_ = nullptr;
};

} // namespace bloom::ui::kit
