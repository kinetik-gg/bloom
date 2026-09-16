// Included inside bloom::ui. Paths stay document values; the canvas owns only gesture state.
namespace {
render::Path toolRenderPath(const document::PathValue& path) {
    render::Path result;
    result.closed = path.closed;
    for (const auto& anchor : path.anchors) {
        render::PathAnchor next{{anchor.point.x, anchor.point.y}, {}, {}};
        if (anchor.inHandle)
            next.inHandle = {anchor.inHandle->x, anchor.inHandle->y};
        if (anchor.outHandle)
            next.outHandle = {anchor.outHandle->x, anchor.outHandle->y};
        result.anchors.push_back(next);
    }
    return result;
}
QTransform toolScreenTransform(const ViewerMapping& mapping) {
    QTransform result;
    result.translate(mapping.displayRect.left(), mapping.displayRect.top());
    result.scale(mapping.displayRect.width() / mapping.compositionFormat.width(),
                 mapping.displayRect.height() / mapping.compositionFormat.height());
    return result;
}
} // namespace

std::optional<ViewerEditor::PathSelection> ViewerEditor::selectedPath() const {
    const auto* parameter = session_.parameterForSelection("path");
    const auto* kind = session_.parameterForSelection("kind");
    const auto kindValue = kind ? session_.liveValue(kind->id) : std::nullopt;
    if (!parameter || !kindValue || !std::holds_alternative<std::int64_t>(*kindValue) ||
        std::get<std::int64_t>(*kindValue) != static_cast<std::int64_t>(document::ShapeKind::Path))
        return {};
    const auto value = session_.liveValue(parameter->id);
    if (!value || !std::holds_alternative<document::PathValue>(*value))
        return {};
    if (pathDrag_ && pathDrag_->selection.parameter == parameter->id)
        return PathSelection{parameter->id, std::get<document::PathValue>(*value),
                             pathDrag_->selection.toWorld};
    const auto selected = previewController_.selectedLayerBounds();
    const auto layer = session_.selection().contextualLayer;
    const auto bounds = std::ranges::find(selected, layer.value_or(document::LayerId{}),
                                          &runtime::EvaluatedOperationBounds::layerId);
    if (bounds == selected.end() || bounds->local.empty())
        return {};
    const auto x = (toolPoint(bounds->polygon[1]) - toolPoint(bounds->polygon[0])) /
                   (bounds->local.right - bounds->local.left);
    const auto y = (toolPoint(bounds->polygon[3]) - toolPoint(bounds->polygon[0])) /
                   (bounds->local.bottom - bounds->local.top);
    const auto origin =
        toolPoint(bounds->polygon[0]) - x * bounds->local.left - y * bounds->local.top;
    const QTransform world(x.x(), x.y(), y.x(), y.y(), origin.x(), origin.y());
    if (!world.isInvertible())
        return {};
    return PathSelection{parameter->id, std::get<document::PathValue>(*value), world};
}

void ViewerEditor::cancelPathDrag() {
    if (!pathDrag_)
        return;
    pathDrag_.reset();
    session_.cancelValueEdit();
    previewController_.notifyScrubEnded();
    update();
}

