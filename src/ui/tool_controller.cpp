#include <bloom/ui/tool_controller.hpp>

#include <QCoreApplication>

namespace bloom::ui {

namespace {
QString tr(const char* text) { return QCoreApplication::translate("bloom::ui::ToolController", text); }
} // namespace

ViewerToolInfo viewerToolInfo(const ViewerTool tool) {
    switch (tool) {
    case ViewerTool::Select:
        return {tr("Select"), QStringLiteral("V"), tr("Select and move layers"), "Select"};
    case ViewerTool::Hand:
        return {tr("Hand"), QStringLiteral("H"), tr("Drag to pan"), "Hand"};
    case ViewerTool::Zoom:
        return {tr("Zoom"), QStringLiteral("Z"), tr("Click to zoom in; Alt-click to zoom out"),
                "Zoom"};
    case ViewerTool::Rectangle:
        return {tr("Rectangle"), QStringLiteral("R"),
                tr("Drag; Shift constrains; Alt draws from center"), "Rectangle"};
    case ViewerTool::Ellipse:
        return {tr("Ellipse"), QStringLiteral("E"),
                tr("Drag; Shift makes a circle; Alt draws from center"), "Ellipse"};
    case ViewerTool::Polygon:
        return {tr("Polygon"),
                {},
                tr("Drag a five-sided polygon; Shift constrains; Alt draws from center"),
                "Polygon"};
    case ViewerTool::Star:
        return {tr("Star"),
                {},
                tr("Drag a five-point star; Shift constrains; Alt draws from center"),
                "Star"};
    case ViewerTool::Line:
        return {tr("Line"), {}, tr("Drag; Shift snaps to 45 degrees; Alt draws from center"),
                "Line"};
    case ViewerTool::Pen:
        return {tr("Pen"), QStringLiteral("P"),
                tr("Click anchors; drag handles; click first to close; Enter or Escape finishes. "
                   "On a selected path: click a segment to add a point, an anchor to remove it, "
                   "an open end to continue"),
                "Pen"};
    case ViewerTool::Text:
        return {tr("Text"), QStringLiteral("T"), tr("Click to place and type on the canvas"),
                "Text"};
    case ViewerTool::Cryptomatte:
        return {tr("Cryptomatte"),
                {},
                tr("On a selected Cryptomatte, click or Shift-click adds the object under the "
                   "cursor; Alt- or Ctrl-click removes it"),
                "Cryptomatte"};
    }
    return {};
}

QString viewerToolTip(const ViewerTool tool) {
    const auto info = viewerToolInfo(tool);
    const auto title = info.key.isEmpty() ? info.name : tr("%1 (%2)").arg(info.name, info.key);
    return title + QLatin1Char('\n') + info.usage;
}

std::optional<ViewerTool> toolForKey(const int key) noexcept {
    switch (key) {
    case Qt::Key_V:
        return ViewerTool::Select;
    case Qt::Key_H:
        return ViewerTool::Hand;
    case Qt::Key_Z:
        return ViewerTool::Zoom;
    case Qt::Key_R:
        return ViewerTool::Rectangle;
    case Qt::Key_E:
        return ViewerTool::Ellipse;
    case Qt::Key_P:
        return ViewerTool::Pen;
    case Qt::Key_T:
        return ViewerTool::Text;
    default:
        return std::nullopt;
    }
}

commands::ShapeLayerStyle shapeLayerStyle(const NewShapeStyle& style) {
    return {style.fillEnabled, style.fillColor, style.strokeEnabled, style.strokeColor,
            style.strokeWidth};
}

ToolController::ToolController(QObject* parent) : QObject(parent) {}

void ToolController::setTool(const ViewerTool tool) {
    if (isShapeTool(tool))
        lastShapeTool_ = tool;
    if (tool == tool_)
        return;
    tool_ = tool;
    emit toolChanged(tool);
}

void ToolController::setNewShapeStyle(const NewShapeStyle& style) {
    if (style == shapeStyle_)
        return;
    shapeStyle_ = style;
    emit newShapeStyleChanged();
}

void ToolController::setNewTextStyle(const NewTextStyle& style) {
    if (style == textStyle_)
        return;
    textStyle_ = style;
    emit newTextStyleChanged();
}

void ToolController::setAlignmentTarget(const commands::AlignmentTarget target) {
    if (target == target_)
        return;
    target_ = target;
    emit alignmentTargetChanged();
}

} // namespace bloom::ui
