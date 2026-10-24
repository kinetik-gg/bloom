// Included inside bloom::ui. Post-confirmation anchor editing, one implementation for every path
// the Viewer edits: a Pen shape-layer path and a layer mask alike (EditablePath). Gestures edit a
// copy in the path's own space through path_editing, preview it through the session-only
// parameter override, and commit one command on release: SetLayerMaskPath for a mask (keyed at the
// playhead when the path is animated) or a path value edit for a shape layer.

namespace {
constexpr std::array<document::PathAnchorMode, 5> kAnchorModes{
    document::PathAnchorMode::Corner, document::PathAnchorMode::Mirrored,
    document::PathAnchorMode::Aligned, document::PathAnchorMode::Free,
    document::PathAnchorMode::Auto};

QString anchorModeText(const document::PathAnchorMode mode) {
    switch (mode) {
    case document::PathAnchorMode::Corner:
        return QCoreApplication::translate("ViewerEditor", "Corner");
    case document::PathAnchorMode::Mirrored:
        return QCoreApplication::translate("ViewerEditor", "Mirrored");
    case document::PathAnchorMode::Aligned:
        return QCoreApplication::translate("ViewerEditor", "Aligned");
    case document::PathAnchorMode::Free:
        return QCoreApplication::translate("ViewerEditor", "Free");
    case document::PathAnchorMode::Auto:
        return QCoreApplication::translate("ViewerEditor", "Auto");
    }
    return {};
}

QPointF pathCentre(const document::PathValue& path) {
    return viewer_tool_geometry::toolOutline(viewer_tool_geometry::toolRenderPath(path))
        .boundingRect()
        .center();
}
} // namespace

std::optional<ViewerEditor::EditablePath> ViewerEditor::editablePath() const {
    if (anchorGesture_)
        return anchorGesture_->target;
    if (auto mask = selectedMaskTarget())
        return mask;
    if (const auto shape = selectedPath()) {
        EditablePath target;
        target.owner = EditablePath::Owner::Shape;
        target.parameter = shape->parameter;
        target.path = shape->path;
        target.toWorld = shape->toWorld;
        return target;
    }
    return {};
}

std::optional<std::pair<document::PathValue, QTransform>>
ViewerEditor::anchorEditTargetForTest() const {
    const auto target = editablePath();
    if (!target)
        return std::nullopt;
    return std::pair{target->path, target->toWorld};
}

std::unique_ptr<QMenu> ViewerEditor::anchorContextMenuForTest() { return anchorContextMenu(); }

path_editing::AnchorSet ViewerEditor::selectedAnchorsForTest() const {
    const auto target = editablePath();
    return target ? anchorSelectionFor(*target) : path_editing::AnchorSet{};
}

path_editing::AnchorSet ViewerEditor::anchorSelectionFor(const EditablePath& target) const {
    path_editing::AnchorSet result;
    if (selectedAnchorsParameter_ != target.parameter)
        return result;
    for (const auto index : selectedAnchors_)
        if (index < target.path.anchors.size())
            result.insert(index);
    return result;
}

void ViewerEditor::setAnchorSelection(const EditablePath& target, path_editing::AnchorSet anchors) {
    selectedAnchorsParameter_ = target.parameter;
    selectedAnchors_ = std::move(anchors);
    refreshAnchorModeSwitcher();
    update();
}

std::optional<ViewerEditor::AnchorHit> ViewerEditor::hitAnchor(const EditablePath& target,
                                                               const QTransform& screen,
                                                               const QPointF position) const {
    const double radius = kit::px(kit::Size::GizmoHandle);
    for (std::size_t index = 0; index < target.path.anchors.size(); ++index) {
        const auto& anchor = target.path.anchors[index];
        const std::array<std::optional<document::Vec2d>, 3> points{anchor.point, anchor.inHandle,
                                                                   anchor.outHandle};
        for (int part = 0; part < 3; ++part) {
            const auto& point = points[static_cast<std::size_t>(part)];
            if (point && QLineF(screen.map(toolPoint(*point)), position).length() <= radius)
                return AnchorHit{index, part};
        }
    }
    return std::nullopt;
}

