// Included inside bloom::ui. ADR 0028 layer masks: a mask path lives in the layer's CONTENT space,
// so every gesture maps through the inverse of the evaluated content-to-composition affine
// (masks::contentToComposition). Drawing a mask and picking one by its outline live here; editing
// its anchors is the shared path editing in viewer_tools_anchors.ipp.

using viewer_tool_geometry::toolOutline;
using viewer_tool_geometry::toolPoint;
using viewer_tool_geometry::toolRenderPath;
using viewer_tool_geometry::toolScreenTransform;
using viewer_tool_geometry::toolValue;

namespace {
std::optional<document::LayerId> maskContextLayer(const CompositionSession& session) {
    if (const auto* direct = std::get_if<document::LayerId>(&session.selection().primary))
        return *direct;
    return session.selection().contextualLayer;
}
} // namespace

std::optional<ViewerEditor::EditablePath> ViewerEditor::selectedMaskTarget() const {
    const auto selection = session_.selectedMask();
    const auto* composition = session_.composition();
    if (!selection || !composition)
        return {};
    const auto* layer = composition->graph().findLayer(selection->layer);
    if (layer == nullptr)
        return {};
    const auto mask = std::ranges::find(layer->masks, selection->mask, &document::LayerMask::id);
    if (mask == layer->masks.end())
        return {};
    const auto selected = previewController_.selectedLayerBounds();
    const auto bounds =
        std::ranges::find(selected, selection->layer, &runtime::EvaluatedOperationBounds::layerId);
    if (bounds == selected.end())
        return {};
    const auto world = masks::contentToComposition(*bounds);
    const auto path =
        masks::maskPathAt(*composition, *mask, session_.keyTimeAtPlayhead(mask->path));
    if (!world || !path)
        return {};
    EditablePath target;
    target.owner = EditablePath::Owner::Mask;
    target.parameter = mask->path;
    target.path = *path;
    target.toWorld = *world;
    target.layer = selection->layer;
    target.mask = selection->mask;
    target.feather = session_.effectiveScalarValue(mask->feather).value_or(0.0);
    target.expansion = session_.effectiveScalarValue(mask->expansion).value_or(0.0);
    return target;
}

std::optional<ViewerEditor::MaskCreationTarget> ViewerEditor::maskCreationTarget() const {
    const auto layer = maskContextLayer(session_);
    if (!layer || selectedPath())
        return {};
    MaskCreationTarget target{*layer, std::nullopt};
    const auto selected = previewController_.selectedLayerBounds();
    const auto bounds =
        std::ranges::find(selected, *layer, &runtime::EvaluatedOperationBounds::layerId);
    if (bounds != selected.end())
        if (const auto world = masks::contentToComposition(*bounds))
            target.toContent = world->inverted();
    return target;
}

bool ViewerEditor::addMaskFromPath(const MaskCreationTarget& target,
                                   const document::PathValue& path) {
    // A layer without evaluated bounds at this time (inactive, audio only) has no content space to
    // draw into; the gesture is dropped rather than guessed.
    if (!target.toContent)
        return false;
    return session_.addLayerMask(target.layer, masks::mappedPath(path, *target.toContent));
}

// Clicking a mask outline of the selected layer with the Select tool selects that mask.
bool ViewerEditor::maskOutlinePress(QMouseEvent* event, const ViewerMapping& mapping) {
    if (tool() != ViewerTool::Select)
        return false;
    const auto layerId = maskContextLayer(session_);
    const auto* composition = session_.composition();
    const auto* layer = layerId && composition ? composition->graph().findLayer(*layerId) : nullptr;
    const auto selected = previewController_.selectedLayerBounds();
    const auto bounds =
        layerId ? std::ranges::find(selected, *layerId, &runtime::EvaluatedOperationBounds::layerId)
                : selected.end();
    const auto world =
        bounds != selected.end() ? masks::contentToComposition(*bounds) : std::nullopt;
    if (layer == nullptr || !world)
        return false;
    const auto transform = *world * toolScreenTransform(mapping);
    QPainterPathStroker stroker;
    stroker.setWidth(2 * kit::px(kit::Size::GizmoHandle));
    for (const auto& mask : layer->masks) {
        const auto path =
            masks::maskPathAt(*composition, mask, session_.keyTimeAtPlayhead(mask.path));
        if (!path ||
            !stroker.createStroke(transform.map(masks::outline(*path))).contains(event->position()))
            continue;
        session_.selectMask(layer->layerId, mask.id);
        selectedAnchors_.clear();
        event->accept();
        return true;
    }
    return false;
}
