// Included inside bloom::ui. Gestures only read bounded evaluated geometry and sampled values;
// evaluation, source I/O and rasterization remain on the preview worker.
namespace {
QPointF transformPoint(const document::Vec2d point) { return {point.x, point.y}; }
document::Vec2d transformValue(const QPointF point) { return {point.x(), point.y()}; }
// SAVEFIX-1. Every degeneracy test in this file used to be a test against exactly zero -- an empty
// bounds rectangle, a singular transform, a zero-length scale arm. NaN passes all of them
// (NaN <= NaN, NaN != 0 and Qt's fuzzy determinant test are all false/true in the wrong
// direction), so one non-finite number in the frozen evaluated bounds propagated straight through
// the gesture arithmetic into the value the gesture offers the document. These predicates make
// finiteness the precondition it always had to be.
[[nodiscard]] bool finite(const double value) noexcept { return std::isfinite(value); }
[[nodiscard]] bool finite(const QPointF point) noexcept {
    return finite(point.x()) && finite(point.y());
}
[[nodiscard]] bool finite(const document::Vec2d value) noexcept {
    return finite(value.x) && finite(value.y);
}
[[nodiscard]] bool finite(const QTransform& transform) noexcept {
    return finite(transform.m11()) && finite(transform.m12()) && finite(transform.m21()) &&
           finite(transform.m22()) && finite(transform.dx()) && finite(transform.dy());
}
[[nodiscard]] bool finite(const runtime::ContentBounds& bounds) noexcept {
    return finite(bounds.left) && finite(bounds.top) && finite(bounds.right) &&
           finite(bounds.bottom);
}
[[nodiscard]] bool finite(const runtime::EvaluatedOperationBounds& bounds) noexcept {
    return finite(bounds.local) && finite(bounds.output) && finite(bounds.anchor) &&
           std::ranges::all_of(bounds.polygon,
                               [](const document::Vec2d point) { return finite(point); });
}
// A divisor is usable only when it is finite and not zero, and only when the quotient it produces
// is itself representable -- a denormal arm overflows an ordinary delta to an infinity that every
// `!= 0` guard in this file would have accepted.
[[nodiscard]] bool dividesFinitely(const double numerator, const double divisor) noexcept {
    return finite(numerator) && finite(divisor) && divisor != 0.0 && finite(numerator / divisor);
}
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
    cancelValueEdit();
    cancelTransformInteraction();
    const auto* layerId = std::get_if<document::LayerId>(&selection_.primary);
    if (!layerId)
        return TransformInteractionRejection::NoLayerSelected;
    if (mapping.displayRect.isEmpty())
        return TransformInteractionRejection::EmptyMapping;
    if (gesture.handle < 0 || gesture.handle >= 8)
        return TransformInteractionRejection::NoResolvableTransform;
    const bool nativeResize = gesture.kind == TransformGesture::Kind::Scale &&
                              ((parameterForSelection("kind") && parameterForSelection("path")) ||
                               parameterForSelection(document::kTextParameterRole) ||
                               parameterForSelection(document::kSolidWidthParameterRole));
    constexpr std::array roles{document::kPositionParameterRole, document::kAnchorParameterRole,
                               document::kScaleParameterRole, document::kRotationParameterRole};
    std::array<document::ParameterId, 4> ids;
    std::array<ParameterSample, 4> values;
    for (std::size_t i = 0; i < roles.size(); ++i) {
        const auto* parameter = parameterForSelection(roles[i]);
        if (!parameter)
            return TransformInteractionRejection::NoResolvableTransform;
        if ((!nativeResize || i == 0) && composition()->parameterLocked(parameter->id))
            return TransformInteractionRejection::LockedLayer;
        if ((!nativeResize || i == 0) &&
            std::holds_alternative<document::DriverBindingSource>(parameter->source))
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
    if (!finite(position) || !finite(anchor) || !finite(scale) || !finite(rotation))
        return TransformInteractionRejection::SingularTransform;
    QTransform inverseParent;
    if (gesture.bounds) {
        const auto& bounds = *gesture.bounds;
        if (bounds.layerId != *layerId || !finite(bounds) || bounds.local.empty() ||
            bounds.output.empty())
            return TransformInteractionRejection::NoResolvableTransform;
        const auto x = (transformPoint(bounds.polygon[1]) - transformPoint(bounds.polygon[0])) /
                       (bounds.local.right - bounds.local.left);
        const auto y = (transformPoint(bounds.polygon[3]) - transformPoint(bounds.polygon[0])) /
                       (bounds.local.bottom - bounds.local.top);
        const QTransform world(x.x(), x.y(), y.x(), y.y(), 0, 0);
        bool invertible = false;
        const auto inverseWorld = world.inverted(&invertible);
        // Qt reports invertibility from a fuzzy determinant test, which a non-finite basis passes:
        // the inverse is checked for finiteness rather than trusted.
        if (!invertible || !finite(inverseWorld) || !finite(scale) || scale.x == 0 || scale.y == 0)
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
                                                 {},
                                                 TransformInteraction::NativeKind::Raster,
                                                 {}};
    if (gesture.kind == TransformGesture::Kind::Scale) {
#include "composition_session_native_begin.ipp"
    }
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
    if (!finite(delta) || !finite(point) || !finite(origin)) {
        cancelTransformInteraction();
        return;
    }
    double previousRotation = state.rotation;
    if (state.gesture.kind == TransformGesture::Kind::Rotate && !state.overrides.empty())
        if (const auto* value = std::get_if<double>(&state.overrides.front().value))
            previousRotation = *value;
    // Staged, not written straight into the interaction: an update that would offer an
    // unrepresentable value keeps the last good preview instead (see the check after the switch).
    std::vector<runtime::SnapshotParameterOverride> staged;
    const auto put = [&](const std::size_t index, auto value) {
        staged.push_back({state.baseRevision, state.parameters[index], value});
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
        if (!invertible || !finite(inverse)) {
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
        if (!finite(from) || !finite(to) || std::hypot(from.x(), from.y()) == 0 ||
            std::hypot(to.x(), to.y()) == 0) {
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
        if (state.nativeKind != TransformInteraction::NativeKind::Raster) {
#include "composition_session_native_resize.ipp"
            break;
        }
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
        if (xActive && dividesFinitely(localDelta.x(), arm.x()))
            next.x += localDelta.x() / arm.x();
        if (yActive && dividesFinitely(localDelta.y(), arm.y()))
            next.y += localDelta.y() / arm.y();
        if (modifiers.shift) {
            const QPointF baseArm(xActive ? arm.x() * state.scale.x : 0,
                                  yActive ? arm.y() * state.scale.y : 0);
            const double lengthSquared = QPointF::dotProduct(baseArm, baseArm);
            const double projection = QPointF::dotProduct(localDelta, baseArm);
            const double factor =
                dividesFinitely(projection, lengthSquared) ? 1 + projection / lengthSquared : 1;
            next = {state.scale.x * factor, state.scale.y * factor};
        }
        put(0, transformValue(position + linear.map(fixed - anchor) -
                              authoredLinear(next, state.rotation).map(fixed - anchor)));
        put(2, next);
        break;
    }
    }
    // The one place a gesture's arithmetic reaches the document. A degenerate frame -- an
    // overflowed bounds, a denormal scale arm, a rotation about a zero-length ray -- can still
    // produce a value the document must refuse; offering it would lose the whole gesture at
    // commit. The update is dropped instead and the last representable preview stands.
    const auto representable = [](const runtime::SnapshotParameterOverride& override) {
        return std::visit(
            [](const auto& held) {
                using Held = std::decay_t<decltype(held)>;
                if constexpr (std::is_same_v<Held, double> ||
                              std::is_same_v<Held, document::Vec2d>) {
                    return finite(held);
                } else if constexpr (std::is_same_v<Held, document::Vec3d>) {
                    return finite(held.x) && finite(held.y) && finite(held.z);
                } else if constexpr (std::is_same_v<Held, core::Color4d> ||
                                     std::is_same_v<Held, document::PathValue>) {
                    return held.isValid();
                } else {
                    return true;
                }
            },
            override.value);
    };
    if (!std::ranges::all_of(staged, representable)) {
        return;
    }
    state.overrides = std::move(staged);
    emit transformInteractionChanged();
    emit liveValueChanged();
}

void CompositionSession::cancelTransformInteraction() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (transformInteraction_) {
        transformInteraction_.reset();
        emit transformInteractionChanged();
        emit liveValueChanged();
    }
}

void CompositionSession::invalidateTransformInteraction() { cancelTransformInteraction(); }

// The interaction stays ARMED for the whole of this call, and that ordering is the contract, not
// an accident. Clearing it first emitted transformInteractionChanged() while snapshot_ was still
// the PRE-EDIT revision, so CompositionPreviewController built its next request on that revision
// with no override on it -- and the RAM preview cache, which holds exactly that frame, answered it
// instantly. The artist saw the layer snap back to where the gesture started for one frame before
// the committed frame arrived. Held armed, every request built from here on names the committed
// revision, so the last override frame stays on screen until a frame for that revision replaces
// it. handleResult() -> invalidateTransformInteractionOnStaleRevision() drops the interaction the
// moment the new revision is adopted; `finish` drops it on every path that never gets that far
// (a stale base, a refused edit, or a gesture that moved nothing), and is a no-op once it has.
bool CompositionSession::commitTransformInteraction() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!transformInteraction_)
        return false;
    // A copy, not a move: a moved-from interaction still reports has_value() while its overrides
    // have been emptied, which is precisely the armed-but-override-less state this call must not
    // put the preview controller in.
    const auto state = *transformInteraction_;
    const auto finish = [this](const bool committed) {
        cancelTransformInteraction();
        return committed;
    };
    if (state.baseRevision != snapshot_.revision() || state.time != currentTime_)
        return finish(false);
    constexpr std::array labels{"Move Layer", "Scale Layer", "Rotate Layer", "Move Anchor"};
    commands::Transaction transaction(labels[static_cast<std::size_t>(state.gesture.kind)],
                                      state.baseRevision);
    bool changed = false;
    for (const auto& override : state.overrides) {
        const auto* parameter = composition()->parameters().find(override.parameterId);
        if (!parameter)
            return finish(false);
        std::optional<document::ParameterValue> before;
        if (override.parameterId == state.parameters[0])
            before = state.position;
        else if (override.parameterId == state.parameters[1])
            before = state.anchor;
        else if (override.parameterId == state.parameters[2])
            before = state.scale;
        else if (override.parameterId == state.parameters[3])
            before = state.rotation;
        else {
            const auto native = std::ranges::find(state.native, override.parameterId,
                                                  [](const auto& item) { return item.first; });
            if (native != state.native.end())
                before = native->second;
        }
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
            return finish(false);
        changed = true;
    }
    return finish(!changed || executeTransaction(std::move(transaction)).changed());
}
