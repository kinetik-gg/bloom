// Included inside bloom::ui by composition_session.cpp, like the transform interaction seam.
namespace {

double* editedComponent(document::ParameterValue& value,
                        const document::AnimationComponent component) {
    return std::visit(
        [component](auto& held) -> double* {
            using Value = std::decay_t<decltype(held)>;
            if constexpr (std::is_same_v<Value, document::Vec2d> ||
                          std::is_same_v<Value, document::Vec3d>) {
                if (component == document::AnimationComponent::X)
                    return &held.x;
                if (component == document::AnimationComponent::Y)
                    return &held.y;
                if constexpr (std::is_same_v<Value, document::Vec3d>)
                    if (component == document::AnimationComponent::Z)
                        return &held.z;
            } else if constexpr (std::is_same_v<Value, core::Color4d>) {
                switch (component) {
                case document::AnimationComponent::Red:
                    return &held.red;
                case document::AnimationComponent::Green:
                    return &held.green;
                case document::AnimationComponent::Blue:
                    return &held.blue;
                case document::AnimationComponent::Alpha:
                    return &held.alpha;
                default:
                    break;
                }
            }
            return nullptr;
        },
        value);
}

} // namespace

bool CompositionSession::valueEditActive() const noexcept { return valueEdit_.has_value(); }
bool CompositionSession::isValueEditing(const document::ParameterId parameter) const noexcept {
    return valueEdit_ && valueEdit_->parameter == parameter;
}

std::optional<document::ParameterValue>
CompositionSession::liveValue(const document::ParameterId parameterId) const {
    const auto* current = composition();
    const auto* parameter = current ? current->parameters().find(parameterId) : nullptr;
    if (valueEdit_ && valueEdit_->anchor && valueEdit_->anchor->parameter == parameterId)
        return valueEdit_->anchor->value;
    if (valueEdit_ && isValueEditing(parameterId))
        return valueEdit_->value;
    if (parameter) {
        const auto* constant = std::get_if<document::ConstantValueSource>(&parameter->source);
        if (constant && std::holds_alternative<document::PathValue>(constant->value))
            return constant->value;
    }
    if (const auto sample = effectiveParameterValue(parameter))
        return std::visit([](const auto& value) -> document::ParameterValue { return value; },
                          *sample);
    return std::nullopt;
}

bool CompositionSession::beginValueEdit(
    const document::ParameterId parameterId,
    const std::optional<document::AnimationComponent> component) {
    Q_ASSERT(QThread::currentThread() == thread());
    cancelValueEdit();
    cancelTransformInteraction();
    const auto* current = composition();
    const auto* parameter = current ? current->parameters().find(parameterId) : nullptr;
    auto base = liveValue(parameterId);
    if (!parameter || !base || current->parameterLocked(parameterId) ||
        std::holds_alternative<document::DriverBindingSource>(parameter->source) ||
        (component && !editedComponent(*base, *component))) {
        reportUnavailable(tr("This parameter is not available for editing"));
        return false;
    }
    valueEdit_.emplace(
        ValueEdit{snapshot_.revision(), parameterId, currentTime_, component, *base, *base});
    return true;
}

bool CompositionSession::updateValueEdit(document::ParameterValue value) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!valueEdit_)
        return false;
    auto& edit = *valueEdit_;
    if (edit.revision != snapshot_.revision() || edit.time != currentTime_) {
        cancelValueEdit();
        return false;
    }
    if (edit.component) {
        auto next = edit.base;
        auto* target = editedComponent(next, *edit.component);
        const auto* supplied = std::get_if<double>(&value);
        if (!supplied)
            supplied = editedComponent(value, *edit.component);
        if (!target || !supplied)
            return false;
        *target = *supplied;
        value = std::move(next);
    }
    const auto* parameter = composition()->parameters().find(edit.parameter);
    document::ParameterStore validation;
    if (value.index() != edit.base.index() || !parameter ||
        !validation.insert(
            {edit.parameter, parameter->schemaKey, document::ConstantValueSource{value}}) ||
        !validation.validate().ok()) {
        reportUnavailable(tr("The value is outside this parameter's domain"));
        return false;
    }
    if (edit.value != value) {
        edit.value = std::move(value);
        emit liveValueChanged();
    }
    return true;
}

