#pragma once

// Operating-system notifications behind UiFeedback's Notifier seam.
//
//   Linux    org.freedesktop.Notifications over QtDBus (when the Qt build has it); a click on the
//            default action activates Bloom. Without QtDBus, or without a session bus, the tray
//            message below.
//   macOS    UNUserNotificationCenter from the platform layer (system_notifier_macos.mm) when
//            Bloom runs from an application bundle; otherwise the tray message.
//   Windows  QSystemTrayIcon::showMessage (the supported toast/balloon fallback); a click on the
//            message activates Bloom.
//
// A platform with no notification surface posts nothing and says so through the log. Posting never
// blocks the caller.

#include <bloom/ui/ui_feedback.hpp>

#include <memory>

class QObject;

namespace bloom::ui {

[[nodiscard]] std::unique_ptr<Notifier> makeSystemNotifier(QObject* parent = nullptr);

#if defined(__APPLE__)
// Implemented in system_notifier_macos.mm. Returns false when no bundle identifier exists, in which
// case the caller uses the tray message.
[[nodiscard]] bool postMacNotification(const SystemNotification& notification,
                                       const std::function<void()>& onActivated);
#endif

} // namespace bloom::ui
