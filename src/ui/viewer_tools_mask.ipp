// Included inside bloom::ui. ADR 0028 layer masks: a mask path lives in
// the layer's CONTENT space, so every gesture maps through the inverse of the evaluated
// content-to-composition affine (masks::contentToComposition).

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

std::optional<ViewerEditor::MaskTarget> ViewerEditor::selectedMaskTarget() const {
    const auto selection = session_.selectedMask();
    const auto* composition = session_.composition();
    if (!selection || !composition)
        return {};
    if (maskDrag_ && maskDrag_->target.mask == selection->mask)
        return maskDrag_->target;
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
    return MaskTarget{selection->layer,
                      selection->mask,
                      *path,
                      *world,
                      session_.effectiveScalarValue(mask->feather).value_or(0.0),
                      session_.effectiveScalarValue(mask->expansion).value_or(0.0)};
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

void ViewerEditor::cancelMaskDrag() {
    if (!maskDrag_)
        return;
    maskDrag_.reset();
    update();
}

bool ViewerEditor::maskPress(QMouseEvent* event) {
    if (maskDrag_ && event->button() != Qt::LeftButton) {
        cancelMaskDrag();
        event->accept();
        return true;
    }
    if (event->button() != Qt::LeftButton || (tool_ != Tool::Select && tool_ != Tool::Pen) ||
        creation_ || textEdit_)
        return false;
    const auto mapping = currentMapping();
    if (!mapping || !contentRect().contains(event->position()))
        return false;
    const double radius = kit::px(kit::Size::GizmoHandle);
    if (const auto target = selectedMaskTarget()) {
        const auto transform = target->toWorld * toolScreenTransform(*mapping);
        for (std::size_t i = 0; i < target->path.anchors.size(); ++i) {
            const auto& anchor = target->path.anchors[i];
            const std::array<std::optional<document::Vec2d>, 3> points{
                anchor.point, anchor.inHandle, anchor.outHandle};
            for (int part = 0; part < 3; ++part) {
                const auto& point = points[static_cast<std::size_t>(part)];
                if (!point ||
                    QLineF(transform.map(toolPoint(*point)), event->position()).length() > radius)
                    continue;
                selectedMaskAnchor_ = i;
                playback_->pause();
                maskDrag_.emplace(
                    MaskDrag{{*mapping, session_.snapshot().revision(), session_.currentTime(),
                              session_.compositionId(), event->position(), event->position(),
                              event->modifiers()},
                             *target,
                             target->path,
                             event->position(),
                             i,
                             part});
                setFocus(Qt::MouseFocusReason);
                update();
                event->accept();
                return true;
            }
        }
    }
    // Clicking a mask outline of the selected layer selects it.
    if (tool_ == Tool::Select) {
        const auto layerId = maskContextLayer(session_);
        const auto* composition = session_.composition();
        const auto* layer =
            layerId && composition ? composition->graph().findLayer(*layerId) : nullptr;
        const auto selected = previewController_.selectedLayerBounds();
        const auto bounds = layerId ? std::ranges::find(selected, *layerId,
                                                        &runtime::EvaluatedOperationBounds::layerId)
                                    : selected.end();
        const auto world =
            bounds != selected.end() ? masks::contentToComposition(*bounds) : std::nullopt;
        if (layer != nullptr && world) {
            const auto transform = *world * toolScreenTransform(*mapping);
            QPainterPathStroker stroker;
            stroker.setWidth(2 * radius);
            for (const auto& mask : layer->masks) {
                const auto path =
                    masks::maskPathAt(*composition, mask, session_.keyTimeAtPlayhead(mask.path));
                if (!path || !stroker.createStroke(transform.map(masks::outline(*path)))
                                  .contains(event->position()))
                    continue;
                session_.selectMask(layer->layerId, mask.id);
                selectedMaskAnchor_.reset();
                event->accept();
                return true;
            }
        }
    }
    selectedMaskAnchor_.reset();
    return false;
}

bool ViewerEditor::maskMove(QMouseEvent* event) {
    if (!maskDrag_)
        return false;
    const auto mapping = currentMapping();
    auto& drag = *maskDrag_;
    if (!mapping || *mapping != drag.gesture.mapping ||
        session_.snapshot().revision() != drag.gesture.revision ||
        session_.currentTime() != drag.gesture.time) {
        cancelMaskDrag();
        return true;
    }
    auto path = drag.base;
    auto& anchor = path.anchors[drag.index];
    const auto inverse = drag.target.toWorld.inverted();
    const auto delta = inverse.map(toolPoint(mapping->toComposition(event->position()))) -
                       inverse.map(toolPoint(mapping->toComposition(drag.origin)));
    if (drag.part == 0) {
        anchor.point = toolValue(toolPoint(anchor.point) + delta);
        if (anchor.inHandle)
            anchor.inHandle = toolValue(toolPoint(*anchor.inHandle) + delta);
        if (anchor.outHandle)
            anchor.outHandle = toolValue(toolPoint(*anchor.outHandle) + delta);
    } else {
        auto& handle = drag.part == 1 ? anchor.inHandle : anchor.outHandle;
        auto& opposite = drag.part == 1 ? anchor.outHandle : anchor.inHandle;
        if (!handle)
            return true;
        handle = toolValue(toolPoint(*handle) + delta);
        if (!event->modifiers().testFlag(Qt::AltModifier))
            opposite = toolValue(2 * toolPoint(anchor.point) - toolPoint(*handle));
    }
    drag.target.path = std::move(path);
    update();
    event->accept();
    return true;
}

void ViewerEditor::commitMaskDrag() {
    if (!maskDrag_)
        return;
    const auto drag = std::move(*maskDrag_);
    maskDrag_.reset();
    // A zero-delta gesture commits nothing; one drag is exactly one SetLayerMaskPath.
    if (drag.target.path != drag.base)
        (void)session_.commitMaskPath(drag.target.layer, drag.target.mask, drag.target.path);
    update();
}

bool ViewerEditor::maskRelease(QMouseEvent* event) {
    if (!maskDrag_ || event->button() != Qt::LeftButton)
        return false;
    (void)maskMove(event);
    commitMaskDrag();
    event->accept();
    return true;
}

void ViewerEditor::paintMaskTools(QPainter& painter) const {
    const auto mapping = currentMapping();
    const auto target = selectedMaskTarget();
    if (!mapping || !target)
        return;
    const auto transform = target->toWorld * toolScreenTransform(*mapping);
    painter.save();
    painter.setClipRect(contentRect());
    painter.setRenderHint(QPainter::Antialiasing);
    QPen pen(kit::color(kit::Color::Accent), kit::px(kit::Size::Hairline));
    pen.setCosmetic(true);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(transform.map(masks::outline(target->path)));
    // Feather bounds at expansion +/- feather / 2, in content pixels, mapped with the outline.
    if (target->feather > 0.0) {
        QPen dashed = pen;
        dashed.setStyle(Qt::DashLine);
        painter.setPen(dashed);
        for (const double sign : {1.0, -1.0})
            painter.drawPath(transform.map(masks::offsetOutline(
                target->path, target->expansion + sign * target->feather / 2.0)));
        painter.setPen(pen);
    } else if (target->expansion != 0.0) {
        QPen dashed = pen;
        dashed.setStyle(Qt::DashLine);
        painter.setPen(dashed);
        painter.drawPath(transform.map(masks::offsetOutline(target->path, target->expansion)));
        painter.setPen(pen);
    }
    const auto half = kit::px(kit::Size::GizmoHandle) / 2.0;
    for (std::size_t i = 0; i < target->path.anchors.size(); ++i) {
        const auto& anchor = target->path.anchors[i];
        const auto point = transform.map(toolPoint(anchor.point));
        for (const auto& handle : {anchor.inHandle, anchor.outHandle}) {
            if (!handle)
                continue;
            const auto end = transform.map(toolPoint(*handle));
            painter.drawLine(point, end);
            painter.setBrush(kit::color(kit::Color::Surface));
            painter.drawEllipse(end, half, half);
        }
        painter.setBrush(kit::color(selectedMaskAnchor_ && *selectedMaskAnchor_ == i
                                        ? kit::Color::Accent
                                        : kit::Color::Surface));
        painter.drawRect(QRectF(point.x() - half, point.y() - half, 2 * half, 2 * half));
    }
    painter.restore();
}