bool CompositionSession::beginPathEdit(const document::ParameterId parameter) {
    if (!beginValueEdit(parameter) || !valueEdit_)
        return false;
    const auto* anchor = parameterForSelection(document::kAnchorParameterRole);
    const auto base = anchor ? liveValue(anchor->id) : std::nullopt;
    if (!anchor || !base || !std::holds_alternative<document::Vec2d>(*base) ||
        composition()->parameterLocked(anchor->id) ||
        std::holds_alternative<document::DriverBindingSource>(anchor->source) ||
        !std::holds_alternative<document::PathValue>(valueEdit_->base)) {
        cancelValueEdit();
        return false;
    }
    const auto point = std::get<document::Vec2d>(*base);
    valueEdit_->anchor = ValueEdit::PathAnchorEdit{anchor->id, point, point};
    return true;
}

bool CompositionSession::updatePathEdit(document::PathValue path,
                                        const document::Vec2d centreDelta) {
    if (!valueEdit_ || !valueEdit_->anchor || !std::isfinite(centreDelta.x) ||
        !std::isfinite(centreDelta.y))
        return false;
    auto& anchor = *valueEdit_->anchor;
    const document::Vec2d value{anchor.base.x - centreDelta.x, anchor.base.y - centreDelta.y};
    if (!std::isfinite(value.x) || !std::isfinite(value.y) || !path.isValid())
        return false;
    anchor.value = value;
    return updateValueEdit(std::move(path));
}

std::vector<runtime::SnapshotParameterOverride> CompositionSession::valueEditOverrides() const {
    if (!valueEdit_)
        return {};
    auto result = std::visit(
        [this](const auto& value) -> std::vector<runtime::SnapshotParameterOverride> {
            // Only the kinds the override variant carries can be previewed live.
            using Override = decltype(runtime::SnapshotParameterOverride::value);
            if constexpr (std::is_constructible_v<Override, const std::decay_t<decltype(value)>&>)
                return {{valueEdit_->revision, valueEdit_->parameter, value}};
            else
                return {};
        },
        valueEdit_->value);
    if (valueEdit_->anchor)
        result.push_back(
            {valueEdit_->revision, valueEdit_->anchor->parameter, valueEdit_->anchor->value});
    return result;
}

void CompositionSession::cancelValueEdit() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (valueEdit_) {
        valueEdit_.reset();
        emit liveValueChanged();
    }
}

bool CompositionSession::commitValueEdit() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!valueEdit_)
        return false;
    auto edit = std::move(*valueEdit_);
    valueEdit_.reset();
    bool result = edit.revision == snapshot_.revision() && edit.time == currentTime_;
    if (result && edit.value != edit.base) {
        commands::Transaction transaction("Set Parameter", edit.revision);
        const auto* parameter = composition()->parameters().find(edit.parameter);
        if (edit.component && parameter &&
            std::holds_alternative<document::AnimationCurveSource>(parameter->source)) {
            transaction.emplace<commands::SetKeyframeAtTimeForParameterComponent>(
                compositionId_, edit.parameter, *edit.component, edit.time,
                *editedComponent(edit.value, *edit.component));
        } else {
            result = appendParameterEdit(transaction, edit.parameter, edit.time, edit.value);
        }
        if (result && edit.anchor && edit.anchor->base != edit.anchor->value)
            result = appendParameterEdit(transaction, edit.anchor->parameter, edit.time,
                                         edit.anchor->value);
        if (result)
            result = executeTransaction(std::move(transaction)).succeeded();
    }
    emit liveValueChanged();
    return result;
}
