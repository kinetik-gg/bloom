// Included after the world/parent mapping and authored values have been frozen.
using Native = TransformInteraction::NativeKind;
auto& state = *transformInteraction_;
std::vector<std::string_view> nativeRoles;
if (parameterForSelection("kind") && parameterForSelection("path")) {
    const auto* kind = parameterForSelection("kind");
    const auto value = liveValue(kind->id);
    if (!value || !std::holds_alternative<std::int64_t>(*value)) {
        cancelTransformInteraction();
        return TransformInteractionRejection::NoResolvableTransform;
    }
    const auto shape = static_cast<document::ShapeKind>(std::get<std::int64_t>(*value));
    if (shape == document::ShapeKind::Path) {
        state.nativeKind = Native::Path;
        nativeRoles = {"path"};
    } else if (shape == document::ShapeKind::Line) {
        state.nativeKind = Native::Line;
        nativeRoles = {"lineStart", "lineEnd"};
    } else {
        state.nativeKind = Native::Size;
        nativeRoles = {"size"};
    }
} else if (parameterForSelection(document::kTextParameterRole)) {
    const auto* box = parameterForSelection(document::kTextBoxParameterRole);
    const auto value = box ? liveValue(box->id) : std::nullopt;
    const auto* extent = value ? std::get_if<document::Vec2d>(&*value) : nullptr;
    const bool boxed = extent && extent->x > 0 && extent->y > 0;
    state.nativeKind = boxed ? Native::BoxText : Native::PointText;
    nativeRoles = {boxed ? document::kTextBoxParameterRole : document::kTextSizeParameterRole};
    if (!boxed && gesture.handle % 2 != 0) {
        cancelTransformInteraction();
        return TransformInteractionRejection::NoResolvableTransform;
    }
} else if (parameterForSelection(document::kSolidWidthParameterRole) &&
           parameterForSelection(document::kSolidHeightParameterRole)) {
    state.nativeKind = Native::Solid;
    nativeRoles = {document::kSolidWidthParameterRole, document::kSolidHeightParameterRole};
}
for (const auto role : nativeRoles) {
    const auto* parameter = parameterForSelection(role);
    const auto value = parameter ? liveValue(parameter->id) : std::nullopt;
    if (!parameter || !value || composition()->parameterLocked(parameter->id) ||
        std::holds_alternative<document::DriverBindingSource>(parameter->source)) {
        cancelTransformInteraction();
        return TransformInteractionRejection::NoResolvableTransform;
    }
    state.native.emplace_back(parameter->id, *value);
}
