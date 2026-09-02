#pragma once

#include <bloom/ui/kit/icons.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <QAbstractButton>
#include <QSize>
#include <QString>

#include <cstdint>
#include <optional>

namespace bloom::ui::kit {

// The Kinetik button. Four variants across three control sizes, with an optional leading icon.
//
// A QAbstractButton subclass that paints itself rather than a QPushButton wearing a stylesheet:
// the accent recipes, the surface-ladder hover step, the outside-drawn focus ring, and the exact
// hairline all need real painting, and a QSS rule cannot express "one step up the surface ladder"
// or "snapped to a whole physical pixel at 150%".
class KButton final : public QAbstractButton {
    Q_OBJECT

  public:
    enum class Variant : std::uint8_t {
        // The one affirmative action on a surface: Accent fill, Foreground ink.
        Primary,
        // The ordinary action: a raised surface with a hairline.
        Secondary,
        // Chrome-weight: no surface at rest, muted ink, surface only on hover.
        Ghost,
        // Destructive: Error ink and border at rest, Error fill once the pointer commits.
        Danger,
    };

    enum class ControlSize : std::uint8_t {
        Compact,
        Default,
        Roomy,
    };

    explicit KButton(QWidget* parent = nullptr);
    explicit KButton(const QString& text, QWidget* parent = nullptr);
    KButton(IconId icon, const QString& text, QWidget* parent = nullptr);

    void setVariant(Variant variant);
    [[nodiscard]] Variant variant() const noexcept;

    void setControlSize(ControlSize size);
    [[nodiscard]] ControlSize controlSize() const noexcept;

    void setIconId(std::optional<IconId> icon);
    [[nodiscard]] std::optional<IconId> iconId() const noexcept;

    // Opt-in, Ghost-only: hover/press take the Danger recipe (Error fill, Foreground ink) while
    // rest/focus stay plain ghost -- the "close button" convention (title bar, panel header) where
    // the control reads as chrome-weight until the pointer commits to the destructive action, never
    // Error-colored at rest the way Variant::Danger itself is. False for every other variant.
    void setDangerOnHover(bool dangerOnHover);
    [[nodiscard]] bool dangerOnHover() const noexcept;

    // The single state this button is in right now, resolved by one rule so that painting and
    // tests can never disagree about it.
    [[nodiscard]] State visualState() const;

    // The fill and ink this button paints in a given state, for the state the button is in. Public
    // so a gallery test asserts the recipe rather than sampling pixels.
    [[nodiscard]] QColor fillForState(State state) const;
    [[nodiscard]] QColor inkForVisualState(State state) const;

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

  protected:
    void paintEvent(QPaintEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void changeEvent(QEvent* event) override;

  private:
    [[nodiscard]] int controlExtent() const;
    [[nodiscard]] Size iconBox() const;

    [[nodiscard]] bool paintsAsDanger(State state) const noexcept;

    Variant variant_ = Variant::Secondary;
    ControlSize controlSize_ = ControlSize::Default;
    std::optional<IconId> icon_;
    bool hovered_ = false;
    bool dangerOnHover_ = false;
};

} // namespace bloom::ui::kit