void ViewerEditor::previewPathEdit(AnchorGesture& gesture, const document::PathValue& next) {
    auto& target = gesture.target;
    if (!gesture.previewing) {
        const bool started = target.owner == EditablePath::Owner::Shape
                                 ? session_.beginPathEdit(target.parameter)
                                 : session_.beginMaskPathEdit(target.parameter, gesture.base);
        if (!started)
            return;
        gesture.previewing = true;
        previewController_.beginInteractiveScrub();
    }
    if (target.owner == EditablePath::Owner::Shape)
        (void)session_.updatePathEdit(next, toolValue(pathCentre(next) - pathCentre(gesture.base)));
    else
        (void)session_.updateValueEdit(next);
}

void ViewerEditor::endPathPreview(AnchorGesture& gesture) {
    if (!gesture.previewing)
        return;
    if (session_.isValueEditing(gesture.target.parameter))
        session_.cancelValueEdit();
    previewController_.notifyScrubEnded();
    gesture.previewing = false;
}

bool ViewerEditor::commitPathEdit(const EditablePath& target, document::PathValue next) {
    if (next == target.path || !next.isValid())
        return false;
    if (target.owner == EditablePath::Owner::Mask)
        return target.layer && target.mask &&
               session_.commitMaskPath(*target.layer, *target.mask, std::move(next));
    if (!session_.beginPathEdit(target.parameter))
        return false;
    const auto delta = toolValue(pathCentre(next) - pathCentre(target.path));
    if (!session_.updatePathEdit(std::move(next), delta)) {
        session_.cancelValueEdit();
        return false;
    }
    return session_.commitValueEdit();
}

void ViewerEditor::cancelAnchorGesture() {
    if (!anchorGesture_)
        return;
    auto gesture = std::move(*anchorGesture_);
    anchorGesture_.reset();
    endPathPreview(gesture);
    update();
}

void ViewerEditor::commitAnchorGesture() {
    if (!anchorGesture_)
        return;
    auto gesture = std::move(*anchorGesture_);
    anchorGesture_.reset();
    endPathPreview(gesture);
    auto original = gesture.target;
    original.path = gesture.base;
    switch (gesture.kind) {
    case AnchorGesture::Kind::Marquee: {
        if (!gesture.moved) {
            // A plain click on empty canvas keeps its meaning: deselect.
            selectedAnchors_.clear();
            session_.clearSelection();
            break;
        }
        const auto mapping = currentMapping();
        if (!mapping)
            break;
        const auto screen = original.toWorld * toolScreenTransform(*mapping);
        const auto rect = QRectF(gesture.gesture.origin, gesture.gesture.pointer).normalized();
        auto anchors = gesture.additive ? anchorSelectionFor(original) : path_editing::AnchorSet{};
        for (std::size_t index = 0; index < original.path.anchors.size(); ++index)
            if (rect.contains(screen.map(toolPoint(original.path.anchors[index].point))))
                anchors.insert(index);
        setAnchorSelection(original, std::move(anchors));
        break;
    }
    case AnchorGesture::Kind::AltPoint:
        if (!gesture.moved) {
            // Alt-click: Corner <-> the anchor's previous curved mode (Mirrored the first time).
            const auto key = std::pair{gesture.target.parameter.value(), gesture.index};
            const auto mode = path_editing::modeOf(gesture.base.anchors[gesture.index]);
            if (mode != document::PathAnchorMode::Corner)
                curvedModeMemory_[key] = mode;
            const auto remembered = curvedModeMemory_.find(key);
            auto next = gesture.base;
            path_editing::toggleCorner(next, gesture.index,
                                       remembered != curvedModeMemory_.end()
                                           ? remembered->second
                                           : document::PathAnchorMode::Mirrored);
            (void)commitPathEdit(original, std::move(next));
            break;
        }
        [[fallthrough]];
    case AnchorGesture::Kind::Move:
    case AnchorGesture::Kind::Handle:
        if (gesture.moved)
            (void)commitPathEdit(original, gesture.target.path);
        else if (gesture.kind == AnchorGesture::Kind::Move)
            // A click on an anchor of a multi-selection, without a drag, selects just that one.
            setAnchorSelection(original, {gesture.index});
        break;
    }
    refreshAnchorModeSwitcher();
    update();
}

