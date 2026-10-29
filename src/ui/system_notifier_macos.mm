// macOS notification delivery through UNUserNotificationCenter (system_notifier.hpp). Compiled only
// on Apple platforms. UNUserNotificationCenter requires an application bundle; an unbundled
// developer binary has no bundle identifier and falls back to the tray message in the caller.
// Not exercised on Linux CI; unverified on macOS hardware.

#include <bloom/ui/system_notifier.hpp>

#import <Foundation/Foundation.h>
#import <UserNotifications/UserNotifications.h>

#include <QMetaObject>
#include <QString>
#include <QCoreApplication>

#include <functional>
#include <memory>

@interface BloomNotificationDelegate : NSObject <UNUserNotificationCenterDelegate>
@property(nonatomic, assign) std::function<void()>* onActivated;
@end

@implementation BloomNotificationDelegate
- (void)userNotificationCenter:(UNUserNotificationCenter*)center
    didReceiveNotificationResponse:(UNNotificationResponse*)response
             withCompletionHandler:(void (^)(void))completionHandler {
    if (self.onActivated != nullptr && *self.onActivated) {
        auto handler = *self.onActivated;
        QMetaObject::invokeMethod(QCoreApplication::instance(), [handler] { handler(); },
                                  Qt::QueuedConnection);
    }
    completionHandler();
}
// Bloom posts only while inactive, but show the banner if a result lands as the window deactivates.
- (void)userNotificationCenter:(UNUserNotificationCenter*)center
       willPresentNotification:(UNNotification*)notification
         withCompletionHandler:(void (^)(UNNotificationPresentationOptions))completionHandler {
    completionHandler(UNNotificationPresentationOptionBanner);
}
@end

namespace bloom::ui {

bool postMacNotification(const SystemNotification& notification,
                         const std::function<void()>& onActivated) {
    if ([[NSBundle mainBundle] bundleIdentifier] == nil) {
        return false;
    }
    // One delegate and one stored handler for the process; the handler is refreshed on each post.
    static std::function<void()> storedHandler;
    static BloomNotificationDelegate* delegate = nil;
    storedHandler = onActivated;
    UNUserNotificationCenter* center = [UNUserNotificationCenter currentNotificationCenter];
    if (delegate == nil) {
        delegate = [[BloomNotificationDelegate alloc] init];
        delegate.onActivated = &storedHandler;
        center.delegate = delegate;
    }
    UNMutableNotificationContent* content = [[UNMutableNotificationContent alloc] init];
    content.title = notification.title.toNSString();
    content.body = notification.body.toNSString();
    // Bloom plays its own sound; the system one would double it.
    content.sound = nil;
    UNNotificationRequest* request =
        [UNNotificationRequest requestWithIdentifier:[[NSUUID UUID] UUIDString]
                                             content:content
                                             trigger:nil];
    [center requestAuthorizationWithOptions:UNAuthorizationOptionAlert
                          completionHandler:^(BOOL granted, NSError*) {
                              if (granted) {
                                  [center addNotificationRequest:request withCompletionHandler:nil];
                              }
                          }];
    return true;
}

} // namespace bloom::ui
