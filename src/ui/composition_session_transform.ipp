// Included inside bloom::ui. Gestures only read bounded evaluated geometry and sampled values;
// evaluation, source I/O and rasterization remain on the preview worker.
namespace {
QPointF transformPoint(const document::Vec2d point) { return {point.x, point.y}; }
document::Vec2d transformValue(const QPointF point) { return {point.x(), point.y()}; }
QTransform authoredLinear(const document::Vec2d scale, const double degrees) {
    QTransform result;
    result.rotate(degrees);
    result.scale(scale.x, scale.y);
    return result;
}
} // namespace

bool CompositionSession::transformInteractionActive() const noexcept {
    return transformInteraction_.has_value();
}

std::vector<runtime::SnapshotParameterOverride>
CompositionSession::transformInteractionOverrides() const {
    return transformInteraction_ ? transformInteraction_->overrides
                                 : std::vector<runtime::SnapshotParameterOverride>{};
}

std::optional<TransformInteractionRejection>
CompositionSession::beginTransformInteraction(TransformGesture gesture, ViewerMapping mapping,
                                              const TransformModifiers modifiers) {
    Q_ASSERT(QThread::currentThread() == thread());
    cancelTransformInteraction();
    const auto* layerId = std::get_if<document::LayerId>(&selection_.primary);
    if (!layerId)
        return TransformInteractionRejection::NoLayerSelected;
    if (mapping.displayRect.isEmpty())
        return TransformInteractionRejection::EmptyMapping;
    if (gesture.handle < 0 || gesture.handle >= 8)
        return TransformInteractionRejection::NoResolvableTransform;
    constexpr std::array roles{document::kPositionParameterRole, document::kAnchorParameterRole,
                               document::kScaleParameterRole, document::kRotationParameterRole};
    std::array<document::ParameterId, 4> ids;
    std::array<ParameterSample, 4> values;
    for (std::size_t i = 0; i < roles.size(); ++i) {
        const auto* parameter = parameterForSelection(roles[i]);
        if (!parameter)
            return TransformInteractionRejection::NoResolvableTransform;
        if (composition()->parameterLocked(parameter->id))
            return TransformInteractionRejection::LockedLayer;
        if (std::holds_alternative<document::DriverBindingSource>(parameter->source))
            return TransformInteractionRejection::DrivenParameter;
        const auto value = effectiveParameterValue(parameter);
        if (!value || (i < 3 ? !std::holds_alternative<document::Vec2d>(*value)
                             : !std::holds_alternative<double>(*value)))
            return TransformInteractionRejection::NoResolvableTransform;
        ids[i] = parameter->id;
        values[i] = *value;
    }
    const auto position = std::get<document::Vec2d>(values[0]);
    const auto anchor = std::get<document::Vec2d>(values[1]);
    const auto scale = std::get<document::Vec2d>(values[2]);
    const auto rotation = std::get<double>(values[3]);
    QTransform inverseParent;
    if (gesture.bounds) {
        const auto& bounds = *gesture.bounds;
        if (bounds.layerId != *layerId || bounds.local.empty() || bounds.output.empty())
            return TransformInteractionRejection::NoResolvableTransform;
        const auto x = (transformPoint(bounds.polygon[1]) - transformPoint(bounds.polygon[0])) /
                       (bounds.local.right - bounds.local.left);
        const auto y = (transformPoint(bounds.polygon[3]) - transformPoint(bounds.polygon[0])) /
                       (bounds.local.bottom - bounds.local.top);
        const QTransform world(x.x(), x.y(), y.x(), y.y(), 0, 0);
        bool invertible = false;
        const auto inverseWorld = world.inverted(&invertible);
        if (!invertible || scale.x == 0 || scale.y == 0)
            return TransformInteractionRejection::SingularTransform;
        // Qt composes row vectors left to right: world^-1 followed by childLocal.
        // With world = parent * child in column notation this is exactly parent^-1.
        if (parentOf(*layerId))
            inverseParent = inverseWorld * authoredLinear(scale, rotation);
    } else if (parentOf(*layerId) || gesture.kind != TransformGesture::Kind::Move) {
        return TransformInteractionRejection::NoResolvableTransform;
    }
    transformInteraction_ = TransformInteraction{snapshot_.revision(),
                                                 *layerId,
                                                 currentTime_,
                                                 mapping,
                                                 gesture,
                                                 ids,
                                                 position,
                                                 anchor,
                                                 scale,
                                                 rotation,
                                                 inverseParent,
                                                 {}};
    updateTransformInteraction(transformInteraction_->gesture.origin, modifiers);
    return std::nullopt;
}

