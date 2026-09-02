#include <bloom/ui/kit/mnemonic_style.hpp>

#include <QGuiApplication>
#include <QStyleFactory>

namespace bloom::ui::kit {

bool showMnemonicUnderline(const Qt::KeyboardModifiers modifiers) noexcept {
    return modifiers.testFlag(Qt::AltModifier);
}

AltUnderlineProxyStyle::AltUnderlineProxyStyle()
    : AltUnderlineProxyStyle(QStyleFactory::create(QStringLiteral("Fusion"))) {}

AltUnderlineProxyStyle::AltUnderlineProxyStyle(QStyle* baseStyle) : QProxyStyle(baseStyle) {}

int AltUnderlineProxyStyle::styleHint(const StyleHint hint, const QStyleOption* option,
                                      const QWidget* widget,
                                      QStyleHintReturn* returnData) const {
    if (hint == QStyle::SH_UnderlineShortcut) {
        return showMnemonicUnderline(QGuiApplication::keyboardModifiers()) ? 1 : 0;
    }
    return QProxyStyle::styleHint(hint, option, widget, returnData);
}

} // namespace bloom::ui::kit
