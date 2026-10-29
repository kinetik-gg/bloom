#include <bloom/ui/kit/typography.hpp>

#include <bloom/ui/kit/fonts.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <QApplication>
#include <QCoreApplication>
#include <QEvent>
#include <QGuiApplication>
#include <QMetaObject>
#include <QWidget>

#include <array>
#include <cstring>

namespace bloom::ui::kit {
namespace {

// Qt 6.11's QApplicationPrivate::initializeWidgetFontHash(), name for name. A class Qt does not
// list here never receives a platform entry, so it inherits the application font.
constexpr auto kPlatformFontClasses = std::to_array<const char*>(
    {"QMenu",       "QMenuBar",    "QMenuItem",    "QMessageBox",           "QLabel",
     "QTipLabel",   "QTitleBar",   "QStatusBar",   "QMdiSubWindowTitleBar", "QDockWidgetTitle",
     "QPushButton", "QCheckBox",   "QRadioButton", "QToolButton",           "QAbstractItemView",
     "QListView",   "QHeaderView", "QListBox",     "QComboMenuItem",        "QComboLineEdit",
     "QSmallFont",  "QMiniFont"});

constexpr auto kEnforcementProperty = "bloomTypeRoleEnforcement";

// Re-entry guard: applying the roles itself sends ApplicationFontChange.
bool& applying() {
    static bool value = false;
    return value;
}

[[nodiscard]] bool smallClass(const char* className) {
    return std::strcmp(className, "QSmallFont") == 0 || std::strcmp(className, "QMiniFont") == 0;
}

class TypeRoleEnforcer final : public QObject {
  public:
    using QObject::QObject;

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        switch (event->type()) {
        case QEvent::ApplicationFontChange:
            // Sent to the application by QGuiApplication::setFont() before any window or widget
            // re-resolves: answering here means a platform font is never painted, not even once.
            if (watched == QCoreApplication::instance() && !applying() &&
                QGuiApplication::font() != font(TypeRole::Ui))
                applyTypeRoleFonts();
            schedule();
            break;
        case QEvent::ThemeChange:
        case QEvent::StyleChange:
            schedule();
            break;
        default:
            break;
        }
        return QObject::eventFilter(watched, event);
    }

  private:
    // One coalesced pass after the event that triggered it has finished: QApplication::setFont()
    // clears the class hash after its own notification, and a style change refills it.
    void schedule() {
        if (applying() || pending_)
            return;
        pending_ = true;
        QMetaObject::invokeMethod(
            this,
            [this] {
                pending_ = false;
                applyTypeRoleFonts();
            },
            Qt::QueuedConnection);
    }

    bool pending_ = false;
};

} // namespace

std::span<const char* const> platformFontClasses() { return kPlatformFontClasses; }

void enforceTypeRole(QWidget& widget) {
    if (widget.testAttribute(Qt::WA_SetFont) || isBundledFontFamily(widget.font().family()))
        return;
    widget.setFont(font(TypeRole::Ui));
}

void applyTypeRoleFonts() {
    if (applying() || qobject_cast<QApplication*>(QCoreApplication::instance()) == nullptr)
        return;
    applying() = true;
    const QFont ui = font(TypeRole::Ui);
    if (QGuiApplication::font() != ui)
        QApplication::setFont(ui);
    // An entry that shares the application font's data is the same as no entry at all: the widget
    // inherits its parent's font exactly as if the platform had said nothing.
    const QFont application = QApplication::font();
    const QFont small = font(TypeRole::UiSmall);
    for (const char* className : kPlatformFontClasses) {
        const QFont current = QApplication::font(className);
        if (smallClass(className) ? current != small : !current.isCopyOf(application))
            QApplication::setFont(smallClass(className) ? small : application, className);
    }
    for (QWidget* widget : QApplication::allWidgets())
        enforceTypeRole(*widget);
    applying() = false;
}

void installTypeRoleEnforcement(QApplication& application) {
    if (application.property(kEnforcementProperty).toBool())
        return;
    application.installEventFilter(new TypeRoleEnforcer(&application));
    application.setProperty(kEnforcementProperty, true);
}

} // namespace bloom::ui::kit
