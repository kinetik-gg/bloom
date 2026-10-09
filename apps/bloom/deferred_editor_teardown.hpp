#pragma once

#include <QCoreApplication>
#include <QEvent>

namespace bloom::app {
// Declare after editor borrowed dependencies and before workspace/window consumers. This runs
// only on post-eventloop return/unwind; detached editors already scheduled for safe deletion must
// die while their controller/service references are still valid. It dispatches no ordinary work.
class DeferredEditorTeardown final {
  public:
    ~DeferredEditorTeardown() { drain(); }
    static void drain() { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }
};
} // namespace bloom::app