bool ViewerEditor::anchorPress(QMouseEvent* event) {
    if (anchorGesture_ && event->button() != Qt::LeftButton) {
        cancelAnchorGesture();
        event->accept();
        return true;
    }
    if (event->button() != Qt::LeftButton || (tool_ != Tool::Select && tool_ != Tool::Pen) ||
        creation_ || textEdit_)
        return false;
    const auto mapping = currentMapping();
    if (!mapping || !contentRect().contains(event->position()))
        return false;
    const auto target = editablePath();
    const auto start = [&](const AnchorGesture::Kind kind, const std::size_t index,
                           const path_editing::HandleSide side) {
        playback_->pause();
        anchorGesture_.emplace(AnchorGesture{
            kind,
            {*mapping, session_.snapshot().revision(), session_.currentTime(),
             session_.compositionId(), event->position(), event->position(), event->modifiers()},
            *target,
            target->path,
            anchorSelectionFor(*target),
            index,
            side,
            event->modifiers().testFlag(Qt::AltModifier),
            false,
            false,
            event->modifiers().testFlag(Qt::ShiftModifier)});
        setFocus(Qt::MouseFocusReason);
        update();
        event->accept();
        return true;
    };
    if (target) {
        const auto screen = target->toWorld * toolScreenTransform(*mapping);
        auto selection = anchorSelectionFor(*target);
        if (const auto hit = hitAnchor(*target, screen, event->position())) {
            if (hit->part == 0 && tool_ == Tool::Pen) {
                // The Pen on an open shape path's end continues drawing from it; on any other
                // anchor it removes the anchor.
                if (beginPenContinuation(*target, hit->index, *mapping, event->position())) {
                    event->accept();
                    return true;
                }
                auto next = target->path;
                if (path_editing::deleteAnchors(
                        next, {hit->index},
                        path_editing::minimumAnchors(target->owner == EditablePath::Owner::Mask)) &&
                    commitPathEdit(*target, std::move(next))) {
                    selectedAnchors_.clear();
                    selectTool(Tool::Select);
                }
                event->accept();
                return true;
            }
            if (hit->part == 0 && event->modifiers().testFlag(Qt::ShiftModifier)) {
                if (!selection.erase(hit->index))
                    selection.insert(hit->index);
                setAnchorSelection(*target, std::move(selection));
                event->accept();
                return true;
            }
            if (!selection.contains(hit->index))
                setAnchorSelection(*target, {hit->index});
            if (hit->part == 0)
                return start(event->modifiers().testFlag(Qt::AltModifier)
                                 ? AnchorGesture::Kind::AltPoint
                                 : AnchorGesture::Kind::Move,
                             hit->index, path_editing::HandleSide::Out);
            return start(AnchorGesture::Kind::Handle, hit->index,
                         hit->part == 1 ? path_editing::HandleSide::In
                                        : path_editing::HandleSide::Out);
        }
        if (tool_ == Tool::Pen) {
            // The Pen on a segment adds an anchor there without changing the shape.
            const auto toScreen = [&screen](const document::Vec2d point) {
                return toolValue(screen.map(toolPoint(point)));
            };
            const auto hit = path_editing::nearestSegmentPoint(target->path, toScreen,
                                                               toolValue(event->position()));
            if (hit && hit->distance <= kit::px(kit::Size::GizmoHandle)) {
                auto next = target->path;
                if (const auto added = path_editing::insertAnchor(next, *hit);
                    added && commitPathEdit(*target, std::move(next))) {
                    selectTool(Tool::Select);
                    setAnchorSelection(*target, {*added});
                }
                event->accept();
                return true;
            }
            return false;
        }
    }
    if (tool_ != Tool::Select)
        return false;
    if (maskOutlinePress(event, *mapping))
        return true;
    if (target && hitAt(*mapping, event->position()).region == ViewerHitRegion::Empty)
        return start(AnchorGesture::Kind::Marquee, 0, path_editing::HandleSide::Out);
    return false;
}

