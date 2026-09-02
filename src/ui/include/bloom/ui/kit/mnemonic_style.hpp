#pragma once

#include <QProxyStyle>
#include <QString>

class QStyleOption;
class QWidget;

namespace bloom::ui::kit {

// Mnemonic underlines only while Alt is held (task U2, issue #118): Bloom's menu bar and menus
// carry "&File"-style mnemonics, but the underline itself must stay hidden until the artist
// actually presses Alt, matching the platform-standard "hold Alt to reveal access keys"
// convention rather than showing every underline permanently. Qt's own style hint
// (QStyle::SH_UnderlineShortcut) decides this, and the platform/Fusion default does not reliably
// key it to the live Alt state on every platform Bloom ships to, so this proxy overrides exactly
// that one hint and defers every other style query to its base style unchanged.
//
// Scope: installed once, application-wide, in apps/bloom/main.cpp right after
// installKinetikTheme() -- see that call site's comment. Not a per-widget style.
class AltUnderlineProxyStyle final : public QProxyStyle {
    Q_OBJECT

  public:
    // Wraps a freshly created "Fusion" base style (the same style installKinetikTheme() selects),
    // so replacing the application style with this proxy never double-frees or aliases the style
    // object installKinetikTheme() already installed.
    AltUnderlineProxyStyle();
    explicit AltUnderlineProxyStyle(QStyle* baseStyle);

    [[nodiscard]] int styleHint(StyleHint hint, const QStyleOption* option = nullptr,
                                const QWidget* widget = nullptr,
                                QStyleHintReturn* returnData = nullptr) const override;
};

// The pure decision AltUnderlineProxyStyle::styleHint() applies for SH_UnderlineShortcut, factored
// out so both branches (Alt held / Alt not held) are directly unit-testable without depending on
// real global keyboard state, which an offscreen test cannot reliably drive.
[[nodiscard]] bool showMnemonicUnderline(Qt::KeyboardModifiers modifiers) noexcept;

} // namespace bloom::ui::kit
