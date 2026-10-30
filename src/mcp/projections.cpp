// Parameter projections shared by both hosts (moved from apps/bloom-mcp).
#include "core_internal.hpp"

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
