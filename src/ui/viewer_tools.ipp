// Included inside bloom::ui. Creation previews are bounded vector outlines, never raster work.
namespace {
QPointF toolPoint(document::Vec2d point) { return {point.x, point.y}; }
document::Vec2d toolValue(QPointF point) { return {point.x(), point.y()}; }
QPainterPath toolOutline(const render::Path& path) {
    QPainterPath outline;
    if (path.anchors.empty())
        return outline;
    const auto point = [](render::PathPoint p) { return QPointF(p.x, p.y); };
    outline.moveTo(point(path.anchors.front().point));
    const auto segments = path.closed ? path.anchors.size() : path.anchors.size() - 1;
    for (std::size_t i = 0; i < segments; ++i) {
        const auto& a = path.anchors[i];
        const auto& b = path.anchors[(i + 1) % path.anchors.size()];
        outline.cubicTo(point(a.outHandle.value_or(a.point)), point(b.inHandle.value_or(b.point)),
                        point(b.point));
    }
    if (path.closed)
        outline.closeSubpath();
    return outline;
}
} // namespace

void ViewerEditor::cancelCreation() {
    cancelPathDrag();
    penPath_ = {};
    penDown_ = false;
    creation_.reset();
    update();
}

bool ViewerEditor::creationValid() const {
    if (!creation_ || creation_->revision != session_.snapshot().revision() ||
        creation_->time != session_.currentTime() ||
        creation_->composition != session_.compositionId())
        return false;
    const auto mapping = currentMapping();
    return mapping && *mapping == creation_->mapping;
}

document::ShapeKind ViewerEditor::creationKind() const {
    switch (tool_) {
    case Tool::Ellipse:
        return document::ShapeKind::Ellipse;
    case Tool::Polygon:
        return document::ShapeKind::Polygon;
    case Tool::Star:
        return document::ShapeKind::Star;
    case Tool::Line:
        return document::ShapeKind::Line;
    case Tool::Pen:
        return document::ShapeKind::Path;
    default:
        return document::ShapeKind::Rectangle;
    }
}

commands::ShapeLayerGeometry ViewerEditor::creationGeometry() const {
    if (!creation_)
        return {};
    const auto& gesture = *creation_;
    const auto start = toolPoint(gesture.mapping.toComposition(gesture.origin));
    auto delta = toolPoint(gesture.mapping.toComposition(gesture.pointer)) - start;
    const bool click =
        QLineF(gesture.origin, gesture.pointer).length() < QApplication::startDragDistance();
    const bool line = tool_ == Tool::Line;
    if (click)
        delta = line ? QPointF(100, 0) : QPointF(100, 100);
    if (gesture.modifiers.testFlag(Qt::ShiftModifier)) {
        if (line) {
            const double length = std::hypot(delta.x(), delta.y());
            const double step = std::numbers::pi / 4;
            const double angle = std::round(std::atan2(delta.y(), delta.x()) / step) * step;
            delta = {length * std::cos(angle), length * std::sin(angle)};
        } else {
            // The five-point families have unequal natural X/Y bounds. Preserve their
            // circumcircle proportions, not a square that would stretch a regular pentagon.
            const double ratio =
                (tool_ == Tool::Polygon || tool_ == Tool::Star)
                    ? (2 * std::cos(std::numbers::pi / 10)) / (1 + std::cos(std::numbers::pi / 5))
                    : 1.0;
            const double height = std::max(std::abs(delta.x()) / ratio, std::abs(delta.y()));
            delta = {std::copysign(height * ratio, delta.x()), std::copysign(height, delta.y())};
        }
    }
    QPointF first = start;
    QPointF last = start + delta;
    if (click) {
        first = start - delta / 2;
        last = start + delta / 2;
    } else if (gesture.modifiers.testFlag(Qt::AltModifier)) {
        first = start - delta;
    }
    commands::ShapeLayerGeometry geometry;
    geometry.position = toolValue((first + last) / 2);
    if (line) {
        geometry.lineStart = toolValue(first);
        geometry.lineEnd = toolValue(last);
    } else {
        geometry.size =
            document::Vec2d{std::abs(last.x() - first.x()), std::abs(last.y() - first.y())};
    }
    return geometry;
}

bool ViewerEditor::creationPress(QMouseEvent* event) {
    if (creation_ && event->button() != Qt::LeftButton) {
        cancelCreation();
        event->accept();
        return true;
    }
    if (tool_ < Tool::Rectangle || tool_ > Tool::Line || event->button() != Qt::LeftButton)
        return false;
    const auto mapping = currentMapping();
    if (!mapping || !contentRect().contains(event->position()))
        return true;
    playback_->pause();
    setFocus(Qt::MouseFocusReason);
    creation_.emplace(CreationGesture{*mapping, session_.snapshot().revision(),
                                      session_.currentTime(), session_.compositionId(),
                                      event->position(), event->position(), event->modifiers()});
    update();
    event->accept();
    return true;
}