bool ViewerEditor::anchorMove(QMouseEvent* event) {
    if (!anchorGesture_)
        return false;
    auto& gesture = *anchorGesture_;
    const auto mapping = currentMapping();
    if (!mapping || *mapping != gesture.gesture.mapping ||
        session_.snapshot().revision() != gesture.gesture.revision ||
        session_.currentTime() != gesture.gesture.time) {
        cancelAnchorGesture();
        return true;
    }
    gesture.gesture.pointer = event->position();
    if (!gesture.moved && QLineF(gesture.gesture.origin, event->position()).length() <
                              QApplication::startDragDistance()) {
        event->accept();
        return true;
    }
    gesture.moved = true;
    if (gesture.kind == AnchorGesture::Kind::Marquee) {
        update();
        event->accept();
        return true;
    }
    const auto inverse = gesture.target.toWorld.inverted();
    const auto here = inverse.map(toolPoint(mapping->toComposition(event->position())));
    const auto origin = inverse.map(toolPoint(mapping->toComposition(gesture.gesture.origin)));
    auto next = gesture.base;
    switch (gesture.kind) {
    case AnchorGesture::Kind::Move:
        path_editing::moveAnchors(next, gesture.anchors, toolValue(here - origin));
        break;
    case AnchorGesture::Kind::Handle: {
        const auto& anchor = gesture.base.anchors[gesture.index];
        const auto& handle =
            gesture.side == path_editing::HandleSide::In ? anchor.inHandle : anchor.outHandle;
        if (!handle)
            break;
        path_editing::dragHandle(next, gesture.index, gesture.side,
                                 toolValue(toolPoint(*handle) + (here - origin)),
                                 gesture.free || event->modifiers().testFlag(Qt::AltModifier));
        break;
    }
    case AnchorGesture::Kind::AltPoint:
        path_editing::pullHandles(next, gesture.index, toolValue(here));
        break;
    case AnchorGesture::Kind::Marquee:
        break;
    }
    gesture.target.path = next;
    previewPathEdit(gesture, next);
    update();
    event->accept();
    return true;
}

bool ViewerEditor::anchorRelease(QMouseEvent* event) {
    if (!anchorGesture_ || event->button() != Qt::LeftButton)
        return false;
    (void)anchorMove(event);
    commitAnchorGesture();
    event->accept();
    return true;
}

bool ViewerEditor::setAnchorMode(const document::PathAnchorMode mode) {
    const auto target = editablePath();
    if (!target || anchorGesture_)
        return false;
    const auto anchors = anchorSelectionFor(*target);
    if (anchors.empty())
        return false;
    auto next = target->path;
    path_editing::setMode(next, anchors, mode);
    // A re-labelled anchor whose geometry is already right still records its new mode.
    const bool changed = commitPathEdit(*target, std::move(next));
    refreshAnchorModeSwitcher();
    return changed;
}

bool ViewerEditor::deleteSelectedAnchors() {
    const auto target = editablePath();
    if (!target || anchorGesture_)
        return false;
    auto next = target->path;
    if (!path_editing::deleteAnchors(
            next, anchorSelectionFor(*target),
            path_editing::minimumAnchors(target->owner == EditablePath::Owner::Mask)) ||
        !commitPathEdit(*target, std::move(next)))
        return false;
    selectedAnchors_.clear();
    refreshAnchorModeSwitcher();
    return true;
}

