#pragma once

#include <QString>

#include <functional>
#include <string>
#include <vector>

class QWidget;

namespace bloom::ui {

class CompositionPreviewController;
class RamPreviewController;
class CompositionSession;

using EditorFactory = std::function<QWidget*(QWidget* parent)>;

struct EditorDescriptor {
    std::string id;
    QString displayName;
    EditorFactory create;
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
[[nodiscard]] bool registerFoundationEditors(EditorRegistry& registry, CompositionSession& session,
                                             CompositionPreviewController& previewController,
                                             RamPreviewController* ramPreview = nullptr);

} // namespace bloom::ui