bool ViewerEditor::pathPress(QMouseEvent* event) {
    if (pathDrag_ && event->button() != Qt::LeftButton) {
        cancelPathDrag();
        event->accept();
        return true;
    }
    if (event->button() != Qt::LeftButton || (tool_ != Tool::Pen && tool_ != Tool::Select))
        return false;
    const auto mapping = currentMapping();
    if (!mapping || !contentRect().contains(event->position()))
        return tool_ == Tool::Pen;
    if (!creation_) {
        if (const auto path = selectedPath()) {
            const auto transform = path->toWorld * toolScreenTransform(*mapping);
            const double radius = kit::px(kit::Size::GizmoHandle);
            for (std::size_t i = 0; i < path->path.anchors.size(); ++i) {
                const auto& anchor = path->path.anchors[i];
                const std::array<std::optional<document::Vec2d>, 3> points{
                    anchor.point, anchor.inHandle, anchor.outHandle};
                for (int part = 0; part < 3; ++part) {
                    const auto& point = points[static_cast<std::size_t>(part)];
                    if (!point ||
                        QLineF(transform.map(toolPoint(*point)), event->position()).length() >
                            radius)
                        continue;
                    selectedAnchor_ = std::pair(path->parameter, i);
                    playback_->pause();
                    if (!session_.beginPathEdit(path->parameter))
                        return true;
                    pathDrag_.emplace(
                        PathDrag{{*mapping, session_.snapshot().revision(), session_.currentTime(),
                                  session_.compositionId(), event->position(), event->position(),
                                  event->modifiers()},
                                 *path,
                                 event->position(),
                                 i,
                                 part});
                    previewController_.beginInteractiveScrub();
                    setFocus(Qt::MouseFocusReason);
                    update();
                    event->accept();
                    return true;
                }
            }
        }
        selectedAnchor_.reset();
    }
    if (tool_ != Tool::Pen)
        return false;
    if (creation_ && !creationValid())
        cancelCreation();
    if (!creation_) {
        playback_->pause();
        creation_.emplace(CreationGesture{
            *mapping, session_.snapshot().revision(), session_.currentTime(),
            session_.compositionId(), event->position(), event->position(), event->modifiers()});
    }
    if (penPath_.anchors.size() >= 3 &&
        QLineF(mapping->toScreen(penPath_.anchors.front().point), event->position()).length() <=
            kit::px(kit::Size::GizmoHandle)) {
        finishPen(true);
    } else if (penPath_.anchors.size() < document::kMaximumPathAnchors) {
        penPath_.anchors.push_back({mapping->toComposition(event->position()), {}, {}});
        creation_->origin = event->position();
        creation_->pointer = event->position();
        penDown_ = true;
    }
    setFocus(Qt::MouseFocusReason);
    update();
    event->accept();
    return true;
}

bool ViewerEditor::pathMove(QMouseEvent* event) {
    if (pathDrag_) {
        const auto mapping = currentMapping();
        const auto& drag = *pathDrag_;
        const auto* parameter = session_.parameterForSelection("path");
        if (!mapping || *mapping != drag.gesture.mapping ||
            session_.snapshot().revision() != drag.gesture.revision ||
            session_.currentTime() != drag.gesture.time || !parameter ||
            parameter->id != drag.selection.parameter || !session_.isValueEditing(parameter->id)) {
            cancelPathDrag();
            return true;
        }
        auto path = drag.selection.path;
        auto& anchor = path.anchors[drag.index];
        const auto inverse = drag.selection.toWorld.inverted();
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
        const auto centreDelta =
            toolOutline(toolRenderPath(path)).boundingRect().center() -
            toolOutline(toolRenderPath(drag.selection.path)).boundingRect().center();
        (void)session_.updatePathEdit(std::move(path), toolValue(centreDelta));
        update();
        event->accept();
        return true;
    }
    if (tool_ != Tool::Pen || !creation_)
        return false;
    if (!creationValid()) {
        cancelCreation();
        return true;
    }
    creation_->pointer = event->position();
    if (penDown_ && !penPath_.anchors.empty() &&
        QLineF(creation_->origin, event->position()).length() >=
            QApplication::startDragDistance()) {
        auto& anchor = penPath_.anchors.back();
        anchor.outHandle = creation_->mapping.toComposition(event->position());
        anchor.inHandle = toolValue(2 * toolPoint(anchor.point) - toolPoint(*anchor.outHandle));
    }
    update();
    event->accept();
    return true;
}

bool ViewerEditor::pathRelease(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton)
        return false;
    if (pathDrag_) {
        (void)pathMove(event);
        if (pathDrag_) {
            pathDrag_.reset();
            (void)session_.commitValueEdit();
            previewController_.notifyScrubEnded();
            update();
        }
        event->accept();
        return true;
    }
    if (tool_ != Tool::Pen || !creation_)
        return false;
    (void)pathMove(event);
    penDown_ = false;
    event->accept();
    return true;
}

