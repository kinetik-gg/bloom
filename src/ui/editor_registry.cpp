#include <bloom/ui/editor_registry.hpp>

#include <bloom/ui/composition_editors.hpp>
#include <bloom/ui/node_editor.hpp>
#include <bloom/ui/viewer_editor.hpp>

#include "script_panel.hpp"
#ifdef BLOOM_BUILD_PYTHON
#include "script_panel_runtime.hpp"
#include <bloom/scripting/live_session_host.hpp>
#include <bloom/ui/live_session_adapter.hpp>
#endif

#include <algorithm>
#include <utility>

namespace bloom::ui {

#ifdef BLOOM_BUILD_PYTHON
namespace {
// The Script editor's console with, when the composition root supplied no shared live session, a
// live session host and adapter of its own. The console stops before the host it leases from.
struct ScriptConsole final {
    std::unique_ptr<scripting::LiveSessionHost> ownedHost;
    std::unique_ptr<LiveSessionAdapter> ownedAdapter;
    std::shared_ptr<ScriptPanelRuntime> runtime;
    ScriptConsole() = default;
    ScriptConsole(const ScriptConsole&) = delete;
    ScriptConsole& operator=(const ScriptConsole&) = delete;
    ~ScriptConsole() {
        runtime.reset();
        ownedAdapter.reset();
        ownedHost.reset();
    }
};
} // namespace
#endif

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
                               RamPreviewController* const ramPreview, ProjectHost* projectHost,
                               const ViewerGpuDependencies* const gpuDependencies,
                               std::shared_ptr<host::GpuExportProvider> gpuExportProvider,
                               scripting::LiveSessionHost* const liveSession) {
    std::shared_ptr<ScriptPanelRuntime> script;
#ifdef BLOOM_BUILD_PYTHON
    if (projectHost) {
        auto console = std::make_shared<ScriptConsole>();
        auto* live = liveSession;
        if (live == nullptr) {
            console->ownedHost = std::make_unique<scripting::LiveSessionHost>();
            console->ownedAdapter =
                std::make_unique<LiveSessionAdapter>(*console->ownedHost, session, *projectHost);
            live = console->ownedHost.get();
        }
        console->runtime =
            std::make_shared<ScriptPanelRuntime>(*live, std::move(gpuExportProvider));
        // The console's lifetime follows the runtime the Script editors share.
        script = std::shared_ptr<ScriptPanelRuntime>(console, console->runtime.get());
    }
#else
    (void)projectHost;
    (void)gpuExportProvider;
    (void)liveSession;
#endif
    const auto addEditor = [&registry](std::string id, QString name, EditorFactory factory) {
        return registry.registerEditor(
            {.id = std::move(id), .displayName = std::move(name), .create = std::move(factory)});
    };

    return addEditor(
               "bloom.viewer", "Viewer",
               [&session, &previewController, ramPreview, gpuDependencies](QWidget* parent) {
                   auto* viewer = new ViewerEditor(session, previewController, ramPreview, parent);
                   if (gpuDependencies != nullptr && gpuDependencies->presentationClient) {
                       viewer->setGpuPresentationDependencies(
                           gpuDependencies->presentationClient(), gpuDependencies->scheduler,
                           gpuDependencies->vulkanLoaderPath, gpuDependencies->devicePixelRatio);
                   }
                   return viewer;
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
