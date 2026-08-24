#include <bloom/ui/editor_registry.hpp>

#include <QLabel>
#include <QWidget>

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

bool registerFoundationEditors(EditorRegistry& registry) {
    const auto addPlaceholder = [&registry](std::string id, QString name, QString description) {
        return registry.registerEditor(
            {.id = std::move(id),
             .displayName = std::move(name),
             .create = [description = std::move(description)](QWidget* parent) {
                 auto* label = new QLabel(description, parent);
                 label->setAlignment(Qt::AlignCenter);
                 label->setObjectName("editorPlaceholder");
                 return label;
             }});
    };

    return addPlaceholder("bloom.viewer", "Compositor",
                          "Viewer\n\nProject frame output appears here") &&
           addPlaceholder("bloom.nodes", "Nodes",
                          "Node graph\n\nCanonical composition graph appears here") &&
           addPlaceholder("bloom.timeline", "Timeline", "Layers, animation, and current time") &&
           addPlaceholder("bloom.media", "Media", "Assets and compositions") &&
           addPlaceholder("bloom.properties", "Properties", "Selection-driven parameters");
}

} // namespace bloom::ui
