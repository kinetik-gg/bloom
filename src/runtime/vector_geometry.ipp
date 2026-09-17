// Included inside the evaluator namespace; no project or cache ownership.
render::Path shapePath(const CompiledShape& shape, document::Vec2d size) {
    render::Path path;
    const auto w = size.x, h = size.y;
    switch (shape.kind) {
    case document::ShapeKind::Rectangle:
        path = render::rectanglePath(w, h, shape.cornerRadius);
        break;
    case document::ShapeKind::Ellipse:
        path = render::ellipsePath(w, h);
        break;
    case document::ShapeKind::Triangle:
        path = render::polygonPath(w, h, 3);
        break;
    case document::ShapeKind::Polygon:
        path =
            render::polygonPath(w, h, static_cast<std::uint32_t>(shape.points), shape.cornerRadius);
        break;
    case document::ShapeKind::Star:
        path = render::starPath(w, h, static_cast<std::uint32_t>(shape.points), shape.innerRatio);
        break;
    case document::ShapeKind::Line:
        path = render::linePath({shape.lineStart.x, shape.lineStart.y},
                                {shape.lineEnd.x, shape.lineEnd.y});
        break;
    case document::ShapeKind::Path:
        path.closed = shape.path.closed;
        for (const auto& anchor : shape.path.anchors) {
            render::PathAnchor a{{anchor.point.x, anchor.point.y}, {}, {}};
            if (anchor.inHandle)
                a.inHandle = render::PathPoint{anchor.inHandle->x, anchor.inHandle->y};
            if (anchor.outHandle)
                a.outHandle = render::PathPoint{anchor.outHandle->x, anchor.outHandle->y};
            path.anchors.push_back(a);
        }
        break;
    }
    return path;
}
