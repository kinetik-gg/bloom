#pragma once

#include <bloom/document/ids.hpp>
#include <bloom/platform/font_catalog.hpp>

#include <QPointer>
#include <QWidget>

#include <functional>
#include <vector>

namespace bloom::document {
struct NodeRecord;
}

namespace bloom::ui::kit {
class KCheckBox;
class KColorChip;
class KDropdown;
class KRadioGroup;
class KValueField;
} // namespace bloom::ui::kit

namespace bloom::ui {

class CompositionSession;
class ViewerEditor;

// The context bar's tool options (ADR 0031). Each group reads real session or document state and
// writes through the session's commands; the bar shows one instance in its row and, when the row is
// too narrow, another in its overflow menu, both bound to the same state. `committed` runs after a
// field commits so the bar can hand keyboard focus back to the Viewer.

// The handle mode of the anchors the Viewer is editing (Select and Pen): Corner, Mirrored, Aligned,
// Free or Auto, mixed for a selection that disagrees; disabled, with the reason, with no anchors.
class AnchorModeOptions final : public QWidget {
    Q_OBJECT
  public:
    AnchorModeOptions(std::function<ViewerEditor*()> viewer, QWidget* parent);
    void refresh();
    [[nodiscard]] kit::KRadioGroup* switcher() const noexcept { return switcher_; }

  private:
    std::function<ViewerEditor*()> viewer_;
    kit::KRadioGroup* switcher_ = nullptr;
};

// Fill and stroke for the shape tools and the Pen: the selected shape layer's, edited with the
// same parameter commands Properties uses, or with no shape layer selected the style new shapes
// start with (ToolController session state).
class ShapeStyleOptions final : public QWidget {
    Q_OBJECT
  public:
    ShapeStyleOptions(CompositionSession& session, std::function<void()> committed,
                      QWidget* parent);
    void refresh();

  private:
    [[nodiscard]] const document::NodeRecord* target() const;
    [[nodiscard]] std::optional<document::ParameterId> parameter(std::string_view role) const;
    void write(std::string_view role, const QString& label);
    CompositionSession& session_;
    std::function<void()> committed_;
    kit::KCheckBox* fill_ = nullptr;
    kit::KColorChip* fillColor_ = nullptr;
    kit::KCheckBox* stroke_ = nullptr;
    kit::KColorChip* strokeColor_ = nullptr;
    kit::KValueField* strokeWidth_ = nullptr;
    bool refreshing_ = false;
};

// Font, size and alignment for the Text tool: the selected text layer's, through Properties'
// commands, or with no text layer selected the style new text starts with.
class TextStyleOptions final : public QWidget {
    Q_OBJECT
  public:
    TextStyleOptions(CompositionSession& session, std::function<void()> committed,
                     QWidget* parent);
    void refresh();

  private:
    [[nodiscard]] const document::NodeRecord* target() const;
    [[nodiscard]] std::optional<document::ParameterId> parameter(std::string_view role) const;
    void populateFonts();
    void pollFonts();
    void chooseFont(int index);
    CompositionSession& session_;
    std::function<void()> committed_;
    kit::KDropdown* font_ = nullptr;
    std::vector<platform::FontFace> faces_;
    kit::KValueField* size_ = nullptr;
    kit::KRadioGroup* alignment_ = nullptr;
    bool refreshing_ = false;
};

} // namespace bloom::ui
