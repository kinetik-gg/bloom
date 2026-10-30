#pragma once

#include <bloom/ui/kit/icons.hpp>
#include <bloom/ui/kit/painting.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <QAbstractButton>
#include <QSize>
#include <QString>

#include <cstdint>
#include <optional>

namespace bloom::ui::kit {

// The Kinetik button. Five variants across three control sizes, with an optional leading icon.
//
// A QAbstractButton subclass that paints itself rather than a QPushButton wearing a stylesheet:
// the accent recipes, the surface-ladder hover step, the single focus border, and the exact
// hairline all need real painting, and a QSS rule cannot express "one step up the surface ladder"
// or "snapped to a whole physical pixel at 150%". Its colours come from kit::buttonColors(), the
// one button recipe it shares with KIconButton and KMenuButton.
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
        // Destructive: outlined in Error at rest, Error fill once the pointer commits.
        Danger,
        // A value that opens something (a settings dialog, a file chooser) from a dense row, as a
        // render queue's settings do: no plate, Accent ink, underlined under the pointer, the focus
        // border only on keyboard focus. Long text elides in the middle, so a path keeps its file.
        Link,
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

    // Opt-in, Ghost-only: hover/press take the Danger fill (Error fill, Foreground ink) while
    // rest/focus stay plain ghost -- the "close button" convention (title bar, panel header) where
    // the control reads as chrome-weight until the pointer commits to the destructive action, never
    // Error-outlined at rest the way Variant::Danger itself is. False for every other variant.
    void setDangerOnHover(bool dangerOnHover);
    [[nodiscard]] bool dangerOnHover() const noexcept;

    // The single state this button is in right now, resolved by one rule so that painting and
    // tests can never disagree about it.
    [[nodiscard]] State visualState() const;

    // The fill and ink this button paints in a given state, for the state the button is in. Public
    // so a gallery test asserts the recipe rather than sampling pixels.
    [[nodiscard]] QColor fillForState(State state) const;
    [[nodiscard]] QColor inkForVisualState(State state) const;

    // The single border's state channel, by the kit-wide focus/hover rule
    // (kit::borderForInteraction). Accent here outranks every variant's own resting outline.
    [[nodiscard]] Color borderToken() const;

    // The kit::buttonColors() recipe this variant resolves through.
    [[nodiscard]] ButtonKind buttonKind() const noexcept;

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

    Variant variant_ = Variant::Secondary;
    ControlSize controlSize_ = ControlSize::Default;
    std::optional<IconId> icon_;
    bool hovered_ = false;
    bool dangerOnHover_ = false;
};

} // namespace bloom::ui::kit
