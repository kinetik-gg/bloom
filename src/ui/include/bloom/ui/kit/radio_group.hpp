#pragma once

#include <bloom/ui/kit/icons.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <QRect>
#include <QSize>
#include <QString>
#include <QVector>
#include <QWidget>

#include <cstdint>
#include <optional>

namespace bloom::ui::kit {

// A one-of-many choice, in the two presentations Bloom's density needs: a segmented bar for a
// short set that belongs on one row of chrome, and a discrete list for a longer or more explanatory
// set. Same model, same state machine, same keyboard behavior -- only the drawing differs, so a
// panel can change its mind about presentation without changing its logic.
class KRadioGroup final : public QWidget {
    Q_OBJECT

  public:
    enum class Presentation : std::uint8_t {
        Segmented,
        Discrete,
    };

    explicit KRadioGroup(QWidget* parent = nullptr);

    int addOption(const QString& text, std::optional<IconId> icon = std::nullopt);
    [[nodiscard]] int count() const;
    [[nodiscard]] QString optionText(int index) const;

    void setOptionEnabled(int index, bool enabled);
    [[nodiscard]] bool isOptionEnabled(int index) const;

    [[nodiscard]] int currentIndex() const noexcept;
    void setCurrentIndex(int index);

    void setPresentation(Presentation presentation);
    [[nodiscard]] Presentation presentation() const noexcept;

    // The rectangle an option occupies, in this widget's coordinates. Public so a test can drive a
    // real click at a real option rather than guessing coordinates.
    [[nodiscard]] QRect optionRect(int index) const;

    // The state of one option: Selected for the current one, Disabled for an unavailable one,
    // Hover for whichever the pointer is over, Normal otherwise.
    [[nodiscard]] State stateForOption(int index) const;

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

  Q_SIGNALS:
    void currentIndexChanged(int index);

  protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void changeEvent(QEvent* event) override;

  private:
    struct Option {
        QString text;
        std::optional<IconId> icon;
        bool enabled = true;
    };

    [[nodiscard]] int optionAt(const QPoint& point) const;
    [[nodiscard]] int optionExtent() const;
    void commitIndex(int index);
    void selectNeighbour(int direction);
    void paintSegmented(QPainter& painter);
    void paintDiscrete(QPainter& painter);

    QVector<Option> options_;
    int currentIndex_ = -1;
    int hoveredIndex_ = -1;
    Presentation presentation_ = Presentation::Segmented;
};

} // namespace bloom::ui::kit
