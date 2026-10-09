#include <bloom/ui/workspace_host.hpp>

#include <bloom/ui/editor_area.hpp>
#include <bloom/ui/viewer_editor.hpp>

#include <QSplitter>

namespace bloom::ui {
namespace {
void releasePackedFrames(QWidget* widget) {
    if (auto* area = qobject_cast<EditorArea*>(widget)) {
        if (auto* viewer = dynamic_cast<ViewerEditor*>(area->nativeSurface()))
            viewer->releasePackedFramesForShutdown();
        return;
    }
    if (auto* splitter = qobject_cast<QSplitter*>(widget))
        for (int index = 0; index < splitter->count(); ++index)
            releasePackedFrames(splitter->widget(index));
}
} // namespace
void WorkspaceHost::releasePackedFramesForShutdown() { releasePackedFrames(rootWidget_); }
} // namespace bloom::ui