bool ViewerEditor::creationMove(QMouseEvent* event) {
    if (!creation_)
        return false;
    if (!creationValid())
        cancelCreation();
    else {
        creation_->pointer = event->position();
        creation_->modifiers = event->modifiers();
        update();
    }
    event->accept();
    return true;
}

bool ViewerEditor::creationRelease(QMouseEvent* event) {
    if (!creation_ || event->button() != Qt::LeftButton)
        return false;
    if (creationValid()) {
        creation_->pointer = event->position();
        creation_->modifiers = event->modifiers();
        const auto geometry = creationGeometry();
        const auto kind = creationKind();
        cancelCreation();
        (void)session_.addShapeLayer(kind, geometry);
    } else
        cancelCreation();
    event->accept();
    return true;
}

void ViewerEditor::paintCreation(QPainter& painter) const {
    if (tool_ == Tool::Pen || !creation_ || !creationValid())
        return;
    const auto geometry = creationGeometry();
    render::Path path;
    if (tool_ == Tool::Line) {
        if (!geometry.lineStart || !geometry.lineEnd)
            return;
        path = render::linePath({geometry.lineStart->x, geometry.lineStart->y},
                                {geometry.lineEnd->x, geometry.lineEnd->y});
    } else {
        if (!geometry.size || !geometry.position)
            return;
        const auto size = *geometry.size;
        switch (tool_) {
        case Tool::Ellipse:
            path = render::ellipsePath(size.x, size.y);
            break;
        case Tool::Polygon:
            path = render::polygonPath(size.x, size.y, 5);
            break;
        case Tool::Star:
            path = render::starPath(size.x, size.y, 5, 0.5);
            break;
        default:
            path = render::rectanglePath(size.x, size.y);
            break;
        }
        const auto offset = toolPoint(*geometry.position) - QPointF(size.x, size.y) / 2;
        for (auto& a : path.anchors) {
            const auto translate = [offset](render::PathPoint& p) {
                p.x += offset.x();
                p.y += offset.y();
            };
            translate(a.point);
            if (a.inHandle)
                translate(*a.inHandle);
            if (a.outHandle)
                translate(*a.outHandle);
        }
    }
    const auto& mapping = creation_->mapping;
    QTransform toScreen;
    toScreen.translate(mapping.displayRect.left(), mapping.displayRect.top());
    toScreen.scale(mapping.displayRect.width() / mapping.compositionFormat.width(),
                   mapping.displayRect.height() / mapping.compositionFormat.height());
    painter.save();
    painter.setClipRect(contentRect());
    painter.setRenderHint(QPainter::Antialiasing);
    QPen pen(kit::color(kit::Color::Accent), kit::px(kit::Size::Hairline));
    pen.setCosmetic(true);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(toScreen.map(toolOutline(path)));
    painter.restore();
}

bool ViewerEditor::textPress(QMouseEvent* event) {
    if (tool_ != Tool::Text || event->button() != Qt::LeftButton)
        return false;
    const auto mapping = currentMapping();
    if (!mapping || !contentRect().contains(event->position()))
        return true;
    playback_->pause();
    if (session_.addTextLayer(tr("Text"), tr("Text"), document::kDefaultTextSizePixels,
                              {1, 1, 1, 1}, mapping->toComposition(event->position()))) {
        const auto layer = session_.selection().contextualLayer;
        // Properties rebuilds synchronously on selection, then the layout settles on this turn.
        // Use its existing public widget identities: this tool does not own text editing.
        QTimer::singleShot(0, this, [this, layer] {
            if (session_.selection().contextualLayer != layer)
                return;
            for (auto* panel : window()->findChildren<QWidget*>("propertiesEditor")) {
                if (!panel->isVisible())
                    continue;
                QWidget* searchScope = panel;
                for (auto* ancestor = panel->parentWidget(); ancestor;
                     ancestor = ancestor->parentWidget()) {
                    if (qobject_cast<EditorArea*>(ancestor)) {
                        searchScope = ancestor;
                        break;
                    }
                }
                if (auto* search = searchScope->findChild<QLineEdit*>("propertiesSearchField"))
                    search->clear();
                auto* field = panel->findChild<QLineEdit*>("textContentEditor");
                if (!field || !field->isEnabled())
                    continue;
                for (auto* ancestor = field->parentWidget(); ancestor && ancestor != panel;
                     ancestor = ancestor->parentWidget())
                    if (auto* section = qobject_cast<kit::KSection*>(ancestor))
                        section->setCollapsed(false);
                field->setFocus(Qt::OtherFocusReason);
                field->selectAll();
                if (auto* scroll = panel->findChild<QScrollArea*>("propertiesScrollArea"))
                    scroll->ensureWidgetVisible(field);
                return;
            }
        });
    }
    event->accept();
    return true;
}
