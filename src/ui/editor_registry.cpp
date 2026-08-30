#include <bloom/ui/editor_registry.hpp>

#include <bloom/ui/composition_editors.hpp>
#include <bloom/ui/node_editor.hpp>
#include <bloom/ui/viewer_editor.hpp>

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
                               CompositionPreviewController& previewController) {
    const auto addEditor = [&registry](std::string id, QString name, EditorFactory factory) {
        return registry.registerEditor(
            {.id = std::move(id), .displayName = std::move(name), .create = std::move(factory)});
    };

    return addEditor("bloom.viewer", "Compositor",
                     [&session, &previewController](QWidget* parent) {
                         return new ViewerEditor(session, previewController, parent);
                     }) &&
           addEditor(
               "bloom.nodes", "Nodes",
               [&session](QWidget* parent) { return new NodeGraphEditor(session, parent); }) &&
           addEditor("bloom.timeline", "Timeline",
                     [&session, &previewController](QWidget* parent) {
                         return new TimelineEditor(session, previewController, parent);
                     }) &&
           addEditor("bloom.media", "Media",
                     [&session](QWidget* parent) { return new MediaEditor(session, parent); }) &&
           addEditor("bloom.properties", "Properties",
                     [&session](QWidget* parent) { return new PropertiesEditor(session, parent); });
}

} // namespace bloom::ui
