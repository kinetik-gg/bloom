// Parameter projections shared by both hosts (moved from apps/bloom-mcp).
#include "core_internal.hpp"

#include <bloom/document/animation.hpp>
#include <bloom/document/layer_source.hpp>
#include <bloom/document/parameter_enums.hpp>
#include <bloom/document/project.hpp>

#include <array>
#include <cstddef>
#include <string_view>

#include <type_traits>

namespace bloom::mcp {
namespace {

yyjson_mut_val* vector(Json& out, const std::initializer_list<double> values) {
    auto* result = out.array();
    for (const auto value : values)
        out.append(result, out.real(value));
    return result;
}

yyjson_mut_val* parameterValue(Json& out, const document::ParameterValue& value) {
    return std::visit(
        [&out](const auto& item) -> yyjson_mut_val* {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, bool>) {
                return out.boolean(item);
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
                return out.signedNumber(item);
            } else if constexpr (std::is_same_v<T, double>) {
                return out.real(item);
            } else if constexpr (std::is_same_v<T, std::string>) {
                return out.text(item);
            } else if constexpr (std::is_same_v<T, document::Vec2d>) {
                return vector(out, {item.x, item.y});
            } else if constexpr (std::is_same_v<T, document::Vec3d>) {
                return vector(out, {item.x, item.y, item.z});
            } else if constexpr (std::is_same_v<T, core::Color4d>) {
                return vector(out, {item.red, item.green, item.blue, item.alpha});
            } else if constexpr (std::is_same_v<T, core::RationalTime>) {
                auto* result = out.array();
                out.append(result, out.signedNumber(item.numerator()));
                out.append(result, out.signedNumber(item.denominator()));
                return result;
            } else {
                auto* path = out.object();
                out.set(path, "closed", out.boolean(item.closed));
                auto* anchors = out.array();
                for (const auto& anchor : item.anchors) {
                    auto* row = out.object();
                    out.set(row, "point", vector(out, {anchor.point.x, anchor.point.y}));
                    out.set(row, "inHandle",
                            anchor.inHandle ? vector(out, {anchor.inHandle->x, anchor.inHandle->y})
                                            : out.null());
                    out.set(row, "outHandle",
                            anchor.outHandle
                                ? vector(out, {anchor.outHandle->x, anchor.outHandle->y})
                                : out.null());
                    out.set(row, "mode", out.text(document::pathAnchorModeName(anchor.mode)));
                    out.append(anchors, row);
                }
                out.set(path, "anchors", anchors);
                return path;
            }
        },
        value);
}
} // namespace

yyjson_mut_val* parameterRecord(Json& out, const document::ParameterRecord& value) {
    auto* record = out.object();
    out.set(record, "id", out.number(value.id.value()));
    out.set(record, "schemaKey", out.text(value.schemaKey));
    std::visit(
        [&](const auto& source) {
            using T = std::decay_t<decltype(source)>;
            if constexpr (std::is_same_v<T, document::ConstantValueSource>) {
                out.set(record, "source", out.text("constant"));
                out.set(record, "value", parameterValue(out, source.value));
                // An enumeration member reads back as both its stored value and its label.
                if (const auto* stored = std::get_if<std::int64_t>(&source.value)) {
                    for (const auto& option : document::parameterEnumOptions(value.schemaKey)) {
                        if (option.value == *stored)
                            out.set(record, "label", out.text(option.label));
                    }
                }
            } else if constexpr (std::is_same_v<T, document::AnimationCurveSource>) {
                out.set(record, "source", out.text("curve"));
                out.set(record, "curveId", out.number(source.curveId.value()));
            } else {
                out.set(record, "source", out.text("driver"));
                out.set(record, "nodeId", out.number(source.sourceNodeId.value()));
                out.set(record, "port", out.text(source.outputPort));
            }
        },
        value.source);
    return record;
}

yyjson_mut_val* noticeValue(Json& out, const McpNotice& notice) {
    auto* value = out.object();
    out.set(value, "kind", out.text(notice.kind));
    out.set(value, "revision", out.number(notice.revision));
    out.set(value, "message", out.text(notice.message));
    if (notice.composition)
        out.set(value, "composition", out.number(*notice.composition));
    if (notice.node || notice.parameter) {
        auto* subject = out.object();
        if (notice.composition)
            out.set(subject, "composition", out.number(*notice.composition));
        if (notice.node)
            out.set(subject, "node", out.number(*notice.node));
        if (notice.parameter)
            out.set(subject, "parameter", out.number(*notice.parameter));
        out.set(value, "subject", subject);
    }
    if (!notice.path.empty())
        out.set(value, "path", out.text(notice.path));
    if (!notice.projectName.empty())
        out.set(value, "projectName", out.text(notice.projectName));
    return value;
}

