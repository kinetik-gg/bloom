#include <bloom/ui/editor_registry.hpp>

#include <bloom/ui/composition_editors.hpp>
#include <bloom/ui/node_editor.hpp>
#include <bloom/ui/viewer_editor.hpp>

#include "script_panel.hpp"
#ifdef BLOOM_BUILD_PYTHON
#include "script_panel_runtime.hpp"
#endif

#include <algorithm>
#include <utility>

namespace bloom::ui {

bool EditorRegistry::registerEditor(EditorDescriptor descriptor) {
    if (descriptor.id.empty() || descriptor.displayName.isEmpty() || !descriptor.create) {
        return false;
    }

    const auto duplicate = std::ranges::find_if(
        editors_, [&descriptor](const auto& existing) { return existing.id == descriptor.id; });
    if (duplicate != editors_.end()) {
        return false;
    }

    editors_.push_back(std::move(descriptor));
    return true;
}

const std::vector<EditorDescriptor>& EditorRegistry::editors() const noexcept { return editors_; }

bool registerFoundationEditors(EditorRegistry& registry, CompositionSession& session,
                               CompositionPreviewController& previewController,
                               RamPreviewController* const ramPreview, ProjectHost* projectHost) {
    std::shared_ptr<ScriptPanelRuntime> script;
#ifdef BLOOM_BUILD_PYTHON
    if (projectHost)
        script = std::make_shared<ScriptPanelRuntime>(session, *projectHost);
#else
    (void)projectHost;
#endif
    const auto addEditor = [&registry](std::string id, QString name, EditorFactory factory) {
        return registry.registerEditor(
            {.id = std::move(id), .displayName = std::move(name), .create = std::move(factory)});
    };

    return addEditor("bloom.viewer", "Viewer",
                     [&session, &previewController, ramPreview](QWidget* parent) {
                         return new ViewerEditor(session, previewController, ramPreview, parent);
                     }) &&
           addEditor(
               "bloom.nodes", "Nodes",
               [&session](QWidget* parent) { return new NodeGraphEditor(session, parent); }) &&
           addEditor("bloom.timeline", "Timeline",
                     [&session, &previewController](QWidget* parent) {
                         return new TimelineEditor(session, previewController, parent);
                     }) &&
           addEditor("bloom.assets", "Assets",
                     [&session](QWidget* parent) { return new AssetsEditor(session, parent); }) &&
           addEditor(
               "bloom.properties", "Properties",
               [&session](QWidget* parent) { return new PropertiesEditor(session, parent); }) &&
           addEditor("bloom.script", "Script",
                     [script](QWidget* parent) { return new ScriptPanel(script, parent); });
}

} // namespace bloom::ui
