#pragma once

#include <bloom/commands/layer_alignment_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/document/ids.hpp>

#include <QObject>
#include <QString>

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace bloom::ui {

// The one app-wide active tool (ADR 0031, the After Effects model). Every Viewer reads it and acts
// on the gestures it receives with it; the context bar shows and chooses it.
enum class ViewerTool : std::uint8_t {
    Select,
    Hand,
    Zoom,
    Rectangle,
    Ellipse,
    Polygon,
    Star,
    Line,
    Pen,
    Text,
    // #364: picks Cryptomatte objects of the selected Cryptomatte node.
    Cryptomatte,
};

inline constexpr std::array kViewerTools{
    ViewerTool::Select, ViewerTool::Hand, ViewerTool::Zoom,  ViewerTool::Rectangle,
    ViewerTool::Ellipse, ViewerTool::Polygon, ViewerTool::Star, ViewerTool::Line,
    ViewerTool::Pen,    ViewerTool::Text,  ViewerTool::Cryptomatte};

// Rectangle, Ellipse, Polygon and Star: the tools that share one bar slot.
[[nodiscard]] constexpr bool isShapeTool(const ViewerTool tool) noexcept {
    return tool >= ViewerTool::Rectangle && tool <= ViewerTool::Star;
}
// The tools that draw a shape layer and so take the new-shape style (the Pen included).
[[nodiscard]] constexpr bool drawsShape(const ViewerTool tool) noexcept {
    return (tool >= ViewerTool::Rectangle && tool <= ViewerTool::Line) || tool == ViewerTool::Pen;
}

// What the bar and the menus call a tool, its bare-letter shortcut (none for a letterless tool), a
// stable object-name stem, and how it is used. The tooltip format is "Name (Key)\nhow to use it".
struct ViewerToolInfo {
    QString name;
    QString key;
    QString usage;
    std::string_view objectStem;
};
[[nodiscard]] ViewerToolInfo viewerToolInfo(ViewerTool tool);
[[nodiscard]] QString viewerToolTip(ViewerTool tool);

// The tool a modifier-free key chooses, registered once for the whole application: V, H, Z, R, E,
// P and T. Escape is not a tool key here; the Viewer gives it its confirming meaning.
[[nodiscard]] std::optional<ViewerTool> toolForKey(int key) noexcept;

// The shape schema's default stroke, in composition pixels.
inline constexpr double kShapeStrokeDefault = 2.0;

// The style a newly drawn shape starts with: the schema defaults until the artist changes them.
struct NewShapeStyle {
    bool fillEnabled = true;
    core::Color4d fillColor{1.0, 1.0, 1.0, 1.0};
    bool strokeEnabled = false;
    core::Color4d strokeColor{0.0, 0.0, 0.0, 1.0};
    double strokeWidth = kShapeStrokeDefault;
    friend bool operator==(const NewShapeStyle&, const NewShapeStyle&) = default;
};
[[nodiscard]] commands::ShapeLayerStyle shapeLayerStyle(const NewShapeStyle& style);

// The style new text starts with. An empty font reference is the bundled default face; otherwise
// it is a font catalogue reference ("family|style|digest", text_font_choices.hpp).
struct NewTextStyle {
    double size = 0.0; // 0: the text schema's default size
    std::int64_t alignment = 0;
    QString fontReference;
    friend bool operator==(const NewTextStyle&, const NewTextStyle&) = default;
};

// Session/UI state for the tools: the active tool, the shape slot's last shape, the options for
// new shapes and new text, and what Align aligns to. None of it is project truth; it is owned by
// the CompositionSession (one per application window) and handed to its Viewers and the bar.
class ToolController final : public QObject {
    Q_OBJECT

  public:
    explicit ToolController(QObject* parent = nullptr);

    [[nodiscard]] ViewerTool tool() const noexcept { return tool_; }
    // Chooses the tool for every Viewer. Choosing the active tool again emits nothing. Choosing a
    // tool never authors anything.
    void setTool(ViewerTool tool);
    // The shape the shared Rectangle/Ellipse/Polygon/Star slot shows: the last one chosen.
    [[nodiscard]] ViewerTool lastShapeTool() const noexcept { return lastShapeTool_; }

    [[nodiscard]] const NewShapeStyle& newShapeStyle() const noexcept { return shapeStyle_; }
    void setNewShapeStyle(const NewShapeStyle& style);
    [[nodiscard]] const NewTextStyle& newTextStyle() const noexcept { return textStyle_; }
    void setNewTextStyle(const NewTextStyle& style);

    [[nodiscard]] commands::AlignmentTarget alignmentTarget() const noexcept { return target_; }
    void setAlignmentTarget(commands::AlignmentTarget target);

  signals:
    void toolChanged(bloom::ui::ViewerTool tool);
    void newShapeStyleChanged();
    void newTextStyleChanged();
    void alignmentTargetChanged();

  private:
    ViewerTool tool_ = ViewerTool::Select;
    ViewerTool lastShapeTool_ = ViewerTool::Rectangle;
    NewShapeStyle shapeStyle_;
    NewTextStyle textStyle_;
    commands::AlignmentTarget target_ = commands::AlignmentTarget::Selection;
};

} // namespace bloom::ui