bool ViewerEditor::setSelectedPathClosed(const bool closed) {
    const auto target = editablePath();
    if (!target || anchorGesture_ || target->owner != EditablePath::Owner::Shape)
        return false;
    auto next = target->path;
    return path_editing::setClosed(next, closed) && commitPathEdit(*target, std::move(next));
}

bool ViewerEditor::anchorKey(QKeyEvent* event) {
    if (event->key() != Qt::Key_Delete && event->key() != Qt::Key_Backspace)
        return false;
    const auto target = editablePath();
    if (!target || anchorSelectionFor(*target).empty())
        return false;
    // Delete edits the path, never the layer, even when the minimum refuses it.
    (void)deleteSelectedAnchors();
    update();
    event->accept();
    return true;
}

std::unique_ptr<QMenu> ViewerEditor::anchorContextMenu() {
    const auto target = editablePath();
    if (!target)
        return nullptr;
    const auto anchors = anchorSelectionFor(*target);
    if (anchors.empty())
        return nullptr;
    std::unique_ptr<QMenu> menu(kit::makeMenu(this));
    menu->setObjectName(QStringLiteral("viewerAnchorMenu"));
    const auto common = path_editing::commonMode(target->path, anchors);
    for (const auto mode : kAnchorModes) {
        auto* action = menu->addAction(anchorModeText(mode));
        action->setObjectName(QStringLiteral("viewerAnchorMode") + anchorModeText(mode));
        action->setCheckable(true);
        action->setChecked(common == mode);
        connect(action, &QAction::triggered, this, [this, mode] { (void)setAnchorMode(mode); });
    }
    menu->addSeparator();
    auto* remove = menu->addAction(anchors.size() == 1 ? tr("Delete Point") : tr("Delete Points"));
    remove->setObjectName(QStringLiteral("viewerAnchorDelete"));
    connect(remove, &QAction::triggered, this, [this] { (void)deleteSelectedAnchors(); });
    if (target->owner == EditablePath::Owner::Shape) {
        const bool closed = target->path.closed;
        auto* toggle = menu->addAction(closed ? tr("Open Path") : tr("Close Path"));
        toggle->setObjectName(QStringLiteral("viewerPathClosedToggle"));
        toggle->setEnabled(closed || target->path.anchors.size() >= 3);
        connect(toggle, &QAction::triggered, this,
                [this, closed] { (void)setSelectedPathClosed(!closed); });
    }
    return menu;
}

bool ViewerEditor::anchorContextMenuEvent(QContextMenuEvent* event) {
    const auto target = editablePath();
    const auto mapping = currentMapping();
    if (!target || !mapping || anchorGesture_)
        return false;
    const auto screen = target->toWorld * toolScreenTransform(*mapping);
    if (const auto hit = hitAnchor(*target, screen, event->pos());
        hit && !anchorSelectionFor(*target).contains(hit->index))
        setAnchorSelection(*target, {hit->index});
    auto menu = anchorContextMenu();
    if (!menu)
        return false;
    menu->exec(event->globalPos());
    event->accept();
    return true;
}

void ViewerEditor::buildAnchorModeSwitcher() {
    anchorModeSwitcher_ = new kit::KRadioGroup(this);
    anchorModeSwitcher_->setObjectName(QStringLiteral("viewerAnchorModeSwitcher"));
    anchorModeSwitcher_->setAccessibleName(tr("Anchor handle mode"));
    anchorModeSwitcher_->setToolTip(
        tr("Handle mode of the selected anchors: Corner, Mirrored, Aligned, Free or Auto"));
    for (const auto mode : kAnchorModes)
        anchorModeSwitcher_->addOption(anchorModeText(mode));
    anchorModeSwitcher_->setFixedSize(anchorModeSwitcher_->sizeHint());
    anchorModeSwitcher_->hide();
    connect(anchorModeSwitcher_, &kit::KRadioGroup::currentIndexChanged, this,
            [this](const int index) {
                if (index >= 0 && index < static_cast<int>(kAnchorModes.size()))
                    (void)setAnchorMode(kAnchorModes[static_cast<std::size_t>(index)]);
            });
}

