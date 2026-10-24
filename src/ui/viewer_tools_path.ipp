// Included inside bloom::ui. Paths stay document values; the canvas owns only gesture state. This
// file is the Pen: drawing a new path, and continuing an open shape path from one of its ends.
// Editing a confirmed path (masks and shape paths alike) lives in viewer_tools_anchors.ipp.
using viewer_tool_geometry::toolOutline;
using viewer_tool_geometry::toolPoint;
using viewer_tool_geometry::toolRenderPath;
using viewer_tool_geometry::toolScreenTransform;
using viewer_tool_geometry::toolValue;

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

document::Vec2d ViewerEditor::penPoint(const ViewerMapping& mapping, const QPointF screen) const {
    const auto composition = mapping.toComposition(screen);
    if (!penContinuation_)
        return composition;
    return toolValue(penContinuation_->toWorld.inverted().map(toolPoint(composition)));
}

QTransform ViewerEditor::penScreenTransform(const ViewerMapping& mapping) const {
    return penContinuation_ ? penContinuation_->toWorld * toolScreenTransform(mapping)
                            : toolScreenTransform(mapping);
}

bool ViewerEditor::beginPenContinuation(const EditablePath& target, const std::size_t endpoint,
                                        const ViewerMapping& mapping, const QPointF screen) {
    if (target.owner != EditablePath::Owner::Shape || target.path.closed ||
        target.path.anchors.size() < 2 ||
        (endpoint != 0 && endpoint + 1 != target.path.anchors.size()))
        return false;
    playback_->pause();
    penContinuation_ = target;
    penPath_ = target.path;
    // Drawing always appends; continuing from the first anchor reverses the path first.
    if (endpoint == 0)
        path_editing::reverse(penPath_);
    creation_.emplace(CreationGesture{mapping, session_.snapshot().revision(),
                                      session_.currentTime(), session_.compositionId(), screen,
                                      screen, Qt::NoModifier});
    penDown_ = false;
    selectedAnchors_.clear();
    update();
    return true;
}

bool ViewerEditor::pathPress(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || tool_ != Tool::Pen)
        return false;
    const auto mapping = currentMapping();
    if (!mapping || !contentRect().contains(event->position()))
        return true;
    if (creation_ && !creationValid())
        cancelCreation();
    if (!creation_) {
        playback_->pause();
        creation_.emplace(CreationGesture{
            *mapping, session_.snapshot().revision(), session_.currentTime(),
            session_.compositionId(), event->position(), event->position(), event->modifiers()});
    }
    const auto screen = penScreenTransform(*mapping);
    if (penPath_.anchors.size() >= 3 &&
        QLineF(screen.map(toolPoint(penPath_.anchors.front().point)), event->position()).length() <=
            kit::px(kit::Size::GizmoHandle)) {
        if (finishPen(true))
            selectTool(Tool::Select);
    } else if (penPath_.anchors.size() < document::kMaximumPathAnchors) {
        penPath_.anchors.push_back({penPoint(*mapping, event->position()), {}, {}});
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
        // Dragging while placing an anchor pulls symmetric handles: a Mirrored anchor.
        path_editing::pullHandles(penPath_, penPath_.anchors.size() - 1,
                                  penPoint(creation_->mapping, event->position()));
    }
    update();
    event->accept();
    return true;
}

bool ViewerEditor::pathRelease(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || tool_ != Tool::Pen || !creation_)
        return false;
    (void)pathMove(event);
    penDown_ = false;
    event->accept();
    return true;
}

bool ViewerEditor::finishPen(bool closed) {
    if (!creationValid() || penPath_.anchors.size() < 2) {
        cancelCreation();
        return false;
    }
    // Continuing an existing open shape path: the extended path is one edit of that path.
    if (penContinuation_) {
        const auto target = *penContinuation_;
        auto drawn = penPath_;
        drawn.closed = closed && drawn.anchors.size() >= 3;
        cancelCreation();
        return commitPathEdit(target, std::move(drawn));
    }
    // ADR 0028: with a layer selected, the Pen draws a mask on it instead of a shape layer. A mask
    // is an area, so its path always closes; with fewer than three anchors there is no area and
    // nothing is created.
    if (const auto maskTarget = maskCreationTarget()) {
        auto drawn = penPath_;
        cancelCreation();
        if (drawn.anchors.size() < 3)
            return false;
        drawn.closed = true;
        return addMaskFromPath(*maskTarget, drawn);
    }
    penPath_.closed = closed;
    commands::ShapeLayerGeometry geometry;
    geometry.path = penPath_;
    geometry.position = toolValue(toolOutline(toolRenderPath(penPath_)).boundingRect().center());
    cancelCreation();
    return session_.addShapeLayer(document::ShapeKind::Path, std::move(geometry));
}

bool ViewerEditor::pathKey(QKeyEvent* event) {
    // Escape confirms what already exists and returns to Select; discarding is Undo. A drag in
    // progress commits at its current size, the Pen finishes open with the anchors placed so far,
    // and an anchor gesture keeps its edit as one command. Empty or zero-size work creates nothing.
    if (event->key() == Qt::Key_Escape && (creation_ || anchorGesture_)) {
        commitAnchorGesture();
        if (creation_) {
            if (tool_ == Tool::Pen)
                (void)finishPen(false);
            else
                (void)commitCreation();
        }
        selectTool(Tool::Select);
        event->accept();
        return true;
    }
    if (tool_ == Tool::Pen && creation_ &&
        (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)) {
        if (finishPen(false))
            selectTool(Tool::Select);
        event->accept();
        return true;
    }
    return anchorKey(event);
}

void ViewerEditor::paintPathTools(QPainter& painter) const {
    const auto mapping = currentMapping();
    if (!mapping || tool_ != Tool::Pen || !creation_ || !creationValid())
        return;
    const auto transform = penScreenTransform(*mapping);
    painter.save();
    painter.setClipRect(contentRect());
    painter.setRenderHint(QPainter::Antialiasing);
    QPen pen(kit::color(kit::Color::Accent), kit::px(kit::Size::Hairline));
    pen.setCosmetic(true);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(transform.map(toolOutline(toolRenderPath(penPath_))));
    const auto half = kit::px(kit::Size::GizmoHandle) / 2.0;
    for (const auto& anchor : penPath_.anchors) {
        const auto point = transform.map(toolPoint(anchor.point));
        for (const auto& handle : {anchor.inHandle, anchor.outHandle}) {
            if (!handle)
                continue;
            const auto end = transform.map(toolPoint(*handle));
            painter.drawLine(point, end);
            painter.setBrush(kit::color(kit::Color::Surface));
            painter.drawEllipse(end, half, half);
        }
        painter.setBrush(kit::color(kit::Color::Surface));
        painter.drawRect(QRectF(point.x() - half, point.y() - half, 2 * half, 2 * half));
    }
    if (!penDown_ && !penPath_.anchors.empty()) {
        pen.setStyle(Qt::DashLine);
        painter.setPen(pen);
        painter.drawLine(transform.map(toolPoint(penPath_.anchors.back().point)),
                         creation_->pointer);
    }
    painter.restore();
}