yyjson_mut_val* parameterSummary(Json& out, const document::ParameterRecord& value) {
    auto* record = out.object();
    out.set(record, "id", out.number(value.id.value()));
    out.set(record, "schemaKey", out.text(value.schemaKey));
    std::visit(
        [&](const auto& source) {
            using T = std::decay_t<decltype(source)>;
            if constexpr (std::is_same_v<T, document::ConstantValueSource>) {
                out.set(record, "source", out.text("constant"));
                out.set(record, "valueOmitted", out.boolean(true));
                if (const auto* text = std::get_if<std::string>(&source.value)) {
                    out.set(record, "valueType", out.text("string"));
                    out.set(record, "valueBytes", out.number(text->size()));
                } else {
                    out.set(record, "valueType", out.text("other"));
                }
            } else if constexpr (std::is_same_v<T, document::AnimationCurveSource>) {
                out.set(record, "source", out.text("curve"));
                out.set(record, "curveId", out.number(source.curveId.value()));
            } else {
                out.set(record, "source", out.text("driver"));
                out.set(record, "nodeId", out.number(source.sourceNodeId.value()));
                out.set(record, "port", out.text(source.outputPort));
            }
        },
        value.source);
    return record;
}
std::string base64(const std::span<const std::byte> bytes) {
    constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    std::size_t index = 0;
    for (; index + 3 <= bytes.size(); index += 3) {
        const auto value = (std::to_integer<unsigned>(bytes[index]) << 16U) |
                           (std::to_integer<unsigned>(bytes[index + 1]) << 8U) |
                           std::to_integer<unsigned>(bytes[index + 2]);
        out.push_back(alphabet[(value >> 18U) & 63U]);
        out.push_back(alphabet[(value >> 12U) & 63U]);
        out.push_back(alphabet[(value >> 6U) & 63U]);
        out.push_back(alphabet[value & 63U]);
    }
    if (const auto left = bytes.size() - index; left > 0) {
        auto value = std::to_integer<unsigned>(bytes[index]) << 16U;
        if (left == 2)
            value |= std::to_integer<unsigned>(bytes[index + 1]) << 8U;
        out.push_back(alphabet[(value >> 18U) & 63U]);
        out.push_back(alphabet[(value >> 12U) & 63U]);
        out.push_back(left == 2 ? alphabet[(value >> 6U) & 63U] : '=');
        out.push_back('=');
    }
    return out;
}
} // namespace bloom::mcp

