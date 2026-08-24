#pragma once

#include <QString>

#include <functional>
#include <string>
#include <vector>

class QWidget;

namespace bloom::ui {

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

[[nodiscard]] bool registerFoundationEditors(EditorRegistry& registry);

} // namespace bloom::ui
