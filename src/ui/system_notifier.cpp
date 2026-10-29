#include <bloom/ui/system_notifier.hpp>

#include <QApplication>
#include <QDebug>
#include <QIcon>
#include <QObject>
#include <QStyle>
#include <QSystemTrayIcon>

#if defined(BLOOM_UI_HAS_QTDBUS)
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QVariantMap>
#endif

namespace bloom::ui {

namespace {

class TrayNotifier final : public QObject {
  public:
    explicit TrayNotifier(QObject* parent) : QObject(parent) {}

    bool post(const SystemNotification& notification) {
        if (!QSystemTrayIcon::isSystemTrayAvailable() ||
            !QSystemTrayIcon::supportsMessages()) {
            qInfo() << "Bloom: no notification surface is available on this desktop.";
            return false;
        }
        if (tray_ == nullptr) {
            tray_ = new QSystemTrayIcon(QApplication::windowIcon(), this);
            connect(tray_, &QSystemTrayIcon::messageClicked, this, [this] {
                if (activated_) {
                    activated_();
                }
            });
        }
        tray_->show();
        tray_->showMessage(notification.title, notification.body,
                           notification.failure ? QSystemTrayIcon::Warning
                                                : QSystemTrayIcon::Information,
                           8000);
        return true;
    }

    void setActivated(std::function<void()> handler) { activated_ = std::move(handler); }

  private:
    QSystemTrayIcon* tray_ = nullptr;
    std::function<void()> activated_;
};

class SystemNotifier final : public QObject, public Notifier {
    Q_OBJECT
  public:
    explicit SystemNotifier(QObject* parent) : QObject(parent), tray_(new TrayNotifier(this)) {
#if defined(BLOOM_UI_HAS_QTDBUS)
        auto bus = QDBusConnection::sessionBus();
        if (bus.isConnected()) {
            bus.connect(QStringLiteral("org.freedesktop.Notifications"),
                        QStringLiteral("/org/freedesktop/Notifications"),
                        QStringLiteral("org.freedesktop.Notifications"),
                        QStringLiteral("ActionInvoked"), this,
                        SLOT(onActionInvoked(uint,QString)));
        }
#endif
    }

    void post(const SystemNotification& notification) override {
#if defined(BLOOM_UI_HAS_QTDBUS)
        if (postDBus(notification)) {
            return;
        }
#elif defined(__APPLE__)
        if (postMacNotification(notification, activated_)) {
            return;
        }
#endif
        tray_->post(notification);
    }

    void setActivationHandler(std::function<void()> handler) override {
        activated_ = handler;
        tray_->setActivated(std::move(handler));
    }

  public slots:
    // The freedesktop ActionInvoked signal: the artist clicked the notification we last posted.
    void onActionInvoked(unsigned id, const QString& action) {
        if (id == lastId_ && action == QStringLiteral("default") && activated_) {
            activated_();
        }
    }

#if defined(BLOOM_UI_HAS_QTDBUS)
  private:
    bool postDBus(const SystemNotification& notification) {
        auto bus = QDBusConnection::sessionBus();
        if (!bus.isConnected()) {
            return false;
        }
        auto message = QDBusMessage::createMethodCall(
            QStringLiteral("org.freedesktop.Notifications"),
            QStringLiteral("/org/freedesktop/Notifications"),
            QStringLiteral("org.freedesktop.Notifications"), QStringLiteral("Notify"));
        QVariantMap hints;
        hints.insert(QStringLiteral("urgency"), static_cast<uchar>(notification.failure ? 2 : 1));
        hints.insert(QStringLiteral("desktop-entry"), QStringLiteral("bloom"));
        message << QStringLiteral("Bloom") << 0U << QStringLiteral("bloom") << notification.title
                << notification.body
                << QStringList{QStringLiteral("default"), QObject::tr("Show Bloom")} << hints
                << -1;
        auto* watcher = new QDBusPendingCallWatcher(bus.asyncCall(message, 2000), this);
        QObject::connect(watcher, &QDBusPendingCallWatcher::finished, this,
                         [this, watcher, notification] {
                             const QDBusPendingReply<unsigned> reply = *watcher;
                             if (reply.isError()) {
                                 // The service is missing or refused: use the tray message.
                                 tray_->post(notification);
                             } else {
                                 lastId_ = reply.value();
                             }
                             watcher->deleteLater();
                         });
        return true;
    }
#endif

  private:
    unsigned lastId_ = 0;
    TrayNotifier* tray_;
    std::function<void()> activated_;
};

} // namespace

std::unique_ptr<Notifier> makeSystemNotifier(QObject* parent) {
    return std::make_unique<SystemNotifier>(parent);
}

} // namespace bloom::ui

#include "system_notifier.moc"