namespace bloom::mcp {
namespace {

constexpr std::array<std::string_view, 13> kInterpolationLabels{
    "Constant", "Linear",      "Bezier",   "Sine", "Quadratic", "Cubic",  "Quartic",
    "Quintic",  "Exponential", "Circular", "Back", "Bounce",    "Elastic"};
constexpr std::array<std::string_view, 3> kHandleModeLabels{"Free", "Aligned", "Auto"};
constexpr std::array<std::string_view, 4> kEasingLabels{"Automatic Easing", "Ease In", "Ease Out",
                                                        "Ease In and Out"};

yyjson_mut_val* rational(Json& out, const core::RationalTime time) {
    auto* value = out.array();
    out.append(value, out.signedNumber(time.numerator()));
    out.append(value, out.signedNumber(time.denominator()));
    return value;
}

yyjson_mut_val* handle(Json& out, const document::KeyframeHandle& value) {
    auto* record = out.object();
    out.set(record, "time", out.real(value.time));
    out.set(record, "value", out.real(value.value));
    return record;
}

template <typename Keyframe> yyjson_mut_val* keyRecord(Json& out, const Keyframe& key) {
    auto* record = out.object();
    out.set(record, "id", out.number(key.id.value()));
    out.set(record, "time", rational(out, key.time));
    if constexpr (std::is_same_v<Keyframe, document::ScalarKeyframe>)
        out.set(record, "value", out.real(key.value));
    out.set(record, "interpolation",
            out.text(document::keyframeInterpolationToken(key.outgoingInterpolation)));
    out.set(record, "interpolationId",
            out.number(static_cast<std::uint64_t>(key.outgoingInterpolation)));
    out.set(record, "outgoingHandle", handle(out, key.outgoingHandle));
    out.set(record, "incomingHandle", handle(out, key.incomingHandle));
    out.set(record, "ease", out.text(document::keyframeEasingToken(key.easing.easing)));
    out.set(record, "back", out.real(key.easing.back));
    out.set(record, "amplitude", out.real(key.easing.amplitude));
    out.set(record, "period", out.real(key.easing.period));
    out.set(record, "handleMode", out.text(document::keyframeHandleModeToken(key.handleMode)));
    out.set(record, "handleModeId", out.number(static_cast<std::uint64_t>(key.handleMode)));
    return record;
}

template <typename Keyframes> yyjson_mut_val* keyList(Json& out, const Keyframes& keys) {
    auto* list = out.array();
    for (const auto& key : keys)
        out.append(list, keyRecord(out, key));
    return list;
}

constexpr std::array<std::string_view, 7> kComponentNames{"x",     "y",    "z",    "red",
                                                          "green", "blue", "alpha"};

} // namespace

void appendAnimationEnums(Json& out, yyjson_mut_val* records) {
    const auto table = [&](const std::string_view name, const auto& tokens, const auto& labels) {
        auto* record = out.object();
        out.set(record, "enum", out.text(name));
        auto* values = out.array();
        for (std::size_t index = 0; index < labels.size(); ++index) {
            auto* value = out.object();
            out.set(value, "id", out.number(index));
            out.set(value, "token", out.text(tokens(index)));
            out.set(value, "label", out.text(labels[index]));
            out.append(values, value);
        }
        out.set(record, "values", values);
        out.append(records, record);
    };
    table(
        "interpolation",
        [](const std::size_t index) {
            return document::keyframeInterpolationToken(
                static_cast<document::KeyframeInterpolation>(index));
        },
        kInterpolationLabels);
    table(
        "ease",
        [](const std::size_t index) {
            return document::keyframeEasingToken(static_cast<document::KeyframeEasing>(index));
        },
        kEasingLabels);
    table(
        "handleMode",
        [](const std::size_t index) {
            return document::keyframeHandleModeToken(
                static_cast<document::KeyframeHandleMode>(index));
        },
        kHandleModeLabels);
    constexpr std::array<std::string_view, 3> easyEaseTokens{"both", "in", "out"};
    constexpr std::array<std::string_view, 3> easyEaseLabels{"Easy Ease", "Easy Ease In",
                                                             "Easy Ease Out"};
    table(
        "easyEase", [&](const std::size_t index) { return easyEaseTokens[index]; }, easyEaseLabels);
}

void appendLayerRecords(Json& out, yyjson_mut_val* records, const document::Project& project,
                        const document::Composition& composition) {
    for (const auto& layer : composition.graph().layerOutputs()) {
        auto* record = out.object();
        out.set(record, "id", out.number(layer.layerId.value()));
        out.set(record, "name", out.text(layer.name));
        out.set(record, "nodeId", out.number(layer.nodeId.value()));
        out.set(record, "enabled", out.boolean(layer.enabled));
        out.set(record, "solo", out.boolean(layer.solo));
        out.set(record, "locked", out.boolean(layer.locked));
        out.set(record, "shy", out.boolean(layer.shy));
        out.set(record, "nameFollowsSource", out.boolean(layer.nameFollowsSource));
        if (const auto source = document::layerSourceName(project, composition, layer))
            out.set(record, "sourceName", out.text(*source));
        out.set(record, "parent", layer.parent ? out.number(layer.parent->value()) : out.null());
        out.set(record, "inPoint", rational(out, layer.inPoint));
        out.set(record, "outPoint", rational(out, layer.outPoint));
        out.append(records, record);
    }
}

void appendKeyframeRecords(Json& out, yyjson_mut_val* records,
                           const document::Composition& composition) {
    for (const auto& curve : composition.animationCurves().records()) {
        auto* record = out.object();
        out.set(record, "curve", out.number(document::animationCurveId(curve).value()));
        std::visit(
            [&](const auto& typed) {
                using Curve = std::decay_t<decltype(typed)>;
                if constexpr (std::is_same_v<Curve, document::ScalarAnimationCurve>) {
                    out.set(record, "kind", out.text("scalar"));
                    out.set(record, "keys", keyList(out, typed.keyframes));
                } else if constexpr (std::is_same_v<Curve, document::PathAnimationCurve>) {
                    out.set(record, "kind", out.text("path"));
                    out.set(record, "keys", keyList(out, typed.keyframes));
                } else {
                    const bool color = std::is_same_v<Curve, document::Color4AnimationCurve>;
                    out.set(record, "kind",
                            out.text(color                          ? "color4"
                                     : typed.components.size() == 2 ? "vec2"
                                                                    : "vec3"));
                    auto* components = out.object();
                    for (std::size_t index = 0; index < typed.components.size(); ++index)
                        out.set(components, kComponentNames[(color ? 3 : 0) + index],
                                keyList(out, typed.components[index].keyframes));
                    out.set(record, "components", components);
                }
            },
            curve);
        out.append(records, record);
    }
}

} // namespace bloom::mcp
