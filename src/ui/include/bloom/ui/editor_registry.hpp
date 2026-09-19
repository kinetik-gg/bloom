#pragma once

#include <QString>

#include <functional>
#include <memory>
#include <string>
#include <vector>

class QWidget;

namespace bloom::runtime {
class GpuPresentationClient;
class TaskScheduler;
} // namespace bloom::runtime

namespace bloom::ui {

class CompositionPreviewController;
class RamPreviewController;
class CompositionSession;
class ProjectHost;

using EditorFactory = std::function<QWidget*(QWidget* parent)>;

struct EditorDescriptor {
    std::string id;
    QString displayName;
    EditorFactory create;
};

// Additive viewer GPU dependency context. It carries NO native/Vulkan/Qt handle:
// `presentationClient` is the runtime's opaque UI-side presentation port, produced by the service's
// coordinator, and `vulkanLoaderPath` is an explicit file path the app already resolved. The getter
// is read when a ViewerEditor is created, so a client that becomes available after startup is still
// handed to every later editor (including workspace replacements) without a second registry.
struct ViewerGpuDependencies final {
    std::function<std::shared_ptr<runtime::GpuPresentationClient>()> presentationClient;
    runtime::TaskScheduler* scheduler = nullptr;
    std::string vulkanLoaderPath;
    double devicePixelRatio = 1.0;
};

class EditorRegistry final {
  public:
    [[nodiscard]] bool registerEditor(EditorDescriptor descriptor);
    [[nodiscard]] const std::vector<EditorDescriptor>& editors() const noexcept;

  private:
    std::vector<EditorDescriptor> editors_;
};

// `ramPreview` is handed to the Timeline editor, whose transport cluster carries the RAM Preview
// command (task PERF1, item 3). Null leaves that one button disabled; every other editor is
// unaffected.
//
// `gpuDependencies` is an additive, optional injection seam for the Viewer editor only. When
// non-null, the viewer factory calls ViewerEditor::setGpuPresentationDependencies() with the
// dependency context's current presentation client. Null leaves every editor on its unchanged CPU
// path, exactly as before this parameter existed. The pointee must outlive every editor the
// registry creates.
[[nodiscard]] bool registerFoundationEditors(
    EditorRegistry& registry, CompositionSession& session,
    CompositionPreviewController& previewController, RamPreviewController* ramPreview = nullptr,
    ProjectHost* projectHost = nullptr, const ViewerGpuDependencies* gpuDependencies = nullptr);

} // namespace bloom::ui
