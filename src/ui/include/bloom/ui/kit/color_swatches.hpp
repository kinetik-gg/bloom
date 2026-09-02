#pragma once

#include <bloom/ui/kit/color.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <QRectF>
#include <QSize>
#include <QWidget>

#include <optional>
#include <vector>

namespace bloom::ui::kit {

// Task U6 (issue #121), decision 4: the swatch row's recent-color memory. A plain C++ object, not
// a QObject -- "a small injectable store" so a KColorSwatchRow (or a future non-widget consumer)
// can share one MRU list, or a test can hand it a throwaway instance, without any Qt parent-child
// lifetime to manage. Session memory only: nothing here is written to disk, by design (decision 4
// explicitly defers settings persistence; see the report for the follow-up).
class KRecentColorStore final {
  public:
    static constexpr int kDefaultCapacity = 32;

    explicit KRecentColorStore(int capacity = kDefaultCapacity);

    // Moves `value` to the front if it is already present (dedup by exact RGBA value), otherwise
    // inserts it at the front and drops the oldest entry past capacity.
    void push(const KColor& value);

    void clear();

    [[nodiscard]] int capacity() const noexcept;
    void setCapacity(int capacity);

    // Most-recent-first.
    [[nodiscard]] const std::vector<KColor>& colors() const noexcept;

  private:
    std::vector<KColor> colors_;
    int capacity_;
};

// The swatch row below KColorPicker (decision 4): a fixed number of slots, each either explicitly
// "set" by the artist (sticky until overwritten) or, absent that, driven live off a bound
// KRecentColorStore's most-recent-first list. Clicking a slot applies its color; right-clicking (or
// holding the press) sets it to the row's current color.
class KColorSwatchRow final : public QWidget {
    Q_OBJECT

  public:
    static constexpr int kDefaultSlotCount = 8;
    static constexpr int kMaxSlotCount = 32;

    explicit KColorSwatchRow(QWidget* parent = nullptr);

    // Clamped to [1, kMaxSlotCount]; defaults to kDefaultSlotCount.
    void setSlotCount(int count);
    [[nodiscard]] int slotCount() const noexcept;

    // Not owned. Pass nullptr to detach -- unbound slots show only what was explicitly set.
    void setRecentColorStore(KRecentColorStore* store);
    [[nodiscard]] KRecentColorStore* recentColorStore() const noexcept;

    // The color a click on an unset slot would have applied is meaningless without this: the
    // color that "set" (long-press/context) writes into a slot, and what push()es into the bound
    // store whenever a color is actually applied elsewhere in the picker.
    void setCurrentColor(const KColor& color);
    [[nodiscard]] KColor currentColor() const noexcept;

    // Explicitly pins `index` to `color`, taking priority over the bound store's recents at that
    // position until clearSlot() releases it.
    void setSlotColor(int index, const KColor& color);
    void clearSlot(int index);

    // The color actually painted at `index`: the pinned value if one was set, else the store's
    // `index`-th most recent color, else nullopt for an empty slot.
    [[nodiscard]] std::optional<KColor> slotColor(int index) const;
    [[nodiscard]] bool isSlotPinned(int index) const;

    [[nodiscard]] QRectF slotRect(int index) const;
    // -1 when `point` is not over any slot.
    [[nodiscard]] int slotAt(const QPointF& point) const;

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

  Q_SIGNALS:
    // A slot was clicked: the artist wants `color` applied as the current color.
    void colorActivated(const KColor& color);
    // A slot was explicitly set (long-press or context menu) to the row's current color.
    void slotPinned(int index, const KColor& color);

  protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void timerEvent(QTimerEvent* event) override;

  private:
    void pinSlot(int index);
    void cancelLongPress();

    int slotCount_ = kDefaultSlotCount;
    KRecentColorStore* store_ = nullptr;
    KColor currentColor_{};
    std::vector<std::optional<KColor>> pinned_;
    int pressedSlot_ = -1;
    int longPressTimerId_ = 0;
    int hoveredSlot_ = -1;
};

} // namespace bloom::ui::kit
