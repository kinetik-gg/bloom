#include "native.hpp"

#include <nanobind/stl/string.h>
#include <nanobind/stl/string_view.h>

#include <type_traits>

namespace bloom::scripting::python {
namespace {

nb::object parameterValue(const document::ParameterValue& value) {
    return std::visit(
        [](const auto& item) -> nb::object {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, document::Vec2d>) {
                return nb::make_tuple(item.x, item.y);
            } else if constexpr (std::is_same_v<T, document::Vec3d>) {
                return nb::make_tuple(item.x, item.y, item.z);
            } else if constexpr (std::is_same_v<T, core::Color4d>) {
                return nb::make_tuple(item.red, item.green, item.blue, item.alpha);
            } else if constexpr (std::is_same_v<T, core::RationalTime>) {
                return nb::make_tuple(item.numerator(), item.denominator());
            } else if constexpr (std::is_same_v<T, document::PathValue>) {
                nb::dict path;
                path["closed"] = item.closed;
                nb::list anchors;
                for (const auto& anchor : item.anchors) {
                    nb::dict row;
                    row["point"] = nb::make_tuple(anchor.point.x, anchor.point.y);
                    row["in_handle"] = anchor.inHandle
                                           ? nb::make_tuple(anchor.inHandle->x, anchor.inHandle->y)
                                           : nb::object(nb::none());
                    row["out_handle"] =
                        anchor.outHandle ? nb::make_tuple(anchor.outHandle->x, anchor.outHandle->y)
                                         : nb::object(nb::none());
                    anchors.append(row);
                }
                path["anchors"] = anchors;
                return path;
            } else {
                return nb::cast(item);
            }
        },
        value);
}

nb::list keyframes(const std::vector<document::ScalarKeyframe>& keys) {
    nb::list result;
    for (const auto& key : keys) {
        nb::dict row;
        row["id"] = key.id.value();
        row["time"] = nb::make_tuple(key.time.numerator(), key.time.denominator());
        row["value"] = key.value;
        row["interpolation"] = static_cast<int>(key.outgoingInterpolation);
        result.append(row);
    }
    return result;
}

} // namespace

nb::dict projectSnapshot(const document::Snapshot& snapshot) {
    nb::dict result;
    result["revision"] = snapshot.revision().value();
    nb::dict project;
    project["id"] = snapshot.project().id().value();
    project["name"] = snapshot.project().name();
    result["project"] = project;
    nb::list compositions;
    for (const auto& composition : snapshot.project().compositions()) {
        nb::dict row;
        row["id"] = composition.id().value();
        row["name"] = composition.name();
        row["width"] = composition.format().width();
        row["height"] = composition.format().height();
        row["frame_rate"] = nb::make_tuple(composition.format().frameRate().numerator(),
                                           composition.format().frameRate().denominator());
        row["duration"] = nb::make_tuple(composition.duration().numerator(),
                                         composition.duration().denominator());
        nb::list nodes;
        for (const auto& node : composition.graph().nodes()) {
            nb::dict record;
            record["id"] = node.id.value();
            record["type_id"] = node.typeId;
            nb::dict bindings;
            for (const auto& binding : node.parameters) {
                bindings[nb::str(binding.role.c_str())] = binding.parameterId.value();
            }
            record["parameters"] = bindings;
            nodes.append(record);
        }
        row["nodes"] = nodes;
        nb::list parameters;
        for (const auto& parameter : composition.parameters().records()) {
            nb::dict record;
            record["id"] = parameter.id.value();
            record["schema_key"] = parameter.schemaKey;
            std::visit(
                [&record](const auto& source) {
                    using T = std::decay_t<decltype(source)>;
                    if constexpr (std::is_same_v<T, document::ConstantValueSource>) {
                        record["source"] = "constant";
                        record["value"] = parameterValue(source.value);
                    } else if constexpr (std::is_same_v<T, document::AnimationCurveSource>) {
                        record["source"] = "curve";
                        record["curve_id"] = source.curveId.value();
                    } else {
                        record["source"] = "driver";
                        record["node_id"] = source.sourceNodeId.value();
                        record["port"] = source.outputPort;
                    }
                },
                parameter.source);
            parameters.append(record);
        }
        row["parameters"] = parameters;
        nb::list curves;
        for (const auto& curve : composition.animationCurves().records()) {
            nb::dict record;
            record["id"] = document::animationCurveId(curve).value();
            nb::list components;
            std::visit(
                [&components](const auto& item) {
                    using T = std::decay_t<decltype(item)>;
                    if constexpr (std::is_same_v<T, document::ScalarAnimationCurve>) {
                        components.append(keyframes(item.keyframes));
                    } else {
                        for (const auto& component : item.components) {
                            components.append(keyframes(component.keyframes));
                        }
                    }
                },
                curve);
            record["components"] = components;
            curves.append(record);
        }
        row["curves"] = curves;
        compositions.append(row);
    }
    result["compositions"] = compositions;
    nb::list assets;
    for (const auto& asset : snapshot.project().assets()) {
        nb::dict record;
        record["id"] = asset.id.value();
        record["name"] = asset.name;
        record["kind"] = static_cast<int>(asset.kind);
        record["path"] = asset.locator.path;
        record["width"] = asset.width;
        record["height"] = asset.height;
        const auto digest = asset.contentDigest.toLowercaseHex();
        record["digest"] = "sha256:" + std::string(digest.data(), digest.size());
        assets.append(record);
    }
    result["assets"] = assets;
    return result;
}

nb::dict commandResult(const commands::CommandResult& result) {
    nb::dict row;
    row["succeeded"] = result.succeeded();
    row["revision"] = result.afterRevision.value();
    row["status"] = static_cast<int>(result.status);
    nb::list diagnostics;
    for (const auto& failure : result.operationFailures) {
        nb::dict diagnostic;
        diagnostic["code"] = "bloom.scripting.command-rejected";
        diagnostic["operation_id"] = failure.operationType;
        diagnostic["argument"] = "";
        diagnostic["message"] = failure.issue.message;
        diagnostics.append(diagnostic);
    }
    if (!result.succeeded() && diagnostics.empty()) {
        nb::dict diagnostic;
        diagnostic["code"] = result.status == commands::CommandStatus::StaleRevision
                                 ? "bloom.scripting.stale-revision"
                                 : "bloom.scripting.command-rejected";
        diagnostic["message"] = "The host refused the transaction";
        diagnostics.append(diagnostic);
    }
    row["diagnostics"] = diagnostics;
    nb::list outputs;
    for (const auto& output : result.outputs) {
        nb::dict value;
        value["operation_index"] = output.operationIndex;
        value["name"] = output.output.name;
        value["id"] = std::visit([](const auto& id) { return id.value(); }, output.output.id);
        outputs.append(value);
    }
    row["outputs"] = outputs;
    return row;
}

} // namespace bloom::scripting::python