void ViewerEditor::refreshAnchorModeSwitcher() {
    if (anchorModeSwitcher_ == nullptr)
        return;
    const auto target = editablePath();
    const auto anchors = target ? anchorSelectionFor(*target) : path_editing::AnchorSet{};
    if (anchors.empty()) {
        anchorModeSwitcher_->hide();
        return;
    }
    const QSignalBlocker blocker(anchorModeSwitcher_);
    if (const auto common = path_editing::commonMode(target->path, anchors))
        anchorModeSwitcher_->setCurrentIndex(static_cast<int>(*common));
    else
        anchorModeSwitcher_->setMixed();
    // The Viewer's tool-options position: the top of the canvas, beside the tool column.
    const int gap = kit::px(kit::Spacing::S);
    anchorModeSwitcher_->move(toolColumn_->geometry().right() + gap,
                              static_cast<int>(contentRect().top()) + gap);
    anchorModeSwitcher_->show();
    anchorModeSwitcher_->raise();
}

void ViewerEditor::paintAnchorEditing(QPainter& painter) const {
    const auto mapping = currentMapping();
    const auto target = editablePath();
    if (!mapping || !target || (creation_ && tool_ == Tool::Pen && penContinuation_))
        return;
    const auto transform = target->toWorld * toolScreenTransform(*mapping);
    const auto selection = anchorSelectionFor(*target);
    painter.save();
    painter.setClipRect(contentRect());
    painter.setRenderHint(QPainter::Antialiasing);
    QPen pen(kit::color(kit::Color::Accent), kit::px(kit::Size::Hairline));
    pen.setCosmetic(true);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(transform.map(masks::outline(target->path)));
    if (target->owner == EditablePath::Owner::Mask &&
        (target->feather > 0.0 || target->expansion != 0.0)) {
        // Feather bounds at expansion +/- feather / 2, in content pixels, mapped with the outline.
        QPen dashed = pen;
        dashed.setStyle(Qt::DashLine);
        painter.setPen(dashed);
        if (target->feather > 0.0) {
            for (const double sign : {1.0, -1.0})
                painter.drawPath(transform.map(masks::offsetOutline(
                    target->path, target->expansion + sign * target->feather / 2.0)));
        } else {
            painter.drawPath(transform.map(masks::offsetOutline(target->path, target->expansion)));
        }
        painter.setPen(pen);
    }
    const auto half = kit::px(kit::Size::GizmoHandle) / 2.0;
    for (std::size_t index = 0; index < target->path.anchors.size(); ++index) {
        const auto& anchor = target->path.anchors[index];
        const auto point = transform.map(toolPoint(anchor.point));
        for (const auto& handle : {anchor.inHandle, anchor.outHandle}) {
            if (!handle)
                continue;
            const auto end = transform.map(toolPoint(*handle));
            painter.drawLine(point, end);
            painter.setBrush(kit::color(kit::Color::Surface));
            painter.drawEllipse(end, half, half);
        }
        painter.setBrush(
            kit::color(selection.contains(index) ? kit::Color::Accent : kit::Color::Surface));
        painter.drawRect(QRectF(point.x() - half, point.y() - half, 2 * half, 2 * half));
    }
    if (anchorGesture_ && anchorGesture_->kind == AnchorGesture::Kind::Marquee &&
        anchorGesture_->moved) {
        QPen dashed = pen;
        dashed.setStyle(Qt::DashLine);
        painter.setPen(dashed);
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(
            QRectF(anchorGesture_->gesture.origin, anchorGesture_->gesture.pointer).normalized());
    }
    painter.restore();
}