void ViewerEditor::finishPen(bool closed) {
    if (!creationValid() || penPath_.anchors.size() < 2) {
        cancelCreation();
        return;
    }
    penPath_.closed = closed;
    commands::ShapeLayerGeometry geometry;
    geometry.path = penPath_;
    geometry.position = toolValue(toolOutline(toolRenderPath(penPath_)).boundingRect().center());
    cancelCreation();
    (void)session_.addShapeLayer(document::ShapeKind::Path, std::move(geometry));
}

bool ViewerEditor::pathKey(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape && (creation_ || pathDrag_)) {
        cancelCreation();
        selectTool(Tool::Select);
        event->accept();
        return true;
    }
    if (tool_ == Tool::Pen && creation_ &&
        (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)) {
        finishPen(false);
        event->accept();
        return true;
    }
    if (event->key() == Qt::Key_Delete && selectedAnchor_ && !pathDrag_) {
        auto path = selectedPath();
        if (path && path->parameter == selectedAnchor_->first &&
            selectedAnchor_->second < path->path.anchors.size()) {
            const auto baseCentre = toolOutline(toolRenderPath(path->path)).boundingRect().center();
            path->path.anchors.erase(path->path.anchors.begin() +
                                     static_cast<std::ptrdiff_t>(selectedAnchor_->second));
            if (path->path.anchors.size() < 3)
                path->path.closed = false;
            if (session_.beginPathEdit(path->parameter)) {
                const auto centreDelta =
                    toolOutline(toolRenderPath(path->path)).boundingRect().center() - baseCentre;
                (void)session_.updatePathEdit(path->path, toolValue(centreDelta));
                (void)session_.commitValueEdit();
            }
            selectedAnchor_.reset();
            update();
            event->accept();
            return true;
        }
        selectedAnchor_.reset();
    }
    return false;
}

void ViewerEditor::paintPathTools(QPainter& painter) const {
    const auto mapping = currentMapping();
    if (!mapping)
        return;
    std::optional<PathSelection> selected = selectedPath();
    const bool drawing = tool_ == Tool::Pen && creation_ && creationValid();
    if (drawing)
        selected = PathSelection{{}, penPath_, {}};
    if (!selected)
        return;
    const auto transform = selected->toWorld * toolScreenTransform(*mapping);
    painter.save();
    painter.setClipRect(contentRect());
    painter.setRenderHint(QPainter::Antialiasing);
    QPen pen(kit::color(kit::Color::Accent), kit::px(kit::Size::Hairline));
    pen.setCosmetic(true);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(transform.map(toolOutline(toolRenderPath(selected->path))));
    const auto half = kit::px(kit::Size::GizmoHandle) / 2.0;
    for (std::size_t i = 0; i < selected->path.anchors.size(); ++i) {
        const auto& anchor = selected->path.anchors[i];
        const auto point = transform.map(toolPoint(anchor.point));
        for (const auto& handle : {anchor.inHandle, anchor.outHandle}) {
            if (!handle)
                continue;
            const auto end = transform.map(toolPoint(*handle));
            painter.drawLine(point, end);
            painter.setBrush(kit::color(kit::Color::Surface));
            painter.drawEllipse(end, half, half);
        }
        painter.setBrush(kit::color(selectedAnchor_ &&
                                            selectedAnchor_->first == selected->parameter &&
                                            selectedAnchor_->second == i
                                        ? kit::Color::Accent
                                        : kit::Color::Surface));
        painter.drawRect(QRectF(point.x() - half, point.y() - half, 2 * half, 2 * half));
    }
    if (drawing && !penDown_ && !penPath_.anchors.empty()) {
        pen.setStyle(Qt::DashLine);
        painter.setPen(pen);
        painter.drawLine(mapping->toScreen(penPath_.anchors.back().point), creation_->pointer);
    }
    painter.restore();
}