void CompositionSession::updateTransformInteraction(const QPointF screenPoint,
                                                    const TransformModifiers modifiers) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!transformInteraction_)
        return;
    auto& state = *transformInteraction_;
    if (state.baseRevision != snapshot_.revision() || state.time != currentTime_) {
        cancelTransformInteraction();
        return;
    }
    const auto point = transformPoint(state.mapping.toComposition(screenPoint));
    const auto origin = transformPoint(state.mapping.toComposition(state.gesture.origin));
    const auto screenDelta = screenPoint - state.gesture.origin;
    const auto compositionDelta =
        state.mapping.toComposition(state.mapping.displayRect.topLeft() + screenDelta);
    const auto delta = state.inverseParent.map(transformPoint(compositionDelta));
    if (!std::isfinite(delta.x()) || !std::isfinite(delta.y())) {
        cancelTransformInteraction();
        return;
    }
    double previousRotation = state.rotation;
    if (state.gesture.kind == TransformGesture::Kind::Rotate && !state.overrides.empty())
        if (const auto* value = std::get_if<double>(&state.overrides.front().value))
            previousRotation = *value;
    state.overrides.clear();
    const auto put = [&](const std::size_t index, auto value) {
        state.overrides.push_back({state.baseRevision, state.parameters[index], value});
    };
    const auto position = transformPoint(state.position);
    const auto linear = authoredLinear(state.scale, state.rotation);
    switch (state.gesture.kind) {
    case TransformGesture::Kind::Move:
        put(0, transformValue(position + delta));
        break;
    case TransformGesture::Kind::Anchor: {
        bool invertible = false;
        const auto inverse = linear.inverted(&invertible);
        if (!invertible) {
            cancelTransformInteraction();
            return;
        }
        put(0, transformValue(position + delta));
        put(1, transformValue(transformPoint(state.anchor) + inverse.map(delta)));
        break;
    }
    case TransformGesture::Kind::Rotate: {
        if (delta.isNull() && previousRotation == state.rotation) {
            put(3, state.rotation);
            break;
        }
        if (!state.gesture.bounds) {
            cancelTransformInteraction();
            return;
        }
        const auto pivot = transformPoint(state.gesture.bounds->anchor);
        const auto from = state.inverseParent.map(origin - pivot);
        const auto to = state.inverseParent.map(point - pivot);
        if (std::hypot(from.x(), from.y()) == 0 || std::hypot(to.x(), to.y()) == 0) {
            put(3, state.rotation);
            break;
        }
        constexpr double degrees = 180.0 / 3.14159265358979323846;
        const double angle = std::atan2(from.x() * to.y() - from.y() * to.x(),
                                        from.x() * to.x() + from.y() * to.y()) *
                             degrees;
        double rotation = state.rotation + angle;
        // Select the continuous turn nearest the previous preview; the frozen base still owns
        // the angle calculation, while crossing atan2's seam never jumps by a full revolution.
        rotation += 360.0 * std::round((previousRotation - rotation) / 360.0);
        if (modifiers.shift)
            rotation = std::round(rotation / 15.0) * 15.0;
        put(3, rotation);
        break;
    }
    case TransformGesture::Kind::Scale: {
        if (delta.isNull()) {
            put(0, state.position);
            put(2, state.scale);
            break;
        }
        if (!state.gesture.bounds) {
            cancelTransformInteraction();
            return;
        }
        const auto& bounds = state.gesture.bounds->local;
        const auto centre = transformPoint(bounds.centre());
        const auto anchor = centre + transformPoint(state.anchor);
        const std::array<QPointF, 8> handles{{{bounds.left, bounds.top},
                                              {centre.x(), bounds.top},
                                              {bounds.right, bounds.top},
                                              {bounds.right, centre.y()},
                                              {bounds.right, bounds.bottom},
                                              {centre.x(), bounds.bottom},
                                              {bounds.left, bounds.bottom},
                                              {bounds.left, centre.y()}}};
        const auto handle = static_cast<std::size_t>(state.gesture.handle);
        const auto fixed = modifiers.alt ? anchor : handles[(handle + 4) % handles.size()];
        const auto arm = handles[handle] - fixed;
        const auto localDelta = authoredLinear({1, 1}, -state.rotation).map(delta);
        const bool xActive = handle != 1 && handle != 5;
        const bool yActive = handle != 3 && handle != 7;
        document::Vec2d next = state.scale;
        if (xActive && arm.x() != 0)
            next.x += localDelta.x() / arm.x();
        if (yActive && arm.y() != 0)
            next.y += localDelta.y() / arm.y();
        if (modifiers.shift) {
            const QPointF baseArm(xActive ? arm.x() * state.scale.x : 0,
                                  yActive ? arm.y() * state.scale.y : 0);
            const double lengthSquared = QPointF::dotProduct(baseArm, baseArm);
            const double factor =
                lengthSquared == 0 ? 1
                                   : 1 + QPointF::dotProduct(localDelta, baseArm) / lengthSquared;
            next = {state.scale.x * factor, state.scale.y * factor};
        }
        put(0, transformValue(position + linear.map(fixed - anchor) -
                              authoredLinear(next, state.rotation).map(fixed - anchor)));
        put(2, next);
        break;
    }
    }
    emit transformInteractionChanged();
}

void CompositionSession::cancelTransformInteraction() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (transformInteraction_) {
        transformInteraction_.reset();
        emit transformInteractionChanged();
    }
}

void CompositionSession::invalidateTransformInteraction() { cancelTransformInteraction(); }

bool CompositionSession::commitTransformInteraction() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!transformInteraction_)
        return false;
    auto state = std::move(*transformInteraction_);
    cancelTransformInteraction();
    if (state.baseRevision != snapshot_.revision() || state.time != currentTime_)
        return false;
    constexpr std::array labels{"Move Layer", "Scale Layer", "Rotate Layer", "Move Anchor"};
    commands::Transaction transaction(labels[static_cast<std::size_t>(state.gesture.kind)],
                                      state.baseRevision);
    bool changed = false;
    for (const auto& override : state.overrides) {
        const auto* parameter = composition()->parameters().find(override.parameterId);
        if (!parameter)
            return false;
        const auto before = effectiveParameterValue(parameter);
        const bool same =
            before && std::visit(
                          [&](const auto& value) {
                              const auto* base =
                                  std::get_if<std::decay_t<decltype(value)>>(&*before);
                              return base && *base == value;
                          },
                          override.value);
        if (same)
            continue;
        auto value = std::visit([](const auto& held) -> document::ParameterValue { return held; },
                                override.value);
        if (!appendParameterEdit(transaction, override.parameterId, state.time, std::move(value)))
            return false;
        changed = true;
    }
    return !changed || executeTransaction(std::move(transaction)).changed();
}
