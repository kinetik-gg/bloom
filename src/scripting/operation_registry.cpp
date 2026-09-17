#include <bloom/commands/operations.hpp>
#include <bloom/scripting/operation_registry.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <limits>
#include <map>
#include <ranges>
#include <set>
#include <string>
#include <utility>

namespace bloom::scripting {
namespace {

[[nodiscard]] OperationCreateResult failure(const std::string& operationId, std::string argument,
                                            std::string message) {
    return OperationCreateResult(nullptr,
                                 OperationDiagnostic{.code = "bloom.scripting.invalid-argument",
                                                     .operationId = operationId,
                                                     .argument = std::move(argument),
                                                     .message = std::move(message)});
}

[[nodiscard]] const Value* findArgument(const Arguments& arguments, const std::string& name) {
    const auto iterator = arguments.find(name);
    return iterator == arguments.end() ? nullptr : &iterator->second;
}

// ---------------------------------------------------------------------------------------------
// Value readers. Every one of them phrases its rejection as a predicate ("expects ...") because
// OperationRegistry::create prefixes the operation and argument and appends a runnable example.
// ---------------------------------------------------------------------------------------------

[[nodiscard]] std::optional<double> numericOf(const Value& value) {
    if (const auto* result = std::get_if<double>(&value.storage)) {
        return *result;
    }
    if (const auto* result = std::get_if<std::int64_t>(&value.storage)) {
        return static_cast<double>(*result);
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::int64_t> integerOf(const Value& value) {
    if (const auto* result = std::get_if<std::int64_t>(&value.storage)) {
        return *result;
    }
    if (const auto* result = std::get_if<StableId>(&value.storage)) {
        return static_cast<std::int64_t>(result->value);
    }
    if (const auto* result = std::get_if<double>(&value.storage);
        result != nullptr && std::isfinite(*result) && std::floor(*result) == *result) {
        return static_cast<std::int64_t>(*result);
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::uint64_t> idOf(const Value& value) {
    const auto number = integerOf(value);
    return number.has_value() && *number > 0 ? std::optional(static_cast<std::uint64_t>(*number))
                                             : std::nullopt;
}

[[nodiscard]] std::optional<core::RationalTime> rationalOf(const Value& value) {
    if (const auto* whole = std::get_if<std::int64_t>(&value.storage)) {
        return core::RationalTime::fromInteger(*whole);
    }
    if (const auto* real = std::get_if<double>(&value.storage);
        real != nullptr && std::isfinite(*real) && std::floor(*real) == *real) {
        return core::RationalTime::fromInteger(static_cast<std::int64_t>(*real));
    }
    if (const auto* array = std::get_if<ValueArray>(&value.storage);
        array != nullptr && array->size() == 2) {
        const auto numerator = integerOf((*array)[0]);
        const auto denominator = integerOf((*array)[1]);
        if (numerator.has_value() && denominator.has_value()) {
            return core::RationalTime::create(*numerator, *denominator);
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<document::Vec2d> vec2Of(const Value& value) {
    if (const auto* result = std::get_if<document::Vec2d>(&value.storage)) {
        return *result;
    }
    if (const auto* array = std::get_if<ValueArray>(&value.storage);
        array != nullptr && array->size() == 2) {
        const auto x = numericOf((*array)[0]);
        const auto y = numericOf((*array)[1]);
        if (x.has_value() && y.has_value() && std::isfinite(*x) && std::isfinite(*y)) {
            return document::Vec2d{*x, *y};
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<document::Vec3d> vec3Of(const Value& value) {
    if (const auto* result = std::get_if<document::Vec3d>(&value.storage)) {
        return *result;
    }
    if (const auto* array = std::get_if<ValueArray>(&value.storage);
        array != nullptr && array->size() == 3) {
        std::array<double, 3> components{};
        for (std::size_t index = 0; index < components.size(); ++index) {
            const auto component = numericOf((*array)[index]);
            if (!component.has_value() || !std::isfinite(*component)) {
                return std::nullopt;
            }
            components[index] = *component;
        }
        return document::Vec3d{components[0], components[1], components[2]};
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<core::Color4d> color4Of(const Value& value) {
    if (const auto* result = std::get_if<core::Color4d>(&value.storage)) {
        return result->isValid() ? std::optional(*result) : std::nullopt;
    }
    if (const auto* array = std::get_if<ValueArray>(&value.storage);
        array != nullptr && array->size() == 4) {
        std::array<double, 4> channels{};
        for (std::size_t index = 0; index < channels.size(); ++index) {
            const auto channel = numericOf((*array)[index]);
            if (!channel.has_value() || !std::isfinite(*channel)) {
                return std::nullopt;
            }
            channels[index] = *channel;
        }
        const core::Color4d result{channels[0], channels[1], channels[2], channels[3]};
        return result.isValid() ? std::optional(result) : std::nullopt;
    }
    return std::nullopt;
}

using AnimationValue = std::variant<double, document::Vec2d, document::Vec3d, core::Color4d>;

// The width a parameter-keyed animation operation takes is the parameter's own; the caller states
// it by the shape of the value rather than by naming a separate kind.
[[nodiscard]] std::optional<AnimationValue> animationValueOf(const Value& value) {
    if (const auto* array = std::get_if<ValueArray>(&value.storage)) {
        if (array->size() == 2) {
            if (const auto result = vec2Of(value)) {
                return AnimationValue(*result);
            }
        }
        if (array->size() == 3) {
            if (const auto result = vec3Of(value)) {
                return AnimationValue(*result);
            }
        }
        if (array->size() == 4) {
            if (const auto result = color4Of(value)) {
                return AnimationValue(*result);
            }
        }
        return std::nullopt;
    }
    if (const auto number = numericOf(value); number.has_value() && std::isfinite(*number)) {
        return AnimationValue(*number);
    }
    if (const auto* result = std::get_if<document::Vec2d>(&value.storage)) {
        return AnimationValue(*result);
    }
    if (const auto* result = std::get_if<document::Vec3d>(&value.storage)) {
        return AnimationValue(*result);
    }
    if (const auto* result = std::get_if<core::Color4d>(&value.storage)) {
        return AnimationValue(*result);
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<document::ParameterValue> parameterValueOf(const Value& value) {
    if (const auto* result = std::get_if<bool>(&value.storage)) {
        return document::ParameterValue(*result);
    }
    if (const auto* result = std::get_if<std::string>(&value.storage)) {
        return document::ParameterValue(*result);
    }
    const auto authoredValue = animationValueOf(value);
    if (!authoredValue.has_value()) {
        return std::nullopt;
    }
    return std::visit([](const auto& held) { return document::ParameterValue(held); },
                      *authoredValue);
}

[[nodiscard]] std::optional<document::KeyframeInterpolation> interpolationOf(const Value& value) {
    const auto* name = std::get_if<std::string>(&value.storage);
    if (name == nullptr) {
        return std::nullopt;
    }
    if (*name == "hold") {
        return document::KeyframeInterpolation::Hold;
    }
    if (*name == "linear") {
        return document::KeyframeInterpolation::Linear;
    }
    if (*name == "ease-in-out") {
        return document::KeyframeInterpolation::EaseInOut;
    }
    return std::nullopt;
}

// -1 is "the whole value", the address a scalar parameter and a component-less batch entry use.
[[nodiscard]] bool componentOf(const Value& value,
                               std::optional<document::AnimationComponent>& component) {
    const auto number = integerOf(value);
    if (!number.has_value() || *number < -1 || *number > 6) {
        return false;
    }
    component = *number < 0 ? std::optional<document::AnimationComponent>{}
                            : std::optional(static_cast<document::AnimationComponent>(*number));
    return true;
}

[[nodiscard]] std::optional<document::ShapeKind> shapeKindOf(const Value& value) {
    const auto* name = std::get_if<std::string>(&value.storage);
    if (name == nullptr) {
        return std::nullopt;
    }
    constexpr std::array<std::string_view, 7> names{"rectangle", "ellipse", "triangle", "polygon",
                                                    "star",      "line",    "path"};
    const auto* const found = std::ranges::find(names, *name);
    return found == names.end() ? std::nullopt
                                : std::optional(static_cast<document::ShapeKind>(
                                      std::distance(names.begin(), found)));
}

[[nodiscard]] std::optional<document::DataBlockKind> dataBlockKindOf(const Value& value) {
    const auto* name = std::get_if<std::string>(&value.storage);
    if (name == nullptr) {
        return std::nullopt;
    }
    constexpr std::array<std::string_view, 13> names{
        "image", "sequence",  "video", "audio", "font",     "curve", "ramp",
        "table", "point-set", "path",  "mask",  "analysis", "opaque"};
    const auto* const found = std::ranges::find(names, *name);
    return found == names.end() ? std::nullopt
                                : std::optional(static_cast<document::DataBlockKind>(
                                      std::distance(names.begin(), found)));
}

// Typed argument accessors. `error` is written only on the first rejection so a factory can read
// every argument and still report the first thing that was wrong.
template <typename Reader>
[[nodiscard]] auto read(const Arguments& arguments, const std::string& operationId,
                        const std::string& name, OperationCreateResult& error, const bool required,
                        const std::string& expectation, Reader reader)
    -> decltype(reader(std::declval<const Value&>())) {
    using Result = decltype(reader(std::declval<const Value&>()));
    const auto* value = findArgument(arguments, name);
    if (value == nullptr) {
        if (required && !error) {
            error = failure(operationId, name, "is required");
        }
        return Result{};
    }
    auto result = reader(*value);
    if (!result.has_value() && !error) {
        error = failure(operationId, name, expectation);
    }
    return result;
}

[[nodiscard]] std::optional<bool> boolean(const Arguments& arguments,
                                          const std::string& operationId, const std::string& name,
                                          OperationCreateResult& error,
                                          const bool required = true) {
    return read(arguments, operationId, name, error, required, "expects true or false",
                [](const Value& value) -> std::optional<bool> {
                    const auto* result = std::get_if<bool>(&value.storage);
                    return result == nullptr ? std::nullopt : std::optional(*result);
                });
}

[[nodiscard]] std::optional<std::int64_t>
integer(const Arguments& arguments, const std::string& operationId, const std::string& name,
        OperationCreateResult& error, const bool required = true) {
    return read(arguments, operationId, name, error, required, "expects a whole number", integerOf);
}

[[nodiscard]] std::optional<double> number(const Arguments& arguments,
                                           const std::string& operationId, const std::string& name,
                                           OperationCreateResult& error,
                                           const bool required = true) {
    return read(arguments, operationId, name, error, required, "expects a finite number",
                [](const Value& value) -> std::optional<double> {
                    const auto result = numericOf(value);
                    return result.has_value() && std::isfinite(*result) ? result : std::nullopt;
                });
}

[[nodiscard]] std::optional<std::string> text(const Arguments& arguments,
                                              const std::string& operationId,
                                              const std::string& name, OperationCreateResult& error,
                                              const bool required = true) {
    return read(arguments, operationId, name, error, required, "expects text",
                [](const Value& value) -> std::optional<std::string> {
                    const auto* result = std::get_if<std::string>(&value.storage);
                    return result == nullptr ? std::nullopt : std::optional(*result);
                });
}

[[nodiscard]] std::optional<std::uint64_t> id(const Arguments& arguments,
                                              const std::string& operationId,
                                              const std::string& name, OperationCreateResult& error,
                                              const bool required = true) {
    return read(arguments, operationId, name, error, required,
                "expects a positive stable id or a live proxy", idOf);
}

[[nodiscard]] std::optional<document::Vec2d>
vec2(const Arguments& arguments, const std::string& operationId, const std::string& name,
     OperationCreateResult& error, const bool required = true) {
    return read(arguments, operationId, name, error, required, "expects two numbers as (x, y)",
                vec2Of);
}

[[nodiscard]] std::optional<document::Vec3d>
vec3(const Arguments& arguments, const std::string& operationId, const std::string& name,
     OperationCreateResult& error, const bool required = true) {
    return read(arguments, operationId, name, error, required, "expects three numbers as (x, y, z)",
                vec3Of);
}

[[nodiscard]] std::optional<core::Color4d>
color4(const Arguments& arguments, const std::string& operationId, const std::string& name,
       OperationCreateResult& error, const bool required = true) {
    return read(arguments, operationId, name, error, required,
                "expects four numbers as (r, g, b, a) with alpha in 0..1", color4Of);
}

[[nodiscard]] std::optional<core::RationalTime>
rational(const Arguments& arguments, const std::string& operationId, const std::string& name,
         OperationCreateResult& error, const bool required = true) {
    return read(arguments, operationId, name, error, required,
                "expects a whole frame or an exact (numerator, denominator) time", rationalOf);
}

[[nodiscard]] std::optional<document::KeyframeInterpolation>
interpolation(const Arguments& arguments, const std::string& operationId, const std::string& name,
              OperationCreateResult& error, const bool required = true) {
    return read(arguments, operationId, name, error, required,
                R"(expects "hold", "linear" or "ease-in-out")", interpolationOf);
}

[[nodiscard]] std::optional<AnimationValue>
authored(const Arguments& arguments, const std::string& operationId, const std::string& name,
         OperationCreateResult& error, const bool required = true) {
    return read(arguments, operationId, name, error, required,
                "expects a number, (x, y), (x, y, z) or (r, g, b, a)", animationValueOf);
}

[[nodiscard]] std::optional<ValueArray>
list(const Arguments& arguments, const std::string& operationId, const std::string& name,
     OperationCreateResult& error, const std::string& expectation, const bool required = true) {
    return read(arguments, operationId, name, error, required, expectation,
                [](const Value& value) -> std::optional<ValueArray> {
                    const auto* result = std::get_if<ValueArray>(&value.storage);
                    return result == nullptr ? std::nullopt : std::optional(*result);
                });
}

[[nodiscard]] std::optional<std::vector<std::string>>
strings(const Arguments& arguments, const std::string& operationId, const std::string& name,
        OperationCreateResult& error, const bool required = true) {
    return read(arguments, operationId, name, error, required, "expects a list of text values",
                [](const Value& value) -> std::optional<std::vector<std::string>> {
                    const auto* array = std::get_if<ValueArray>(&value.storage);
                    if (array == nullptr || array->size() > 4096) {
                        return std::nullopt;
                    }
                    std::vector<std::string> result;
                    result.reserve(array->size());
                    for (const auto& entry : *array) {
                        const auto* item = std::get_if<std::string>(&entry.storage);
                        if (item == nullptr) {
                            return std::nullopt;
                        }
                        result.push_back(*item);
                    }
                    return result;
                });
}

[[nodiscard]] std::optional<std::vector<std::uint64_t>>
identities(const Arguments& arguments, const std::string& operationId, const std::string& name,
           OperationCreateResult& error, const bool required = true) {
    return read(arguments, operationId, name, error, required,
                "expects 1..4096 stable ids or live proxies",
                [](const Value& value) -> std::optional<std::vector<std::uint64_t>> {
                    const auto* array = std::get_if<ValueArray>(&value.storage);
                    if (array == nullptr || array->empty() || array->size() > 4096) {
                        return std::nullopt;
                    }
                    std::vector<std::uint64_t> result;
                    result.reserve(array->size());
                    for (const auto& entry : *array) {
                        const auto item = idOf(entry);
                        if (!item.has_value()) {
                            return std::nullopt;
                        }
                        result.push_back(*item);
                    }
                    return result;
                });
}

// ---------------------------------------------------------------------------------------------
// Composite readers shared by the batch animation operations. Each batch entry is a flat list so
// the same shape reads identically from Python tuples, JSON arrays and MCP arguments.
// ---------------------------------------------------------------------------------------------

[[nodiscard]] bool keyframeAddressOf(const Value& value, commands::KeyframeAddress& address) {
    const auto* entry = std::get_if<ValueArray>(&value.storage);
    if (entry == nullptr || entry->size() < 2 || entry->size() > 3) {
        return false;
    }
    const auto curve = idOf((*entry)[0]);
    const auto keyframe = idOf((*entry)[1]);
    if (!curve.has_value() || !keyframe.has_value()) {
        return false;
    }
    address.curveId = document::AnimationCurveId::fromRaw(*curve);
    address.keyframeId = document::KeyframeId::fromRaw(*keyframe);
    address.component.reset();
    return entry->size() == 2 || componentOf((*entry)[2], address.component);
}

[[nodiscard]] std::optional<std::vector<commands::KeyframeAddress>>
keyframeAddressesOf(const Value& value) {
    const auto* array = std::get_if<ValueArray>(&value.storage);
    if (array == nullptr || array->empty() || array->size() > 4096) {
        return std::nullopt;
    }
    std::vector<commands::KeyframeAddress> result;
    result.reserve(array->size());
    for (const auto& entry : *array) {
        commands::KeyframeAddress address;
        if (!keyframeAddressOf(entry, address)) {
            return std::nullopt;
        }
        result.push_back(address);
    }
    return result;
}

[[nodiscard]] std::optional<std::vector<commands::KeyframeMove>>
keyframeMovesOf(const Value& value) {
    const auto* array = std::get_if<ValueArray>(&value.storage);
    if (array == nullptr || array->empty() || array->size() > 4096) {
        return std::nullopt;
    }
    std::vector<commands::KeyframeMove> result;
    result.reserve(array->size());
    for (const auto& item : *array) {
        const auto* entry = std::get_if<ValueArray>(&item.storage);
        if (entry == nullptr || entry->size() < 3 || entry->size() > 4) {
            return std::nullopt;
        }
        const auto curve = idOf((*entry)[0]);
        const auto keyframe = idOf((*entry)[1]);
        const auto time = rationalOf((*entry)[2]);
        if (!curve.has_value() || !keyframe.has_value() || !time.has_value()) {
            return std::nullopt;
        }
        commands::KeyframeMove move{.key = {.curveId = document::AnimationCurveId::fromRaw(*curve),
                                            .keyframeId = document::KeyframeId::fromRaw(*keyframe),
                                            .component = {}},
                                    .time = *time};
        if (entry->size() == 4 && !componentOf((*entry)[3], move.key.component)) {
            return std::nullopt;
        }
        result.push_back(move);
    }
    return result;
}

[[nodiscard]] std::optional<std::vector<commands::KeyframeValueEdit>>
keyframeValuesOf(const Value& value) {
    const auto* array = std::get_if<ValueArray>(&value.storage);
    if (array == nullptr || array->empty() || array->size() > 4096) {
        return std::nullopt;
    }
    std::vector<commands::KeyframeValueEdit> result;
    result.reserve(array->size());
    for (const auto& item : *array) {
        const auto* entry = std::get_if<ValueArray>(&item.storage);
        if (entry == nullptr || entry->size() < 3 || entry->size() > 4) {
            return std::nullopt;
        }
        const auto curve = idOf((*entry)[0]);
        const auto keyframe = idOf((*entry)[1]);
        const auto scalar = numericOf((*entry)[2]);
        if (!curve.has_value() || !keyframe.has_value() || !scalar.has_value() ||
            !std::isfinite(*scalar)) {
            return std::nullopt;
        }
        commands::KeyframeValueEdit edit{
            .key = {.curveId = document::AnimationCurveId::fromRaw(*curve),
                    .keyframeId = document::KeyframeId::fromRaw(*keyframe),
                    .component = {}},
            .value = *scalar};
        if (entry->size() == 4 && !componentOf((*entry)[3], edit.key.component)) {
            return std::nullopt;
        }
        result.push_back(edit);
    }
    return result;
}

// One side of an ease. `(curve, keyframe, time, value)`, optionally with a trailing component.
[[nodiscard]] bool mergeHandles(const ValueArray& array, const bool outgoing,
                                std::vector<commands::KeyframeHandleEdit>& edits) {
    if (array.empty() || array.size() > 4096) {
        return false;
    }
    for (const auto& item : array) {
        const auto* entry = std::get_if<ValueArray>(&item.storage);
        if (entry == nullptr || entry->size() < 4 || entry->size() > 5) {
            return false;
        }
        const auto curve = idOf((*entry)[0]);
        const auto keyframe = idOf((*entry)[1]);
        const auto time = numericOf((*entry)[2]);
        const auto offset = numericOf((*entry)[3]);
        if (!curve.has_value() || !keyframe.has_value() || !time.has_value() ||
            !offset.has_value() || !std::isfinite(*time) || !std::isfinite(*offset)) {
            return false;
        }
        commands::KeyframeAddress address{.curveId = document::AnimationCurveId::fromRaw(*curve),
                                          .keyframeId = document::KeyframeId::fromRaw(*keyframe),
                                          .component = {}};
        if (entry->size() == 5 && !componentOf((*entry)[4], address.component)) {
            return false;
        }
        const document::KeyframeHandle handle{.time = *time, .value = *offset};
        auto found = std::ranges::find(edits, address, &commands::KeyframeHandleEdit::key);
        if (found == edits.end()) {
            edits.push_back({.key = address, .outgoing = {}, .incoming = {}});
            found = std::prev(edits.end());
        }
        if (outgoing) {
            found->outgoing = handle;
        } else {
            found->incoming = handle;
        }
    }
    return true;
}

[[nodiscard]] std::optional<std::vector<commands::KeyframePaste>>
keyframePastesOf(const Value& value) {
    const auto* array = std::get_if<ValueArray>(&value.storage);
    if (array == nullptr || array->empty() || array->size() > 4096) {
        return std::nullopt;
    }
    std::vector<commands::KeyframePaste> result;
    result.reserve(array->size());
    for (const auto& item : *array) {
        const auto* entry = std::get_if<ValueArray>(&item.storage);
        if (entry == nullptr || entry->size() < 3 || entry->size() > 5) {
            return std::nullopt;
        }
        const auto parameter = idOf((*entry)[0]);
        const auto time = rationalOf((*entry)[1]);
        const auto authoredValue = animationValueOf((*entry)[2]);
        if (!parameter.has_value() || !time.has_value() || !authoredValue.has_value()) {
            return std::nullopt;
        }
        commands::KeyframePaste paste{.parameterId = document::ParameterId::fromRaw(*parameter),
                                      .time = *time,
                                      .value = 0.0,
                                      .interpolation = document::KeyframeInterpolation::Linear,
                                      .component = {},
                                      .outgoingHandle = {},
                                      .incomingHandle = {}};
        std::visit([&paste](const auto& held) { paste.value = held; }, *authoredValue);
        if (entry->size() >= 4 && !componentOf((*entry)[3], paste.component)) {
            return std::nullopt;
        }
        if (entry->size() == 5) {
            const auto mode = interpolationOf((*entry)[4]);
            if (!mode.has_value()) {
                return std::nullopt;
            }
            paste.interpolation = *mode;
        }
        result.push_back(paste);
    }
    return result;
}

// ---------------------------------------------------------------------------------------------
// Shared record builders.
// ---------------------------------------------------------------------------------------------

[[nodiscard]] std::optional<document::CompositionFormat>
format(const Arguments& arguments, const std::string& operationId, OperationCreateResult& error,
       const std::int64_t defaultWidth, const std::int64_t defaultHeight) {
    const auto width =
        integer(arguments, operationId, "width", error, defaultWidth <= 0).value_or(defaultWidth);
    const auto height = integer(arguments, operationId, "height", error, defaultHeight <= 0)
                            .value_or(defaultHeight);
    const auto numerator = number(arguments, operationId, "frameRateNumerator", error, false);
    const auto denominator = number(arguments, operationId, "frameRateDenominator", error, false);
    if (error) {
        return std::nullopt;
    }
    const std::array<std::pair<std::optional<double>, const char*>, 2> components{
        {{numerator, "frameRateNumerator"}, {denominator, "frameRateDenominator"}}};
    for (const auto& [component, name] : components) {
        if (component.has_value() && (!std::isfinite(*component) || *component < 1 ||
                                      *component > std::numeric_limits<std::uint32_t>::max() ||
                                      std::floor(*component) != *component)) {
            error = failure(operationId, name, "expects a whole number in 1..4294967295");
            return std::nullopt;
        }
    }
    const auto frameRate = document::FrameRate::create(
        numerator.has_value() ? static_cast<std::uint32_t>(*numerator) : 24U,
        denominator.has_value() ? static_cast<std::uint32_t>(*denominator) : 1U);
    if (!frameRate.has_value()) {
        error = failure(operationId, "frameRateNumerator", "expects a representable frame rate");
        return std::nullopt;
    }
    if (width <= 0 || height <= 0 ||
        static_cast<std::uint64_t>(width) > document::CompositionFormat::kMaximumDimension ||
        static_cast<std::uint64_t>(height) > document::CompositionFormat::kMaximumDimension) {
        error = failure(operationId, "width", "expects positive pixel dimensions");
        return std::nullopt;
    }
    auto result = document::CompositionFormat::create(static_cast<std::uint32_t>(width),
                                                      static_cast<std::uint32_t>(height),
                                                      core::PixelAspectRatio::square(), *frameRate);
    if (!result.has_value()) {
        error = failure(operationId, "width", "expects a representable frame size");
    }
    return result;
}

// A font face the artist already picked: its bytes are addressed by path and identified by digest,
// exactly the pair the Assets and Properties surfaces build from a platform font face.
[[nodiscard]] std::optional<document::AssetRecord> fontRecord(const Arguments& arguments,
                                                              const std::string& operationId,
                                                              OperationCreateResult& error) {
    const auto path = text(arguments, operationId, "path", error);
    const auto digest = text(arguments, operationId, "digest", error);
    const auto family = text(arguments, operationId, "family", error, false);
    const auto style = text(arguments, operationId, "style", error, false);
    const auto index = integer(arguments, operationId, "index", error, false);
    const auto portability = text(arguments, operationId, "portability", error, false);
    const auto relinkHint = text(arguments, operationId, "relinkHint", error, false);
    const auto name = text(arguments, operationId, "name", error, false);
    const auto tags = strings(arguments, operationId, "tags", error, false);
    if (!path.has_value() || !digest.has_value() || error) {
        return std::nullopt;
    }
    std::string_view hex = *digest;
    if (hex.starts_with("sha256:")) {
        hex.remove_prefix(std::string_view("sha256:").size());
    }
    const auto contentDigest = core::Sha256Digest::fromLowercaseHex(hex);
    if (!contentDigest.has_value()) {
        error = failure(operationId, "digest", "expects 64 lowercase hex characters");
        return std::nullopt;
    }
    if (index.value_or(0) < 0 || index.value_or(0) > std::numeric_limits<std::uint32_t>::max()) {
        error = failure(operationId, "index", "expects a face index in 0..4294967295");
        return std::nullopt;
    }
    document::AssetRecord asset;
    asset.kind = document::AssetKind::Font;
    asset.contentDigest = *contentDigest;
    asset.fontFamily = family.value_or(std::string());
    asset.fontStyle = style.value_or(std::string());
    asset.fontIndex = static_cast<std::uint32_t>(index.value_or(0));
    asset.locator = {"font", portability.value_or(std::string("system")), *path,
                     relinkHint.value_or(std::string())};
    asset.name = name.value_or(std::string());
    asset.tags = tags.value_or(std::vector<std::string>{});
    return asset;
}

[[nodiscard]] std::optional<document::DataBlockPayload>
payload(const Arguments& arguments, const std::string& operationId, OperationCreateResult& error) {
    const auto kind = text(arguments, operationId, "payloadKind", error, false);
    if (error) {
        return std::nullopt;
    }
    const auto selected = kind.value_or(std::string("opaque"));
    if (selected == "opaque") {
        const auto bytes = text(arguments, operationId, "text", error, false);
        if (error) {
            return std::nullopt;
        }
        const auto source = bytes.value_or(std::string());
        std::vector<std::byte> storage(source.size());
        std::ranges::transform(source, storage.begin(),
                               [](const char item) { return static_cast<std::byte>(item); });
        return document::DataBlockPayload(document::OpaqueExtensionPayload(std::move(storage)));
    }
    const auto numbers =
        list(arguments, operationId, "payload", error, "expects a list of numbers", true);
    if (!numbers.has_value()) {
        return std::nullopt;
    }
    std::vector<double> samples;
    samples.reserve(numbers->size());
    for (const auto& entry : *numbers) {
        const auto sample = numericOf(entry);
        if (!sample.has_value() || !std::isfinite(*sample)) {
            error = failure(operationId, "payload", "expects a list of finite numbers");
            return std::nullopt;
        }
        samples.push_back(*sample);
    }
    if (selected == "curve") {
        if (samples.size() < 2) {
            error = failure(operationId, "payload",
                            "expects (domainStart, domainEnd, sample, ...) for a curve payload");
            return std::nullopt;
        }
        document::DataBlockCurve curve{.domainStart = samples[0],
                                       .domainEnd = samples[1],
                                       .samples =
                                           std::vector<double>(samples.begin() + 2, samples.end())};
        return document::DataBlockPayload(std::move(curve));
    }
    if (selected == "ramp") {
        if (samples.empty() || samples.size() % 5 != 0) {
            error = failure(operationId, "payload",
                            "expects (position, r, g, b, a, ...) groups for a ramp payload");
            return std::nullopt;
        }
        document::DataBlockRamp ramp;
        for (std::size_t index = 0; index < samples.size(); index += 5) {
            ramp.stops.push_back({.position = samples[index],
                                  .color = core::Color4d{samples[index + 1], samples[index + 2],
                                                         samples[index + 3], samples[index + 4]}});
        }
        return document::DataBlockPayload(std::move(ramp));
    }
    error = failure(operationId, "payloadKind", R"(expects "curve", "ramp" or "opaque")");
    return std::nullopt;
}

[[nodiscard]] std::optional<document::AssetLocator>
locator(const Arguments& arguments, const std::string& operationId, OperationCreateResult& error) {
    const auto path = text(arguments, operationId, "path", error);
    const auto kind = text(arguments, operationId, "locatorKind", error, false);
    const auto portability = text(arguments, operationId, "portability", error, false);
    const auto relinkHint = text(arguments, operationId, "relinkHint", error, false);
    if (!path.has_value() || error) {
        return std::nullopt;
    }
    return document::AssetLocator{kind.value_or(std::string("file")),
                                  portability.value_or(std::string("project-relative")), *path,
                                  relinkHint.value_or(std::string())};
}

// An input port is either an ordinary node port or a Merge stack slot; `role` is what tells the
// two apart, and a zero slot is the documented "a new slot here" sentinel.
[[nodiscard]] std::optional<document::InputPortRef> inputPort(const Arguments& arguments,
                                                              const std::string& operationId,
                                                              const std::string& nodeName,
                                                              OperationCreateResult& error) {
    const auto node = id(arguments, operationId, nodeName, error);
    const auto role = text(arguments, operationId, "role", error, false);
    if (!node.has_value() || error) {
        return std::nullopt;
    }
    if (role.has_value()) {
        const auto slot = integer(arguments, operationId, "slot", error, false);
        if (error) {
            return std::nullopt;
        }
        if (slot.value_or(0) < 0) {
            error = failure(operationId, "slot", "expects a stack slot id, or 0 for a new slot");
            return std::nullopt;
        }
        return document::InputPortRef(document::LayerStackInputRef{
            .stackNodeId = document::NodeId::fromRaw(*node),
            .slotId = document::LayerSlotId::fromRaw(static_cast<std::uint64_t>(slot.value_or(0))),
            .role = *role});
    }
    const auto port = text(arguments, operationId, "port", error, false);
    if (!port.has_value()) {
        if (!error) {
            error = failure(operationId, "port", "is required unless a stack role is given");
        }
        return std::nullopt;
    }
    return document::InputPortRef(
        document::NodeInputRef{.nodeId = document::NodeId::fromRaw(*node), .port = *port});
}

// ---------------------------------------------------------------------------------------------
// Schema construction.
// ---------------------------------------------------------------------------------------------

[[nodiscard]] ArgumentSchema need(std::string name, const ValueKind kind, Value example,
                                  std::string summary) {
    return {.name = std::move(name),
            .kind = kind,
            .required = true,
            .inExample = true,
            .context = ContextSource::None,
            .example = std::move(example),
            .summary = std::move(summary)};
}

[[nodiscard]] ArgumentSchema maybe(std::string name, const ValueKind kind, Value example,
                                   std::string summary) {
    return {.name = std::move(name),
            .kind = kind,
            .required = false,
            .inExample = false,
            .context = ContextSource::None,
            .example = std::move(example),
            .summary = std::move(summary)};
}

// Optional, but the operation still needs it unless one of its alternatives is given, so the
// example call names it.
[[nodiscard]] ArgumentSchema alternative(std::string name, const ValueKind kind, Value example,
                                         std::string summary) {
    return {.name = std::move(name),
            .kind = kind,
            .required = false,
            .inExample = true,
            .context = ContextSource::None,
            .example = std::move(example),
            .summary = std::move(summary)};
}

[[nodiscard]] ArgumentSchema fromContext(std::string name, const ValueKind kind,
                                         const ContextSource source, Value example,
                                         std::string summary) {
    return {.name = std::move(name),
            .kind = kind,
            .required = true,
            .inExample = false,
            .context = source,
            .example = std::move(example),
            .summary = std::move(summary)};
}

[[nodiscard]] ArgumentSchema compositionArgument() {
    return fromContext("composition", ValueKind::Id, ContextSource::Composition, StableId{1},
                       "The composition to edit. Defaults to the current composition.");
}

[[nodiscard]] ArgumentSchema layerArgument() {
    return fromContext("layer", ValueKind::Id, ContextSource::Layer, StableId{1},
                       "The layer to edit. Defaults to the first selected layer.");
}

[[nodiscard]] ArgumentSchema nodeSetArgument(std::string name) {
    return fromContext(std::move(name), ValueKind::Array, ContextSource::Selection,
                       ValueArray{Value(StableId{1})},
                       "Stable ids or proxies. Defaults to the current selection.");
}

[[nodiscard]] ArgumentSchema timeArgument() {
    return fromContext("time", ValueKind::Time, ContextSource::Time, std::int64_t{0},
                       "An exact time. Defaults to the current playhead time.");
}

[[nodiscard]] ArgumentSchema interpolationArgument() {
    return maybe("interpolation", ValueKind::String, "linear",
                 R"(Outgoing ease: "hold", "linear" or "ease-in-out". Defaults to "linear".)");
}

[[nodiscard]] ArgumentSchema componentArgument() {
    return maybe("component", ValueKind::Integer, std::int64_t{0},
                 "Component index: X=0, Y=1, Z=2, Red=3, Green=4, Blue=5, Alpha=6. Omit for a "
                 "whole-value address.");
}

void addDescriptor(std::vector<OperationDescriptor>& descriptors, std::string id,
                   std::vector<ArgumentSchema> arguments, OperationFactory factory) {
    descriptors.push_back({.schema = {.typeId = std::move(id), .arguments = std::move(arguments)},
                           .factory = std::move(factory)});
}

// One rejection, said the same way everywhere: the operation, the argument, what it expects, and a
// call the artist can paste. The factories only supply the predicate.
[[nodiscard]] OperationCreateResult decorate(const OperationSchema& schema,
                                             OperationCreateResult result) {
    if (result || result.diagnostic() == nullptr) {
        return result;
    }
    auto diagnostic = *result.diagnostic();
    std::string message = schema.typeId;
    message += diagnostic.argument.empty() ? ": " : ": argument '" + diagnostic.argument + "' ";
    message += diagnostic.message;
    const auto argument =
        std::ranges::find(schema.arguments, diagnostic.argument, &ArgumentSchema::name);
    if (argument != schema.arguments.end() && argument->contextual()) {
        message += " and bloom.context had no current value for it";
    }
    message += ". Example: " + exampleCall(schema);
    diagnostic.message = std::move(message);
    return OperationCreateResult(nullptr, std::move(diagnostic));
}

} // namespace

std::string_view valueKindName(const ValueKind kind) noexcept {
    constexpr std::array<std::string_view, 11> names{
        "bool", "int", "float", "str", "vec2", "vec3", "color4", "id", "array", "time", "value"};
    const auto index = static_cast<std::size_t>(kind);
    return index < names.size() ? names[index] : "value";
}

std::string pythonOperationName(const std::string_view typeId) {
    const auto first = typeId.find('.');
    if (first == std::string_view::npos) {
        return std::string(typeId);
    }
    const auto second = typeId.find('.', first + 1);
    if (second == std::string_view::npos) {
        return std::string(typeId);
    }
    std::string domain(typeId.substr(first + 1, second - first - 1));
    std::string verb(typeId.substr(second + 1));
    std::ranges::replace(domain, '-', '_');
    std::ranges::replace(verb, '-', '_');
    // Python keywords take the trailing underscore the client binds them under.
    constexpr std::array<std::string_view, 5> keywords{"import", "del", "class", "lambda", "not"};
    if (std::ranges::find(keywords, verb) != keywords.end()) {
        verb.push_back('_');
    }
    return "bloom.ops." + domain + "." + verb;
}

std::string pythonLiteral(const Value& value) {
    return std::visit(
        [](const auto& held) -> std::string {
            using Held = std::decay_t<decltype(held)>;
            if constexpr (std::is_same_v<Held, bool>) {
                return held ? "True" : "False";
            } else if constexpr (std::is_same_v<Held, std::int64_t>) {
                return std::to_string(held);
            } else if constexpr (std::is_same_v<Held, double>) {
                // A whole number reads as one: `(1, 0, 0, 1)`, the way an artist types a colour.
                if (std::isfinite(held) && std::floor(held) == held && std::fabs(held) < 1e15) {
                    return std::to_string(static_cast<std::int64_t>(held));
                }
                auto text = std::to_string(held);
                while (text.size() > 1 && text.back() == '0' && text[text.size() - 2] != '.') {
                    text.pop_back();
                }
                return text;
            } else if constexpr (std::is_same_v<Held, std::string>) {
                return "\"" + held + "\"";
            } else if constexpr (std::is_same_v<Held, StableId>) {
                return std::to_string(held.value);
            } else if constexpr (std::is_same_v<Held, document::Vec2d>) {
                return "(" + pythonLiteral(Value(held.x)) + ", " + pythonLiteral(Value(held.y)) +
                       ")";
            } else if constexpr (std::is_same_v<Held, document::Vec3d>) {
                return "(" + pythonLiteral(Value(held.x)) + ", " + pythonLiteral(Value(held.y)) +
                       ", " + pythonLiteral(Value(held.z)) + ")";
            } else if constexpr (std::is_same_v<Held, core::Color4d>) {
                return "(" + pythonLiteral(Value(held.red)) + ", " +
                       pythonLiteral(Value(held.green)) + ", " + pythonLiteral(Value(held.blue)) +
                       ", " + pythonLiteral(Value(held.alpha)) + ")";
            } else {
                std::string text = "(";
                for (std::size_t index = 0; index < held.size(); ++index) {
                    text += (index == 0 ? std::string() : std::string(", ")) +
                            pythonLiteral(held[index]);
                }
                return text + (held.size() == 1 ? ",)" : ")");
            }
        },
        value.storage);
}

std::string exampleCall(const OperationSchema& schema) {
    std::string call = pythonOperationName(schema.typeId) + "(";
    bool first = true;
    for (const auto& argument : schema.arguments) {
        if (!argument.inExample || argument.contextual() || !argument.example.has_value()) {
            continue;
        }
        call += (first ? std::string() : std::string(", ")) + argument.name + "=" +
                pythonLiteral(*argument.example);
        first = false;
    }
    return call + ")";
}

std::vector<std::string> applyContextDefaults(const OperationSchema& schema, Arguments& arguments,
                                              const OperationContext& context) {
    std::vector<std::string> filled;
    for (const auto& argument : schema.arguments) {
        if (!argument.contextual() || arguments.contains(argument.name)) {
            continue;
        }
        switch (argument.context) {
        case ContextSource::Project:
            if (context.project.has_value()) {
                arguments.emplace(argument.name, Value(StableId{*context.project}));
                filled.push_back(argument.name);
            }
            break;
        case ContextSource::Composition:
            if (context.composition.has_value()) {
                arguments.emplace(argument.name, Value(StableId{*context.composition}));
                filled.push_back(argument.name);
            }
            break;
        case ContextSource::Time:
            if (context.time.has_value()) {
                arguments.emplace(argument.name,
                                  Value(ValueArray{Value(context.time->numerator()),
                                                   Value(context.time->denominator())}));
                filled.push_back(argument.name);
            }
            break;
        case ContextSource::Layer:
            if (!context.layers.empty()) {
                arguments.emplace(argument.name, Value(StableId{context.layers.front()}));
                filled.push_back(argument.name);
            }
            break;
        case ContextSource::Selection:
            if (!context.selection.empty()) {
                ValueArray entries;
                entries.reserve(context.selection.size());
                for (const auto identity : context.selection) {
                    entries.emplace_back(StableId{identity});
                }
                arguments.emplace(argument.name, Value(std::move(entries)));
                filled.push_back(argument.name);
            }
            break;
        case ContextSource::None:
            break;
        }
    }
    return filled;
}

OperationRegistry OperationRegistry::builtIn() {
    OperationRegistry registry;
    auto& all = registry.descriptors_;
    const auto add = [&all](std::string id, std::vector<ArgumentSchema> arguments,
                            OperationFactory factory) {
        addDescriptor(all, std::move(id), std::move(arguments), std::move(factory));
    };

    // --- animation ----------------------------------------------------------------------------
    add("bloom.animation.convert-to-constant",
        {compositionArgument(),
         need("parameter", ValueKind::Id, StableId{1}, "The animated parameter to freeze."),
         need("value", ValueKind::Value, 0.0, "The constant value to keep.")},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto composition = id(args, op, "composition", error);
            const auto parameter = id(args, op, "parameter", error);
            const auto value = authored(args, op, "value", error);
            if (!composition || !parameter || !value) {
                return error;
            }
            return std::visit(
                [&](const auto& held) {
                    return OperationCreateResult(
                        std::make_unique<commands::ConvertAnimationToConstant>(
                            document::CompositionId::fromRaw(*composition),
                            document::ParameterId::fromRaw(*parameter), held),
                        std::nullopt);
                },
                *value);
        });

    add("bloom.animation.create-for-parameter",
        {compositionArgument(),
         need("parameter", ValueKind::Id, StableId{1}, "The parameter to make animated."),
         timeArgument(), componentArgument()},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto composition = id(args, op, "composition", error);
            const auto parameter = id(args, op, "parameter", error);
            const auto time = rational(args, op, "time", error);
            std::optional<document::AnimationComponent> component;
            if (const auto* value = findArgument(args, "component");
                value != nullptr && !componentOf(*value, component)) {
                return failure(op, "component", "expects a component index in 0..6");
            }
            if (!composition || !parameter || !time) {
                return error;
            }
            return OperationCreateResult(std::make_unique<commands::CreateAnimationForParameter>(
                                             document::CompositionId::fromRaw(*composition),
                                             document::ParameterId::fromRaw(*parameter), *time,
                                             component),
                                         std::nullopt);
        });

    add("bloom.animation.delete-keyframe",
        {compositionArgument(),
         need("curve", ValueKind::Id, StableId{1}, "The animation curve that owns the key."),
         need("keyframe", ValueKind::Id, StableId{1}, "The key to delete."), componentArgument()},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto composition = id(args, op, "composition", error);
            const auto curve = id(args, op, "curve", error);
            const auto keyframe = id(args, op, "keyframe", error);
            std::optional<document::AnimationComponent> component;
            if (const auto* value = findArgument(args, "component");
                value != nullptr && !componentOf(*value, component)) {
                return failure(op, "component", "expects a component index in 0..6");
            }
            if (!composition || !curve || !keyframe) {
                return error;
            }
            return OperationCreateResult(std::make_unique<commands::DeleteKeyframe>(
                                             document::CompositionId::fromRaw(*composition),
                                             document::AnimationCurveId::fromRaw(*curve),
                                             document::KeyframeId::fromRaw(*keyframe), component),
                                         std::nullopt);
        });

    add("bloom.animation.delete-keyframes",
        {compositionArgument(),
         need("keys", ValueKind::Array,
              ValueArray{Value(ValueArray{Value(StableId{1}), Value(StableId{1})})},
              "Key addresses as (curve, keyframe) or (curve, keyframe, component).")},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto composition = id(args, op, "composition", error);
            const auto keys =
                read(args, op, "keys", error, true,
                     "expects (curve, keyframe) or (curve, keyframe, component) entries",
                     keyframeAddressesOf);
            if (!composition || !keys) {
                return error;
            }
            return OperationCreateResult(std::make_unique<commands::DeleteKeyframes>(
                                             document::CompositionId::fromRaw(*composition), *keys),
                                         std::nullopt);
        });

    add("bloom.animation.insert-scalar-keyframe",
        {compositionArgument(),
         need("curve", ValueKind::Id, StableId{1}, "The scalar curve to key."), timeArgument(),
         need("value", ValueKind::Double, 0.0, "The scalar value at that time."),
         interpolationArgument()},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto composition = id(args, op, "composition", error);
            const auto curve = id(args, op, "curve", error);
            const auto time = rational(args, op, "time", error);
            const auto value = number(args, op, "value", error);
            const auto mode = interpolation(args, op, "interpolation", error, false);
            if (!composition || !curve || !time || !value || error) {
                return error;
            }
            return OperationCreateResult(
                std::make_unique<commands::InsertScalarKeyframe>(
                    document::CompositionId::fromRaw(*composition),
                    document::AnimationCurveId::fromRaw(*curve), *time, *value,
                    mode.value_or(document::KeyframeInterpolation::Linear)),
                std::nullopt);
        });

    add("bloom.animation.insert-vec2-keyframe",
        {compositionArgument(), need("curve", ValueKind::Id, StableId{1}, "The vec2 curve to key."),
         timeArgument(),
         need("value", ValueKind::Vec2, document::Vec2d{0.0, 0.0}, "The value at that time."),
         interpolationArgument()},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto composition = id(args, op, "composition", error);
            const auto curve = id(args, op, "curve", error);
            const auto time = rational(args, op, "time", error);
            const auto value = vec2(args, op, "value", error);
            const auto mode = interpolation(args, op, "interpolation", error, false);
            if (!composition || !curve || !time || !value || error) {
                return error;
            }
            return OperationCreateResult(
                std::make_unique<commands::InsertVec2Keyframe>(
                    document::CompositionId::fromRaw(*composition),
                    document::AnimationCurveId::fromRaw(*curve), *time, *value,
                    mode.value_or(document::KeyframeInterpolation::Linear)),
                std::nullopt);
        });

    add("bloom.animation.insert-vec3-keyframe",
        {compositionArgument(), need("curve", ValueKind::Id, StableId{1}, "The vec3 curve to key."),
         timeArgument(),
         need("value", ValueKind::Vec3, document::Vec3d{0.0, 0.0, 0.0}, "The value at that time."),
         interpolationArgument()},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto composition = id(args, op, "composition", error);
            const auto curve = id(args, op, "curve", error);
            const auto time = rational(args, op, "time", error);
            const auto value = vec3(args, op, "value", error);
            const auto mode = interpolation(args, op, "interpolation", error, false);
            if (!composition || !curve || !time || !value || error) {
                return error;
            }
            return OperationCreateResult(
                std::make_unique<commands::InsertVec3Keyframe>(
                    document::CompositionId::fromRaw(*composition),
                    document::AnimationCurveId::fromRaw(*curve), *time, *value,
                    mode.value_or(document::KeyframeInterpolation::Linear)),
                std::nullopt);
        });

    add("bloom.animation.insert-color4-keyframe",
        {compositionArgument(),
         need("curve", ValueKind::Id, StableId{1}, "The colour curve to key."), timeArgument(),
         need("value", ValueKind::Color4, core::Color4d{1.0, 0.0, 0.0, 1.0},
              "The colour at that time."),
         interpolationArgument()},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto composition = id(args, op, "composition", error);
            const auto curve = id(args, op, "curve", error);
            const auto time = rational(args, op, "time", error);
            const auto value = color4(args, op, "value", error);
            const auto mode = interpolation(args, op, "interpolation", error, false);
            if (!composition || !curve || !time || !value || error) {
                return error;
            }
            return OperationCreateResult(
                std::make_unique<commands::InsertColor4Keyframe>(
                    document::CompositionId::fromRaw(*composition),
                    document::AnimationCurveId::fromRaw(*curve), *time, *value,
                    mode.value_or(document::KeyframeInterpolation::Linear)),
                std::nullopt);
        });

    add("bloom.animation.move-keyframes",
        {compositionArgument(),
         need("keys", ValueKind::Array,
              ValueArray{Value(
                  ValueArray{Value(StableId{1}), Value(StableId{1}), Value(std::int64_t{12})})},
              "Moves as (curve, keyframe, time) or (curve, keyframe, time, component).")},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto composition = id(args, op, "composition", error);
            const auto keys = read(
                args, op, "keys", error, true,
                "expects (curve, keyframe, time) or (curve, keyframe, time, component) entries",
                keyframeMovesOf);
            if (!composition || !keys) {
                return error;
            }
            return OperationCreateResult(std::make_unique<commands::MoveKeyframes>(
                                             document::CompositionId::fromRaw(*composition), *keys),
                                         std::nullopt);
        });

    add("bloom.animation.paste-keyframes",
        {compositionArgument(),
         need(
             "keys", ValueKind::Array,
             ValueArray{Value(ValueArray{Value(StableId{1}), Value(std::int64_t{12}), Value(1.0)})},
             "Pastes as (parameter, time, value), plus optional component and interpolation.")},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto composition = id(args, op, "composition", error);
            const auto keys =
                read(args, op, "keys", error, true,
                     "expects (parameter, time, value) entries, each with an optional component "
                     "and interpolation",
                     keyframePastesOf);
            if (!composition || !keys) {
                return error;
            }
            return OperationCreateResult(std::make_unique<commands::PasteKeyframes>(
                                             document::CompositionId::fromRaw(*composition), *keys),
                                         std::nullopt);
        });

    add("bloom.animation.set-keyframe-at-time",
        {compositionArgument(), need("curve", ValueKind::Id, StableId{1}, "The curve to key."),
         timeArgument(), need("value", ValueKind::Value, 0.0, "The value at that time.")},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto composition = id(args, op, "composition", error);
            const auto curve = id(args, op, "curve", error);
            const auto time = rational(args, op, "time", error);
            const auto value = authored(args, op, "value", error);
            if (!composition || !curve || !time || !value) {
                return error;
            }
            return std::visit(
                [&](const auto& held) {
                    return OperationCreateResult(std::make_unique<commands::SetKeyframeAtTime>(
                                                     document::CompositionId::fromRaw(*composition),
                                                     document::AnimationCurveId::fromRaw(*curve),
                                                     *time, held),
                                                 std::nullopt);
                },
                *value);
        });

    add("bloom.animation.set-keyframe-at-time-for-parameter",
        {compositionArgument(),
         need("parameter", ValueKind::Id, StableId{1}, "The parameter to key."), timeArgument(),
         need("value", ValueKind::Value, 0.0, "The value at that time.")},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto composition = id(args, op, "composition", error);
            const auto parameter = id(args, op, "parameter", error);
            const auto time = rational(args, op, "time", error);
            const auto value = authored(args, op, "value", error);
            if (!composition || !parameter || !time || !value) {
                return error;
            }
            return std::visit(
                [&](const auto& held) {
                    return OperationCreateResult(
                        std::make_unique<commands::SetKeyframeAtTimeForParameter>(
                            document::CompositionId::fromRaw(*composition),
                            document::ParameterId::fromRaw(*parameter), *time, held),
                        std::nullopt);
                },
                *value);
        });

    add("bloom.animation.set-keyframe-at-time-for-parameter-component",
        {compositionArgument(),
         need("parameter", ValueKind::Id, StableId{1}, "The parameter to key."),
         need("component", ValueKind::Integer, std::int64_t{0},
              "Component index: X=0, Y=1, Z=2, Red=3, Green=4, Blue=5, Alpha=6."),
         timeArgument(), need("value", ValueKind::Double, 400.0, "The scalar value at that time.")},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto composition = id(args, op, "composition", error);
            const auto parameter = id(args, op, "parameter", error);
            const auto component = integer(args, op, "component", error);
            const auto time = rational(args, op, "time", error);
            const auto value = number(args, op, "value", error);
            if (!composition || !parameter || !component || !time || !value) {
                return error;
            }
            if (*component < 0 || *component > 6) {
                return failure(op, "component", "expects a component index in 0..6");
            }
            return OperationCreateResult(
                std::make_unique<commands::SetKeyframeAtTimeForParameterComponent>(
                    document::CompositionId::fromRaw(*composition),
                    document::ParameterId::fromRaw(*parameter),
                    static_cast<document::AnimationComponent>(*component), *time, *value),
                std::nullopt);
        });

    add("bloom.animation.set-keyframe-handles",
        {compositionArgument(),
         alternative("outgoing", ValueKind::Array,
                     ValueArray{Value(ValueArray{Value(StableId{1}), Value(StableId{1}),
                                                 Value(0.25), Value(0.0)})},
                     "Outgoing handles as (curve, keyframe, time, value) with an optional "
                     "component."),
         maybe("incoming", ValueKind::Array,
               ValueArray{Value(
                   ValueArray{Value(StableId{1}), Value(StableId{1}), Value(0.25), Value(0.0)})},
               "Incoming handles as (curve, keyframe, time, value) with an optional component.")},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto composition = id(args, op, "composition", error);
            if (!composition) {
                return error;
            }
            std::vector<commands::KeyframeHandleEdit> edits;
            for (const auto* side : {"outgoing", "incoming"}) {
                const auto* value = findArgument(args, side);
                if (value == nullptr) {
                    continue;
                }
                const auto* array = std::get_if<ValueArray>(&value->storage);
                if (array == nullptr ||
                    !mergeHandles(*array, std::string_view(side) == "outgoing", edits)) {
                    return failure(op, side,
                                   "expects (curve, keyframe, time, value) entries, each with an "
                                   "optional component");
                }
            }
            if (edits.empty()) {
                return failure(op, "outgoing", "is required unless incoming handles are given");
            }
            return OperationCreateResult(
                std::make_unique<commands::SetKeyframeHandles>(
                    document::CompositionId::fromRaw(*composition), std::move(edits)),
                std::nullopt);
        });

    add("bloom.animation.set-keyframe-interpolation",
        {compositionArgument(), need("curve", ValueKind::Id, StableId{1}, "The curve to edit."),
         need("keyframe", ValueKind::Id, StableId{1}, "The key whose outgoing ease changes."),
         need("interpolation", ValueKind::String, "ease-in-out",
              R"("hold", "linear" or "ease-in-out".)"),
         componentArgument()},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto composition = id(args, op, "composition", error);
            const auto curve = id(args, op, "curve", error);
            const auto keyframe = id(args, op, "keyframe", error);
            const auto mode = interpolation(args, op, "interpolation", error);
            std::optional<document::AnimationComponent> component;
            if (const auto* value = findArgument(args, "component");
                value != nullptr && !componentOf(*value, component)) {
                return failure(op, "component", "expects a component index in 0..6");
            }
            if (!composition || !curve || !keyframe || !mode) {
                return error;
            }
            return OperationCreateResult(std::make_unique<commands::SetKeyframeInterpolation>(
                                             document::CompositionId::fromRaw(*composition),
                                             document::AnimationCurveId::fromRaw(*curve),
                                             document::KeyframeId::fromRaw(*keyframe), *mode,
                                             component),
                                         std::nullopt);
        });

    add("bloom.animation.set-keyframe-values",
        {compositionArgument(),
         need("values", ValueKind::Array,
              ValueArray{Value(ValueArray{Value(StableId{1}), Value(StableId{1}), Value(1.0)})},
              "Edits as (curve, keyframe, value) or (curve, keyframe, value, component).")},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto composition = id(args, op, "composition", error);
            const auto values =
                read(args, op, "values", error, true,
                     "expects (curve, keyframe, value) entries, each with an optional component",
                     keyframeValuesOf);
            if (!composition || !values) {
                return error;
            }
            return OperationCreateResult(
                std::make_unique<commands::SetKeyframeValues>(
                    document::CompositionId::fromRaw(*composition), *values),
                std::nullopt);
        });

    add("bloom.animation.set-keyframes-interpolation",
        {compositionArgument(),
         need("keys", ValueKind::Array,
              ValueArray{Value(ValueArray{Value(StableId{1}), Value(StableId{1})})},
              "Key addresses as (curve, keyframe) or (curve, keyframe, component)."),
         need("interpolation", ValueKind::String, "ease-in-out",
              R"("hold", "linear" or "ease-in-out".)")},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto composition = id(args, op, "composition", error);
            const auto keys =
                read(args, op, "keys", error, true,
                     "expects (curve, keyframe) entries, each with an optional component",
                     keyframeAddressesOf);
            const auto mode = interpolation(args, op, "interpolation", error);
            if (!composition || !keys || !mode) {
                return error;
            }
            return OperationCreateResult(
                std::make_unique<commands::SetKeyframesInterpolation>(
                    document::CompositionId::fromRaw(*composition), *keys, *mode),
                std::nullopt);
        });

    add("bloom.animation.update-scalar-keyframe",
        {compositionArgument(), need("curve", ValueKind::Id, StableId{1}, "The curve to edit."),
         need("keyframe", ValueKind::Id, StableId{1}, "The key to re-place."), timeArgument(),
         need("value", ValueKind::Double, 0.0, "The new value."), interpolationArgument()},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto composition = id(args, op, "composition", error);
            const auto curve = id(args, op, "curve", error);
            const auto keyframe = id(args, op, "keyframe", error);
            const auto time = rational(args, op, "time", error);
            const auto value = number(args, op, "value", error);
            const auto mode = interpolation(args, op, "interpolation", error, false);
            if (!composition || !curve || !keyframe || !time || !value || error) {
                return error;
            }
            return OperationCreateResult(
                std::make_unique<commands::UpdateScalarKeyframe>(
                    document::CompositionId::fromRaw(*composition),
                    document::AnimationCurveId::fromRaw(*curve),
                    document::KeyframeId::fromRaw(*keyframe), *time, *value,
                    mode.value_or(document::KeyframeInterpolation::Linear)),
                std::nullopt);
        });

    add("bloom.animation.update-vec2-keyframe",
        {compositionArgument(), need("curve", ValueKind::Id, StableId{1}, "The curve to edit."),
         need("keyframe", ValueKind::Id, StableId{1}, "The key to re-place."), timeArgument(),
         need("value", ValueKind::Vec2, document::Vec2d{0.0, 0.0}, "The new value."),
         interpolationArgument()},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto composition = id(args, op, "composition", error);
            const auto curve = id(args, op, "curve", error);
            const auto keyframe = id(args, op, "keyframe", error);
            const auto time = rational(args, op, "time", error);
            const auto value = vec2(args, op, "value", error);
            const auto mode = interpolation(args, op, "interpolation", error, false);
            if (!composition || !curve || !keyframe || !time || !value || error) {
                return error;
            }
            return OperationCreateResult(
                std::make_unique<commands::UpdateVec2Keyframe>(
                    document::CompositionId::fromRaw(*composition),
                    document::AnimationCurveId::fromRaw(*curve),
                    document::KeyframeId::fromRaw(*keyframe), *time, *value,
                    mode.value_or(document::KeyframeInterpolation::Linear)),
                std::nullopt);
        });

    add("bloom.animation.update-vec3-keyframe",
        {compositionArgument(), need("curve", ValueKind::Id, StableId{1}, "The curve to edit."),
         need("keyframe", ValueKind::Id, StableId{1}, "The key to re-place."), timeArgument(),
         need("value", ValueKind::Vec3, document::Vec3d{0.0, 0.0, 0.0}, "The new value."),
         interpolationArgument()},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto composition = id(args, op, "composition", error);
            const auto curve = id(args, op, "curve", error);
            const auto keyframe = id(args, op, "keyframe", error);
            const auto time = rational(args, op, "time", error);
            const auto value = vec3(args, op, "value", error);
            const auto mode = interpolation(args, op, "interpolation", error, false);
            if (!composition || !curve || !keyframe || !time || !value || error) {
                return error;
            }
            return OperationCreateResult(
                std::make_unique<commands::UpdateVec3Keyframe>(
                    document::CompositionId::fromRaw(*composition),
                    document::AnimationCurveId::fromRaw(*curve),
                    document::KeyframeId::fromRaw(*keyframe), *time, *value,
                    mode.value_or(document::KeyframeInterpolation::Linear)),
                std::nullopt);
        });

    add("bloom.animation.update-color4-keyframe",
        {compositionArgument(), need("curve", ValueKind::Id, StableId{1}, "The curve to edit."),
         need("keyframe", ValueKind::Id, StableId{1}, "The key to re-place."), timeArgument(),
         need("value", ValueKind::Color4, core::Color4d{1.0, 0.0, 0.0, 1.0}, "The new colour."),
         interpolationArgument()},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto composition = id(args, op, "composition", error);
            const auto curve = id(args, op, "curve", error);
            const auto keyframe = id(args, op, "keyframe", error);
            const auto time = rational(args, op, "time", error);
            const auto value = color4(args, op, "value", error);
            const auto mode = interpolation(args, op, "interpolation", error, false);
            if (!composition || !curve || !keyframe || !time || !value || error) {
                return error;
            }
            return OperationCreateResult(
                std::make_unique<commands::UpdateColor4Keyframe>(
                    document::CompositionId::fromRaw(*composition),
                    document::AnimationCurveId::fromRaw(*curve),
                    document::KeyframeId::fromRaw(*keyframe), *time, *value,
                    mode.value_or(document::KeyframeInterpolation::Linear)),
                std::nullopt);
        });

    // --- assets -------------------------------------------------------------------------------
    add("bloom.asset.create-folder",
        {need("name", ValueKind::String, "Footage", "The folder name."),
         maybe("parent", ValueKind::Id, StableId{1},
               "The containing folder. Omit for the project root.")},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto name = text(args, op, "name", error);
            const auto parent = id(args, op, "parent", error, false);
            if (!name || error) {
                return error;
            }
            return OperationCreateResult(
                std::make_unique<commands::CreateAssetFolder>(
                    *name, parent.has_value()
                               ? std::optional(document::AssetFolderId::fromRaw(*parent))
                               : std::nullopt),
                std::nullopt);
        });

    const auto fontArguments = [](std::vector<ArgumentSchema> leading) {
        leading.push_back(need("path", ValueKind::String, "/usr/share/fonts/DejaVuSans.ttf",
                               "The face's file path, or an embedded locator path."));
        leading.push_back(need("digest", ValueKind::String,
                               "0000000000000000000000000000000000000000000000000000000000000000",
                               "The face's SHA-256 content digest, 64 lowercase hex characters."));
        leading.push_back(maybe("family", ValueKind::String, "DejaVu Sans", "The family name."));
        leading.push_back(maybe("style", ValueKind::String, "Book", "The style name."));
        leading.push_back(maybe("index", ValueKind::Integer, std::int64_t{0},
                                "The face index inside a collection file."));
        leading.push_back(
            maybe("portability", ValueKind::String, "system",
                  R"(Locator portability: "system", "builtin" or "project-relative".)"));
        leading.push_back(maybe("relinkHint", ValueKind::String, "", "A relink hint to remember."));
        leading.push_back(maybe("name", ValueKind::String, "DejaVu Sans", "The asset name."));
        leading.push_back(
            maybe("tags", ValueKind::Array, ValueArray{Value("title")}, "Asset tags."));
        return leading;
    };

    add("bloom.asset.ensure-font", fontArguments({}),
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            auto record = fontRecord(args, op, error);
            if (!record) {
                return error;
            }
            return OperationCreateResult(
                std::make_unique<commands::EnsureFontAsset>(*std::move(record)), std::nullopt);
        });

    add("bloom.asset.relink-font",
        fontArguments({need("asset", ValueKind::Id, StableId{1}, "The font asset to repoint.")}),
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto asset = id(args, op, "asset", error);
            auto record = fontRecord(args, op, error);
            if (!asset || !record) {
                return error;
            }
            return OperationCreateResult(
                std::make_unique<commands::RelinkFontAsset>(document::AssetId::fromRaw(*asset),
                                                            *std::move(record)),
                std::nullopt);
        });

    add("bloom.asset.import",
        {need("paths", ValueKind::Array, ValueArray{Value("footage/plate.exr")},
              "Absolute or project-relative media paths to import."),
         need("projectDirectory", ValueKind::String, ".",
              "The project directory the locators are made relative to.")},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto paths = strings(args, op, "paths", error);
            const auto directory = text(args, op, "projectDirectory", error);
            if (!paths || !directory) {
                return error;
            }
            if (paths->empty()) {
                return failure(op, "paths", "expects at least one media path");
            }
            std::vector<std::filesystem::path> files;
            files.reserve(paths->size());
            for (const auto& entry : *paths) {
                files.emplace_back(entry);
            }
            return OperationCreateResult(
                std::make_unique<commands::ImportAssets>(files, std::filesystem::path(*directory)),
                std::nullopt);
        });

    add("bloom.asset.relink",
        {need("asset", ValueKind::Id, StableId{1}, "The asset to repoint."),
         need("path", ValueKind::String, "footage/plate.exr", "The replacement media path."),
         need("projectDirectory", ValueKind::String, ".",
              "The project directory the locator is made relative to.")},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto asset = id(args, op, "asset", error);
            const auto path = text(args, op, "path", error);
            const auto directory = text(args, op, "projectDirectory", error);
            if (!asset || !path || !directory) {
                return error;
            }
            return OperationCreateResult(
                std::make_unique<commands::RelinkAsset>(document::AssetId::fromRaw(*asset),
                                                        std::filesystem::path(*path),
                                                        std::filesystem::path(*directory)),
                std::nullopt);
        });

    add("bloom.asset.move",
        {need("assets", ValueKind::Array, ValueArray{Value(StableId{1})}, "The assets to move."),
         maybe("folder", ValueKind::Id, StableId{1},
               "The destination folder. Omit for the project root."),
         maybe("index", ValueKind::Integer, std::int64_t{0},
               "Destination position after the moved assets are removed. Defaults to 0.")},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto assets = identities(args, op, "assets", error);
            const auto folder = id(args, op, "folder", error, false);
            const auto index = integer(args, op, "index", error, false);
            if (!assets || error) {
                return error;
            }
            if (index.value_or(0) < 0) {
                return failure(op, "index", "expects a position of 0 or more");
            }
            std::vector<document::AssetId> ids;
            ids.reserve(assets->size());
            for (const auto identity : *assets) {
                ids.push_back(document::AssetId::fromRaw(identity));
            }
            return OperationCreateResult(
                std::make_unique<commands::MoveAssets>(
                    std::move(ids),
                    folder.has_value() ? std::optional(document::AssetFolderId::fromRaw(*folder))
                                       : std::nullopt,
                    static_cast<std::size_t>(index.value_or(0))),
                std::nullopt);
        });

    add("bloom.asset.remove", {need("asset", ValueKind::Id, StableId{1}, "The asset to remove.")},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto asset = id(args, op, "asset", error);
            if (!asset) {
                return error;
            }
            return OperationCreateResult(
                std::make_unique<commands::RemoveAsset>(document::AssetId::fromRaw(*asset)),
                std::nullopt);
        });

    add("bloom.asset.remove-folder",
        {need("folder", ValueKind::Id, StableId{1}, "The folder to remove.")},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto folder = id(args, op, "folder", error);
            if (!folder) {
                return error;
            }
            return OperationCreateResult(std::make_unique<commands::RemoveAssetFolder>(
                                             document::AssetFolderId::fromRaw(*folder)),
                                         std::nullopt);
        });

    add("bloom.asset.rename",
        {need("asset", ValueKind::Id, StableId{1}, "The asset to rename."),
         need("name", ValueKind::String, "Plate", "The new name.")},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto asset = id(args, op, "asset", error);
            const auto name = text(args, op, "name", error);
            if (!asset || !name) {
                return error;
            }
            return OperationCreateResult(
                std::make_unique<commands::RenameAsset>(document::AssetId::fromRaw(*asset), *name),
                std::nullopt);
        });

    add("bloom.asset.rename-folder",
        {need("folder", ValueKind::Id, StableId{1}, "The folder to rename."),
         need("name", ValueKind::String, "Footage", "The new name.")},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto folder = id(args, op, "folder", error);
            const auto name = text(args, op, "name", error);
            if (!folder || !name) {
                return error;
            }
            return OperationCreateResult(std::make_unique<commands::RenameAssetFolder>(
                                             document::AssetFolderId::fromRaw(*folder), *name),
                                         std::nullopt);
        });

    add("bloom.asset.reorder",
        {need("order", ValueKind::Array, ValueArray{Value(StableId{1})},
              "The complete asset order for the folder."),
         maybe("folder", ValueKind::Id, StableId{1},
               "The folder being ordered. Omit for the project root.")},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto order = identities(args, op, "order", error);
            const auto folder = id(args, op, "folder", error, false);
            if (!order || error) {
                return error;
            }
            std::vector<document::AssetId> ids;
            ids.reserve(order->size());
            for (const auto identity : *order) {
                ids.push_back(document::AssetId::fromRaw(identity));
            }
            return OperationCreateResult(
                std::make_unique<commands::ReorderAssets>(
                    folder.has_value() ? std::optional(document::AssetFolderId::fromRaw(*folder))
                                       : std::nullopt,
                    std::move(ids)),
                std::nullopt);
        });

    add("bloom.asset.set-tags",
        {need("assets", ValueKind::Array, ValueArray{Value(StableId{1})}, "The assets to tag."),
         need("tags", ValueKind::Array, ValueArray{Value("plate")}, "The complete tag list.")},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto assets = identities(args, op, "assets", error);
            const auto tags = strings(args, op, "tags", error);
            if (!assets || !tags) {
                return error;
            }
            std::vector<document::AssetId> ids;
            ids.reserve(assets->size());
            for (const auto identity : *assets) {
                ids.push_back(document::AssetId::fromRaw(identity));
            }
            return OperationCreateResult(
                std::make_unique<commands::SetAssetTags>(std::move(ids), *tags), std::nullopt);
        });

    // --- data blocks --------------------------------------------------------------------------
    add("bloom.data-block.create",
        {need("typeId", ValueKind::String, "bloom.curve", "The block's authored type id."),
         need("mediaType", ValueKind::String, "application/vnd.bloom.curve",
              "The payload media type."),
         maybe("kind", ValueKind::String, "curve",
               R"(Block kind name, for example "curve", "ramp" or "opaque".)"),
         maybe("payloadKind", ValueKind::String, "curve",
               R"("curve", "ramp" or "opaque". Defaults to "opaque".)"),
         maybe("payload", ValueKind::Array, ValueArray{Value(0.0), Value(1.0), Value(0.5)},
               "A curve payload as (domainStart, domainEnd, sample, ...), or a ramp payload as "
               "(position, r, g, b, a, ...)."),
         maybe("text", ValueKind::String, "", "Opaque payload bytes, as UTF-8 text."),
         maybe("createdAt", ValueKind::String, "1970-01-01T00:00:00Z", "The provenance timestamp."),
         maybe("schemaMajor", ValueKind::Integer, std::int64_t{1}, "Schema major version."),
         maybe("schemaMinor", ValueKind::Integer, std::int64_t{0}, "Schema minor version."),
         maybe("tags", ValueKind::Array, ValueArray{Value("authored")}, "Block tags.")},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto typeId = text(args, op, "typeId", error);
            const auto mediaType = text(args, op, "mediaType", error);
            auto content = payload(args, op, error);
            const auto major = integer(args, op, "schemaMajor", error, false);
            const auto minor = integer(args, op, "schemaMinor", error, false);
            const auto createdAt = text(args, op, "createdAt", error, false);
            const auto tags = strings(args, op, "tags", error, false);
            if (!typeId || !mediaType || !content || error) {
                return error;
            }
            auto kind = document::DataBlockKind::Opaque;
            if (const auto* value = findArgument(args, "kind")) {
                const auto named = dataBlockKindOf(*value);
                if (!named.has_value()) {
                    return failure(op, "kind", "expects a data-block kind name");
                }
                kind = *named;
            }
            if (major.value_or(1) < 0 || minor.value_or(0) < 0 ||
                major.value_or(1) > std::numeric_limits<std::uint32_t>::max() ||
                minor.value_or(0) > std::numeric_limits<std::uint32_t>::max()) {
                return failure(op, "schemaMajor", "expects a version in 0..4294967295");
            }
            document::DataBlockRecord record;
            record.kind = kind;
            record.typeId = *typeId;
            record.schemaVersion = {.major = static_cast<std::uint32_t>(major.value_or(1)),
                                    .minor = static_cast<std::uint32_t>(minor.value_or(0))};
            record.mediaType = *mediaType;
            record.provenance.source = document::DataBlockProducer{
                .name = "bloom.scripting", .version = "1", .parametersDigest = {}};
            record.provenance.createdAt = createdAt.value_or(std::string());
            record.payload = *std::move(content);
            record.tags = tags.value_or(std::vector<std::string>{});
            return OperationCreateResult(
                std::make_unique<commands::CreateDataBlock>(std::move(record)), std::nullopt);
        });

    add("bloom.data-block.replace-payload",
        {need("dataBlock", ValueKind::Id, StableId{1}, "The block whose payload is replaced."),
         maybe("payloadKind", ValueKind::String, "curve",
               R"("curve", "ramp" or "opaque". Defaults to "opaque".)"),
         maybe("payload", ValueKind::Array, ValueArray{Value(0.0), Value(1.0), Value(0.5)},
               "Curve or ramp payload numbers."),
         maybe("text", ValueKind::String, "", "Opaque payload bytes, as UTF-8 text.")},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto block = id(args, op, "dataBlock", error);
            auto content = payload(args, op, error);
            if (!block || !content) {
                return error;
            }
            return OperationCreateResult(
                std::make_unique<commands::ReplaceDataBlockPayload>(
                    document::DataBlockRecordId::fromRaw(*block), *std::move(content)),
                std::nullopt);
        });

    add("bloom.data-block.remove",
        {need("dataBlock", ValueKind::Id, StableId{1}, "The block to remove.")},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto block = id(args, op, "dataBlock", error);
            if (!block) {
                return error;
            }
            return OperationCreateResult(std::make_unique<commands::RemoveDataBlock>(
                                             document::DataBlockRecordId::fromRaw(*block)),
                                         std::nullopt);
        });

    add("bloom.data-block.relink",
        {need("dataBlock", ValueKind::Id, StableId{1}, "The block to repoint."),
         need("path", ValueKind::String, "data/curve.json", "The replacement source path."),
         maybe("locatorKind", ValueKind::String, "file", R"(Locator kind. Defaults to "file".)"),
         maybe("portability", ValueKind::String, "project-relative",
               R"(Locator portability. Defaults to "project-relative".)"),
         maybe("relinkHint", ValueKind::String, "", "A relink hint to remember.")},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto block = id(args, op, "dataBlock", error);
            auto where = locator(args, op, error);
            if (!block || !where) {
                return error;
            }
            return OperationCreateResult(
                std::make_unique<commands::RelinkDataBlock>(
                    document::DataBlockRecordId::fromRaw(*block), *std::move(where)),
                std::nullopt);
        });

    add("bloom.data-block.set-tags",
        {need("dataBlock", ValueKind::Id, StableId{1}, "The block to tag."),
         need("tags", ValueKind::Array, ValueArray{Value("authored")}, "The complete tag list.")},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto block = id(args, op, "dataBlock", error);
            const auto tags = strings(args, op, "tags", error);
            if (!block || !tags) {
                return error;
            }
            return OperationCreateResult(std::make_unique<commands::SetDataBlockTags>(
                                             document::DataBlockRecordId::fromRaw(*block), *tags),
                                         std::nullopt);
        });

    // --- compositions -------------------------------------------------------------------------
    add("bloom.composition.add",
        {need("name", ValueKind::String, "Scene", "The new composition's name."),
         maybe("width", ValueKind::Integer, std::int64_t{1920}, "Pixel width. Defaults to 1920."),
         maybe("height", ValueKind::Integer, std::int64_t{1080}, "Pixel height. Defaults to 1080."),
         maybe("duration", ValueKind::Time, std::int64_t{48},
               "Duration in frames. Defaults to 48."),
         maybe("frameRateNumerator", ValueKind::Integer, std::int64_t{24},
               "Frame-rate numerator. Defaults to 24."),
         maybe("frameRateDenominator", ValueKind::Integer, std::int64_t{1},
               "Frame-rate denominator. Defaults to 1."),
         maybe("background", ValueKind::Color4, core::Color4d{0.0, 0.0, 0.0, 1.0},
               "Background colour. Defaults to opaque black.")},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto name = text(args, op, "name", error);
            const auto compositionFormat =
                format(args, op, error, document::CompositionFormat::kDefaultWidth,
                       document::CompositionFormat::kDefaultHeight);
            const auto duration = rational(args, op, "duration", error, false);
            const auto background = color4(args, op, "background", error, false);
            if (!name || !compositionFormat || error) {
                return error;
            }
            return OperationCreateResult(
                std::make_unique<commands::AddComposition>(
                    *name, *compositionFormat, compositionFormat->frameRate(),
                    duration.value_or(core::RationalTime::fromInteger(48)),
                    background.value_or(core::Color4d{0.0, 0.0, 0.0, 1.0})),
                std::nullopt);
        });

    add("bloom.composition.clear-work-area", {compositionArgument()},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto composition = id(args, op, "composition", error);
            if (!composition) {
                return error;
            }
            return OperationCreateResult(std::make_unique<commands::ClearWorkArea>(
                                             document::CompositionId::fromRaw(*composition)),
                                         std::nullopt);
        });

    add("bloom.composition.delete", {compositionArgument()},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto composition = id(args, op, "composition", error);
            if (!composition) {
                return error;
            }
            return OperationCreateResult(std::make_unique<commands::DeleteComposition>(
                                             document::CompositionId::fromRaw(*composition)),
                                         std::nullopt);
        });

    add("bloom.composition.duplicate", {compositionArgument()},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto composition = id(args, op, "composition", error);
            if (!composition) {
                return error;
            }
            return OperationCreateResult(std::make_unique<commands::DuplicateComposition>(
                                             document::CompositionId::fromRaw(*composition)),
                                         std::nullopt);
        });

    add("bloom.composition.set-background-color",
        {compositionArgument(), need("color", ValueKind::Color4, core::Color4d{0.0, 0.0, 0.0, 1.0},
                                     "The background colour.")},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto composition = id(args, op, "composition", error);
            const auto color = color4(args, op, "color", error);
            if (!composition || !color) {
                return error;
            }
            return OperationCreateResult(
                std::make_unique<commands::SetCompositionBackgroundColor>(
                    document::CompositionId::fromRaw(*composition), *color),
                std::nullopt);
        });

    add("bloom.composition.set-duration",
        {compositionArgument(),
         need("duration", ValueKind::Time, std::int64_t{48}, "The new duration in frames.")},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto composition = id(args, op, "composition", error);
            const auto duration = rational(args, op, "duration", error);
            if (!composition || !duration) {
                return error;
            }
            return OperationCreateResult(
                std::make_unique<commands::SetCompositionDuration>(
                    document::CompositionId::fromRaw(*composition), *duration),
                std::nullopt);
        });

    add("bloom.composition.set-format",
        {compositionArgument(),
         need("width", ValueKind::Integer, std::int64_t{1920}, "Pixel width."),
         need("height", ValueKind::Integer, std::int64_t{1080}, "Pixel height."),
         maybe("frameRateNumerator", ValueKind::Integer, std::int64_t{24},
               "Frame-rate numerator. Defaults to 24."),
         maybe("frameRateDenominator", ValueKind::Integer, std::int64_t{1},
               "Frame-rate denominator. Defaults to 1.")},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto composition = id(args, op, "composition", error);
            const auto compositionFormat = format(args, op, error, 0, 0);
            if (!composition || !compositionFormat) {
                return error;
            }
            return OperationCreateResult(
                std::make_unique<commands::SetCompositionFormat>(
                    document::CompositionId::fromRaw(*composition), *compositionFormat),
                std::nullopt);
        });

    add("bloom.composition.set-name",
        {compositionArgument(), need("name", ValueKind::String, "Scene", "The new name.")},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto composition = id(args, op, "composition", error);
            const auto name = text(args, op, "name", error);
            if (!composition || !name) {
                return error;
            }
            return OperationCreateResult(std::make_unique<commands::SetCompositionName>(
                                             document::CompositionId::fromRaw(*composition), *name),
                                         std::nullopt);
        });

    add("bloom.composition.set-safe-areas",
        {compositionArgument(),
         maybe("action", ValueKind::Double, 0.9, "Action-safe fraction. Defaults to 0.9."),
         maybe("title", ValueKind::Double, 0.8, "Title-safe fraction. Defaults to 0.8.")},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto composition = id(args, op, "composition", error);
            const auto action = number(args, op, "action", error, false);
            const auto title = number(args, op, "title", error, false);
            if (!composition || error) {
                return error;
            }
            return OperationCreateResult(
                std::make_unique<commands::SetCompositionSafeAreas>(
                    document::CompositionId::fromRaw(*composition),
                    document::SafeAreaSettings{.action = action.value_or(0.90),
                                               .title = title.value_or(0.80)}),
                std::nullopt);
        });

    add("bloom.composition.set-work-area",
        {compositionArgument(),
         need("start", ValueKind::Time, std::int64_t{0}, "The work area's first frame."),
         need("end", ValueKind::Time, std::int64_t{24}, "The work area's last frame.")},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto composition = id(args, op, "composition", error);
            const auto start = rational(args, op, "start", error);
            const auto end = rational(args, op, "end", error);
            if (!composition || !start || !end) {
                return error;
            }
            return OperationCreateResult(
                std::make_unique<commands::SetWorkArea>(
                    document::CompositionId::fromRaw(*composition), *start, *end),
                std::nullopt);
        });

    // --- layers -------------------------------------------------------------------------------
    add("bloom.layer.add-audio",
        {compositionArgument(),
         need("asset", ValueKind::Id, StableId{1}, "The audio asset to place.")},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto composition = id(args, op, "composition", error);
            const auto asset = id(args, op, "asset", error);
            if (!composition || !asset) {
                return error;
            }
            return OperationCreateResult(std::make_unique<commands::AddAudioLayer>(
                                             document::CompositionId::fromRaw(*composition),
                                             document::AssetId::fromRaw(*asset)),
                                         std::nullopt);
        });

    add("bloom.layer.add-image",
        {compositionArgument(),
         need("asset", ValueKind::Id, StableId{1}, "The image asset to place.")},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto composition = id(args, op, "composition", error);
            const auto asset = id(args, op, "asset", error);
            if (!composition || !asset) {
                return error;
            }
            return OperationCreateResult(std::make_unique<commands::AddImageLayer>(
                                             document::CompositionId::fromRaw(*composition),
                                             document::AssetId::fromRaw(*asset)),
                                         std::nullopt);
        });

    add("bloom.layer.add-shape",
        {compositionArgument(),
         need("kind", ValueKind::String, "rectangle",
              R"("rectangle", "ellipse", "triangle", "polygon", "star", "line" or "path".)"),
         maybe("position", ValueKind::Vec2, document::Vec2d{960.0, 540.0},
               "Layer position. Defaults to the composition centre."),
         maybe("size", ValueKind::Vec2, document::Vec2d{400.0, 300.0}, "Shape size."),
         maybe("lineStart", ValueKind::Vec2, document::Vec2d{0.0, 0.0},
               "First point of a line shape."),
         maybe("lineEnd", ValueKind::Vec2, document::Vec2d{100.0, 0.0},
               "Second point of a line shape."),
         maybe("path", ValueKind::Array,
               ValueArray{Value(ValueArray{Value(0.0), Value(0.0)}),
                          Value(ValueArray{Value(100.0), Value(0.0)})},
               "Path anchors as (x, y) or (x, y, inX, inY, outX, outY)."),
         maybe("pathClosed", ValueKind::Boolean, false, "Whether a path shape is closed.")},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto composition = id(args, op, "composition", error);
            const auto kindName = text(args, op, "kind", error);
            if (!composition || !kindName) {
                return error;
            }
            const auto kind = shapeKindOf(Value(*kindName));
            if (!kind.has_value()) {
                return failure(op, "kind", "expects a shape kind name");
            }
            commands::ShapeLayerGeometry geometry;
            geometry.position = vec2(args, op, "position", error, false);
            geometry.size = vec2(args, op, "size", error, false);
            geometry.lineStart = vec2(args, op, "lineStart", error, false);
            geometry.lineEnd = vec2(args, op, "lineEnd", error, false);
            const auto closed = boolean(args, op, "pathClosed", error, false);
            if (error) {
                return error;
            }
            if (const auto* anchors = findArgument(args, "path")) {
                const auto* array = std::get_if<ValueArray>(&anchors->storage);
                if (array == nullptr || array->empty() || array->size() > 4096) {
                    return failure(op, "path", "expects 1..4096 anchors");
                }
                document::PathValue value;
                for (const auto& item : *array) {
                    const auto* entry = std::get_if<ValueArray>(&item.storage);
                    if (entry == nullptr || (entry->size() != 2 && entry->size() != 6)) {
                        return failure(op, "path",
                                       "expects (x, y) or (x, y, inX, inY, outX, outY) anchors");
                    }
                    std::array<double, 6> numbers{};
                    for (std::size_t index = 0; index < entry->size(); ++index) {
                        const auto component = numericOf((*entry)[index]);
                        if (!component.has_value() || !std::isfinite(*component)) {
                            return failure(op, "path", "expects finite anchor coordinates");
                        }
                        numbers[index] = *component;
                    }
                    document::PathAnchor anchor{
                        .point = {numbers[0], numbers[1]}, .inHandle = {}, .outHandle = {}};
                    if (entry->size() == 6) {
                        anchor.inHandle = document::Vec2d{numbers[2], numbers[3]};
                        anchor.outHandle = document::Vec2d{numbers[4], numbers[5]};
                    }
                    value.anchors.push_back(anchor);
                }
                value.closed = closed.value_or(false);
                geometry.path = std::move(value);
            }
            return OperationCreateResult(
                std::make_unique<commands::AddShapeLayer>(
                    document::CompositionId::fromRaw(*composition), *kind, std::move(geometry)),
                std::nullopt);
        });

    add("bloom.layer.add-solid",
        {compositionArgument(), need("name", ValueKind::String, "Red", "The layer name."),
         need("color", ValueKind::Color4, core::Color4d{1.0, 0.0, 0.0, 1.0},
              "The solid colour as (r, g, b, a)."),
         maybe("position", ValueKind::Vec2, document::Vec2d{960.0, 540.0},
               "Layer position. Defaults to the composition centre."),
         maybe("opacity", ValueKind::Double, 1.0, "Layer opacity in 0..1. Defaults to 1.")},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto composition = id(args, op, "composition", error);
            const auto name = text(args, op, "name", error);
            const auto color = color4(args, op, "color", error);
            const auto position = vec2(args, op, "position", error, false);
            const auto opacity = number(args, op, "opacity", error, false);
            if (!composition || !name || !color || error) {
                return error;
            }
            const auto compositionId = document::CompositionId::fromRaw(*composition);
            if (position.has_value()) {
                return OperationCreateResult(
                    std::make_unique<commands::AddSolidLayer>(compositionId, *name, *color,
                                                              *position, opacity.value_or(1.0)),
                    std::nullopt);
            }
            return OperationCreateResult(
                std::make_unique<commands::AddSolidLayer>(compositionId, *name, *color),
                std::nullopt);
        });

    add("bloom.layer.add-text",
        {compositionArgument(), need("name", ValueKind::String, "Title", "The layer name."),
         need("text", ValueKind::String, "Hello, Bloom!", "The text content."),
         maybe("position", ValueKind::Vec2, document::Vec2d{960.0, 540.0},
               "Layer position. Defaults to the composition centre."),
         maybe("opacity", ValueKind::Double, 1.0, "Layer opacity in 0..1. Defaults to 1."),
         maybe("size", ValueKind::Double, 72.0, "Em size in pixels."),
         maybe("color", ValueKind::Color4, core::Color4d{1.0, 1.0, 1.0, 1.0},
               "Fill colour. Defaults to opaque white.")},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto composition = id(args, op, "composition", error);
            const auto name = text(args, op, "name", error);
            const auto content = text(args, op, "text", error);
            const auto position = vec2(args, op, "position", error, false);
            const auto opacity = number(args, op, "opacity", error, false);
            const auto size = number(args, op, "size", error, false);
            const auto color = color4(args, op, "color", error, false);
            if (!composition || !name || !content || error) {
                return error;
            }
            const auto compositionId = document::CompositionId::fromRaw(*composition);
            if (position.has_value() || opacity.has_value() || size.has_value() ||
                color.has_value()) {
                return OperationCreateResult(
                    std::make_unique<commands::AddTextLayer>(
                        compositionId, *name, *content, position.value_or(document::Vec2d{}),
                        opacity.value_or(1.0), size.value_or(document::kDefaultTextSizePixels),
                        color.value_or(core::Color4d{1.0, 1.0, 1.0, 1.0})),
                    std::nullopt);
            }
            return OperationCreateResult(
                std::make_unique<commands::AddTextLayer>(compositionId, *name, *content),
                std::nullopt);
        });

    add("bloom.layer.move-before",
        {compositionArgument(), need("slot", ValueKind::Id, StableId{1}, "The stack slot to move."),
         maybe("before", ValueKind::Id, StableId{1},
               "The slot to land above. Omit to move to the bottom.")},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto composition = id(args, op, "composition", error);
            const auto slot = id(args, op, "slot", error);
            const auto before = id(args, op, "before", error, false);
            if (!composition || !slot || error) {
                return error;
            }
            return OperationCreateResult(
                std::make_unique<commands::MoveLayerBefore>(
                    document::CompositionId::fromRaw(*composition),
                    document::LayerSlotId::fromRaw(*slot),
                    before.has_value() ? std::optional(document::LayerSlotId::fromRaw(*before))
                                       : std::nullopt),
                std::nullopt);
        });

    add("bloom.layer.rename",
        {compositionArgument(), layerArgument(),
         need("name", ValueKind::String, "Title", "The new layer name.")},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto composition = id(args, op, "composition", error);
            const auto layer = id(args, op, "layer", error);
            const auto name = text(args, op, "name", error);
            if (!composition || !layer || !name) {
                return error;
            }
            return OperationCreateResult(std::make_unique<commands::RenameLayer>(
                                             document::CompositionId::fromRaw(*composition),
                                             document::LayerId::fromRaw(*layer), *name),
                                         std::nullopt);
        });

    const auto layerFlag = [](const char* argument, auto make) {
        return [argument, make](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto composition = id(args, op, "composition", error);
            const auto layer = id(args, op, "layer", error);
            const auto value = boolean(args, op, argument, error);
            if (!composition || !layer || !value) {
                return error;
            }
            return OperationCreateResult(make(document::CompositionId::fromRaw(*composition),
                                              document::LayerId::fromRaw(*layer), *value),
                                         std::nullopt);
        };
    };

    add("bloom.layer.set-enabled",
        {compositionArgument(), layerArgument(),
         need("enabled", ValueKind::Boolean, true, "Whether the layer renders.")},
        layerFlag("enabled", [](const document::CompositionId composition,
                                const document::LayerId layer, const bool value) {
            return std::make_unique<commands::SetLayerEnabled>(composition, layer, value);
        }));

    add("bloom.layer.set-solo",
        {compositionArgument(), layerArgument(),
         need("solo", ValueKind::Boolean, true, "Whether the layer is soloed.")},
        layerFlag("solo", [](const document::CompositionId composition,
                             const document::LayerId layer, const bool value) {
            return std::make_unique<commands::SetLayerSolo>(composition, layer, value);
        }));

    add("bloom.layer.set-locked",
        {compositionArgument(), layerArgument(),
         need("locked", ValueKind::Boolean, true, "Whether the layer is locked.")},
        layerFlag("locked", [](const document::CompositionId composition,
                               const document::LayerId layer, const bool value) {
            return std::make_unique<commands::SetLayerLocked>(composition, layer, value);
        }));

    add("bloom.layer.set-label-color",
        {compositionArgument(), layerArgument(),
         maybe("color", ValueKind::Vec3, document::Vec3d{255.0, 128.0, 0.0},
               "Label colour as (r, g, b) in 0..255. Omit to clear the label.")},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto composition = id(args, op, "composition", error);
            const auto layer = id(args, op, "layer", error);
            const auto color = vec3(args, op, "color", error, false);
            if (!composition || !layer || error) {
                return error;
            }
            std::optional<std::array<std::uint8_t, 3>> label;
            if (color.has_value()) {
                const std::array<double, 3> channels{color->x, color->y, color->z};
                std::array<std::uint8_t, 3> bytes{};
                for (std::size_t index = 0; index < channels.size(); ++index) {
                    if (channels[index] < 0.0 || channels[index] > 255.0) {
                        return failure(op, "color", "expects each channel in 0..255");
                    }
                    bytes[index] = static_cast<std::uint8_t>(std::lround(channels[index]));
                }
                label = bytes;
            }
            return OperationCreateResult(std::make_unique<commands::SetLayerLabelColor>(
                                             document::CompositionId::fromRaw(*composition),
                                             document::LayerId::fromRaw(*layer), label),
                                         std::nullopt);
        });

    add("bloom.layer.set-parent",
        {compositionArgument(), layerArgument(),
         maybe("parent", ValueKind::Id, StableId{1},
               "The parent layer. Omit to clear the parent.")},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto composition = id(args, op, "composition", error);
            const auto layer = id(args, op, "layer", error);
            const auto parent = id(args, op, "parent", error, false);
            if (!composition || !layer || error) {
                return error;
            }
            return OperationCreateResult(
                std::make_unique<commands::SetLayerParent>(
                    document::CompositionId::fromRaw(*composition),
                    document::LayerId::fromRaw(*layer),
                    parent.has_value() ? std::optional(document::LayerId::fromRaw(*parent))
                                       : std::nullopt),
                std::nullopt);
        });

    add("bloom.layer.set-range",
        {compositionArgument(), layerArgument(),
         need("start", ValueKind::Time, std::int64_t{0}, "The layer's first frame."),
         need("end", ValueKind::Time, std::int64_t{24}, "The layer's last frame.")},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto composition = id(args, op, "composition", error);
            const auto layer = id(args, op, "layer", error);
            const auto start = rational(args, op, "start", error);
            const auto end = rational(args, op, "end", error);
            if (!composition || !layer || !start || !end) {
                return error;
            }
            return OperationCreateResult(std::make_unique<commands::SetLayerRange>(
                                             document::CompositionId::fromRaw(*composition),
                                             document::LayerId::fromRaw(*layer), *start, *end),
                                         std::nullopt);
        });

    add("bloom.layer.split-at-time", {compositionArgument(), layerArgument(), timeArgument()},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto composition = id(args, op, "composition", error);
            const auto layer = id(args, op, "layer", error);
            const auto time = rational(args, op, "time", error);
            if (!composition || !layer || !time) {
                return error;
            }
            return OperationCreateResult(std::make_unique<commands::SplitLayerAtTime>(
                                             document::CompositionId::fromRaw(*composition),
                                             document::LayerId::fromRaw(*layer), *time),
                                         std::nullopt);
        });

    // --- merge --------------------------------------------------------------------------------
    add("bloom.merge.reorder-input",
        {compositionArgument(), need("merge", ValueKind::Id, StableId{1}, "The Merge node."),
         need("slot", ValueKind::Id, StableId{1}, "The stack slot to move."),
         need("index", ValueKind::Integer, std::int64_t{0}, "The slot's new position.")},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto composition = id(args, op, "composition", error);
            const auto merge = id(args, op, "merge", error);
            const auto slot = id(args, op, "slot", error);
            const auto index = integer(args, op, "index", error);
            if (!composition || !merge || !slot || !index) {
                return error;
            }
            if (*index < 0) {
                return failure(op, "index", "expects a position of 0 or more");
            }
            return OperationCreateResult(std::make_unique<commands::ReorderMergeInput>(
                                             document::CompositionId::fromRaw(*composition),
                                             document::NodeId::fromRaw(*merge),
                                             document::LayerSlotId::fromRaw(*slot),
                                             static_cast<std::size_t>(*index)),
                                         std::nullopt);
        });

    add("bloom.merge.set-enabled",
        {compositionArgument(), need("merge", ValueKind::Id, StableId{1}, "The Merge node."),
         need("enabled", ValueKind::Boolean, true, "Whether the Merge is enabled.")},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto composition = id(args, op, "composition", error);
            const auto merge = id(args, op, "merge", error);
            const auto enabled = boolean(args, op, "enabled", error);
            if (!composition || !merge || !enabled) {
                return error;
            }
            return OperationCreateResult(std::make_unique<commands::SetMergeEnabled>(
                                             document::CompositionId::fromRaw(*composition),
                                             document::NodeId::fromRaw(*merge), *enabled),
                                         std::nullopt);
        });

    // --- node groups --------------------------------------------------------------------------
    add("bloom.node-group.create",
        {compositionArgument(), nodeSetArgument("nodes"),
         maybe("name", ValueKind::String, "Group", R"(The frame's name. Defaults to "Group".)")},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto composition = id(args, op, "composition", error);
            const auto nodes = identities(args, op, "nodes", error);
            const auto name = text(args, op, "name", error, false);
            if (!composition || !nodes || error) {
                return error;
            }
            std::set<document::NodeId> members;
            for (const auto identity : *nodes) {
                members.insert(document::NodeId::fromRaw(identity));
            }
            return OperationCreateResult(
                std::make_unique<commands::GroupNodes>(
                    document::CompositionId::fromRaw(*composition), std::move(members),
                    name.value_or(std::string(commands::kDefaultNodeGroupName))),
                std::nullopt);
        });

    add("bloom.node-group.remove",
        {compositionArgument(), need("group", ValueKind::Id, StableId{1}, "The group to remove.")},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto composition = id(args, op, "composition", error);
            const auto group = id(args, op, "group", error);
            if (!composition || !group) {
                return error;
            }
            return OperationCreateResult(std::make_unique<commands::UngroupNodes>(
                                             document::CompositionId::fromRaw(*composition),
                                             document::NodeGroupId::fromRaw(*group)),
                                         std::nullopt);
        });

    add("bloom.node-group.rename",
        {compositionArgument(), need("group", ValueKind::Id, StableId{1}, "The group to rename."),
         need("name", ValueKind::String, "Group", "The new name.")},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto composition = id(args, op, "composition", error);
            const auto group = id(args, op, "group", error);
            const auto name = text(args, op, "name", error);
            if (!composition || !group || !name) {
                return error;
            }
            return OperationCreateResult(std::make_unique<commands::RenameGroup>(
                                             document::CompositionId::fromRaw(*composition),
                                             document::NodeGroupId::fromRaw(*group), *name),
                                         std::nullopt);
        });

    add("bloom.node-group.set-members",
        {compositionArgument(), need("group", ValueKind::Id, StableId{1}, "The group to edit."),
         nodeSetArgument("members")},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto composition = id(args, op, "composition", error);
            const auto group = id(args, op, "group", error);
            const auto nodes = identities(args, op, "members", error);
            if (!composition || !group || !nodes) {
                return error;
            }
            std::set<document::NodeId> members;
            for (const auto identity : *nodes) {
                members.insert(document::NodeId::fromRaw(identity));
            }
            return OperationCreateResult(std::make_unique<commands::SetGroupMembers>(
                                             document::CompositionId::fromRaw(*composition),
                                             document::NodeGroupId::fromRaw(*group),
                                             std::move(members)),
                                         std::nullopt);
        });

    // --- nodes --------------------------------------------------------------------------------
    add("bloom.node.add",
        {compositionArgument(),
         need("nodeType", ValueKind::String, "bloom.value-scalar", "The node definition id."),
         maybe("position", ValueKind::Vec2, document::Vec2d{0.0, 0.0},
               "Graph layout position. Defaults to the origin.")},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto composition = id(args, op, "composition", error);
            const auto nodeType = text(args, op, "nodeType", error);
            const auto position = vec2(args, op, "position", error, false);
            if (!composition || !nodeType || error) {
                return error;
            }
            return OperationCreateResult(std::make_unique<commands::AddNode>(
                                             document::CompositionId::fromRaw(*composition),
                                             *nodeType, position.value_or(document::Vec2d{})),
                                         std::nullopt);
        });

    add("bloom.node.connect",
        {compositionArgument(),
         need("sourceNode", ValueKind::Id, StableId{1}, "The node that produces the value."),
         need("sourcePort", ValueKind::String, "output", "The output port name."),
         need("targetNode", ValueKind::Id, StableId{1},
              "The node, or Merge stack node, that consumes it."),
         alternative("port", ValueKind::String, "input",
                     "The input port name. Required unless a stack role is given."),
         maybe("role", ValueKind::String, "layer",
               "A Merge stack role. Giving it addresses the layer stack instead of a port."),
         maybe("slot", ValueKind::Id, StableId{1},
               "The stack slot to write. 0, or omitted, creates a new slot."),
         maybe("insertBefore", ValueKind::Id, StableId{1},
               "The slot a new stack link lands above.")},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto composition = id(args, op, "composition", error);
            const auto sourceNode = id(args, op, "sourceNode", error);
            const auto sourcePort = text(args, op, "sourcePort", error);
            auto destination = inputPort(args, op, "targetNode", error);
            const auto insertBefore = id(args, op, "insertBefore", error, false);
            if (!composition || !sourceNode || !sourcePort || !destination || error) {
                return error;
            }
            return OperationCreateResult(
                std::make_unique<commands::ConnectPorts>(
                    document::CompositionId::fromRaw(*composition),
                    document::OutputPortRef{.nodeId = document::NodeId::fromRaw(*sourceNode),
                                            .port = *sourcePort},
                    *std::move(destination), document::builtInNodeDefinitions(),
                    insertBefore.has_value()
                        ? std::optional(document::LayerSlotId::fromRaw(*insertBefore))
                        : std::nullopt),
                std::nullopt);
        });

    add("bloom.node.disconnect-input",
        {compositionArgument(),
         need("node", ValueKind::Id, StableId{1}, "The node whose input is cleared."),
         alternative("port", ValueKind::String, "input",
                     "The input port name. Required unless a stack role is given."),
         maybe("role", ValueKind::String, "layer", "A Merge stack role."),
         maybe("slot", ValueKind::Id, StableId{1}, "The stack slot to clear.")},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto composition = id(args, op, "composition", error);
            auto input = inputPort(args, op, "node", error);
            if (!composition || !input) {
                return error;
            }
            return OperationCreateResult(
                std::make_unique<commands::DisconnectInput>(
                    document::CompositionId::fromRaw(*composition), *std::move(input)),
                std::nullopt);
        });

    add("bloom.node.dissolve",
        {compositionArgument(), need("node", ValueKind::Id, StableId{1}, "The node to dissolve.")},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto composition = id(args, op, "composition", error);
            const auto node = id(args, op, "node", error);
            if (!composition || !node) {
                return error;
            }
            return OperationCreateResult(std::make_unique<commands::DissolveNode>(
                                             document::CompositionId::fromRaw(*composition),
                                             document::NodeId::fromRaw(*node)),
                                         std::nullopt);
        });

    add("bloom.node.duplicate",
        {compositionArgument(), nodeSetArgument("nodes"),
         maybe("offset", ValueKind::Vec2, document::Vec2d{40.0, 40.0},
               "Layout offset for the copies. Defaults to (0, 0).")},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto composition = id(args, op, "composition", error);
            const auto nodes = identities(args, op, "nodes", error);
            const auto offset = vec2(args, op, "offset", error, false);
            if (!composition || !nodes || error) {
                return error;
            }
            std::set<document::NodeId> members;
            for (const auto identity : *nodes) {
                members.insert(document::NodeId::fromRaw(identity));
            }
            return OperationCreateResult(std::make_unique<commands::DuplicateNodes>(
                                             document::CompositionId::fromRaw(*composition),
                                             std::move(members),
                                             offset.value_or(document::Vec2d{})),
                                         std::nullopt);
        });

    add("bloom.node.move",
        {compositionArgument(),
         need("positions", ValueKind::Array,
              ValueArray{Value(ValueArray{Value(StableId{1}), Value(0.0), Value(0.0)})},
              "Layout positions as (node, x, y)."),
         maybe("groups", ValueKind::Array,
               ValueArray{Value(ValueArray{Value(StableId{1}), Value(StableId{1})})},
               "Group membership as (node, group); a group of 0 leaves the node ungrouped.")},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto composition = id(args, op, "composition", error);
            const auto positions =
                list(args, op, "positions", error, "expects (node, x, y) entries");
            if (!composition || !positions) {
                return error;
            }
            if (positions->empty() || positions->size() > 4096) {
                return failure(op, "positions", "expects 1..4096 entries");
            }
            std::map<document::NodeId, document::Vec2d> layout;
            for (const auto& item : *positions) {
                const auto* entry = std::get_if<ValueArray>(&item.storage);
                if (entry == nullptr || entry->size() != 3) {
                    return failure(op, "positions", "expects (node, x, y) entries");
                }
                const auto node = idOf((*entry)[0]);
                const auto x = numericOf((*entry)[1]);
                const auto y = numericOf((*entry)[2]);
                if (!node.has_value() || !x.has_value() || !y.has_value() || !std::isfinite(*x) ||
                    !std::isfinite(*y)) {
                    return failure(op, "positions", "expects (node, x, y) entries");
                }
                layout.emplace(document::NodeId::fromRaw(*node), document::Vec2d{*x, *y});
            }
            commands::NodeGroupMembershipDelta membership;
            if (const auto* groups = findArgument(args, "groups")) {
                const auto* array = std::get_if<ValueArray>(&groups->storage);
                if (array == nullptr || array->size() > 4096) {
                    return failure(op, "groups", "expects (node, group) entries");
                }
                for (const auto& item : *array) {
                    const auto* entry = std::get_if<ValueArray>(&item.storage);
                    if (entry == nullptr || entry->size() != 2) {
                        return failure(op, "groups", "expects (node, group) entries");
                    }
                    const auto node = idOf((*entry)[0]);
                    const auto group = integerOf((*entry)[1]);
                    if (!node.has_value() || !group.has_value() || *group < 0) {
                        return failure(op, "groups", "expects (node, group) entries");
                    }
                    membership.emplace(document::NodeId::fromRaw(*node),
                                       *group == 0 ? std::optional<document::NodeGroupId>{}
                                                   : std::optional(document::NodeGroupId::fromRaw(
                                                         static_cast<std::uint64_t>(*group))));
                }
            }
            return OperationCreateResult(std::make_unique<commands::MoveNodes>(
                                             document::CompositionId::fromRaw(*composition),
                                             std::move(layout), std::move(membership)),
                                         std::nullopt);
        });

    add("bloom.node.remove", {compositionArgument(), nodeSetArgument("nodes")},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto composition = id(args, op, "composition", error);
            const auto nodes = identities(args, op, "nodes", error);
            if (!composition || !nodes) {
                return error;
            }
            std::set<document::NodeId> members;
            for (const auto identity : *nodes) {
                members.insert(document::NodeId::fromRaw(identity));
            }
            return OperationCreateResult(
                std::make_unique<commands::RemoveNodes>(
                    document::CompositionId::fromRaw(*composition), std::move(members)),
                std::nullopt);
        });

    const auto nodeFlag = [](const char* argument, auto make) {
        return [argument, make](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto composition = id(args, op, "composition", error);
            const auto node = id(args, op, "node", error);
            const auto value = boolean(args, op, argument, error);
            if (!composition || !node || !value) {
                return error;
            }
            return OperationCreateResult(make(document::CompositionId::fromRaw(*composition),
                                              document::NodeId::fromRaw(*node), *value),
                                         std::nullopt);
        };
    };

    add("bloom.node.set-collapsed",
        {compositionArgument(), need("node", ValueKind::Id, StableId{1}, "The node to edit."),
         need("collapsed", ValueKind::Boolean, true, "Whether the node is collapsed.")},
        nodeFlag("collapsed", [](const document::CompositionId composition,
                                 const document::NodeId node, const bool value) {
            return std::make_unique<commands::SetNodeCollapsed>(composition, node, value);
        }));

    add("bloom.node.set-muted",
        {compositionArgument(), need("node", ValueKind::Id, StableId{1}, "The node to edit."),
         need("muted", ValueKind::Boolean, true, "Whether the node is muted.")},
        nodeFlag("muted", [](const document::CompositionId composition, const document::NodeId node,
                             const bool value) {
            return std::make_unique<commands::SetNodeMuted>(composition, node, value);
        }));

    add("bloom.node.set-width",
        {compositionArgument(), need("node", ValueKind::Id, StableId{1}, "The node to edit."),
         need("width", ValueKind::Double, 160.0, "The node's layout width.")},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto composition = id(args, op, "composition", error);
            const auto node = id(args, op, "node", error);
            const auto width = number(args, op, "width", error);
            if (!composition || !node || !width) {
                return error;
            }
            return OperationCreateResult(std::make_unique<commands::SetNodeWidth>(
                                             document::CompositionId::fromRaw(*composition),
                                             document::NodeId::fromRaw(*node), *width),
                                         std::nullopt);
        });

    // --- parameters and project ---------------------------------------------------------------
    add("bloom.parameter.set-source",
        {compositionArgument(),
         need("parameter", ValueKind::Id, StableId{1}, "The parameter to repoint."),
         alternative("value", ValueKind::Value, 1.0, "A constant value."),
         maybe("curve", ValueKind::Id, StableId{1}, "An existing animation curve."),
         maybe("driverNode", ValueKind::Id, StableId{1}, "A value-graph node that drives it."),
         maybe("driverPort", ValueKind::String, "output", "The driving node's output port.")},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto composition = id(args, op, "composition", error);
            const auto parameter = id(args, op, "parameter", error);
            const auto curve = id(args, op, "curve", error, false);
            const auto driverNode = id(args, op, "driverNode", error, false);
            if (!composition || !parameter || error) {
                return error;
            }
            const auto compositionId = document::CompositionId::fromRaw(*composition);
            const auto parameterId = document::ParameterId::fromRaw(*parameter);
            std::optional<document::ParameterValue> authoredValue;
            if (const auto* constant = findArgument(args, "value")) {
                authoredValue = parameterValueOf(*constant);
                if (!authoredValue.has_value()) {
                    return failure(op, "value", "expects a number, text, vector or colour");
                }
            }
            if (curve.has_value()) {
                return OperationCreateResult(
                    std::make_unique<commands::SetParameterSource>(
                        compositionId, parameterId,
                        document::AnimationCurveSource{
                            .curveId = document::AnimationCurveId::fromRaw(*curve),
                            .defaultValue = authoredValue}),
                    std::nullopt);
            }
            if (driverNode.has_value()) {
                const auto port = text(args, op, "driverPort", error);
                if (!port) {
                    return error;
                }
                return OperationCreateResult(
                    std::make_unique<commands::SetParameterSource>(
                        compositionId, parameterId,
                        document::DriverBindingSource{.sourceNodeId =
                                                          document::NodeId::fromRaw(*driverNode),
                                                      .outputPort = *port}),
                    std::nullopt);
            }
            if (!authoredValue.has_value()) {
                return failure(op, "value", "is required unless a curve or a driverNode is given");
            }
            return OperationCreateResult(
                std::make_unique<commands::SetParameterSource>(
                    compositionId, parameterId,
                    document::ConstantValueSource{.value = *std::move(authoredValue)}),
                std::nullopt);
        });

    add("bloom.project.set-name",
        {need("name", ValueKind::String, "Title study", "The new project name.")},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto name = text(args, op, "name", error);
            if (!name) {
                return error;
            }
            return OperationCreateResult(std::make_unique<commands::SetProjectName>(*name),
                                         std::nullopt);
        });

    std::ranges::sort(
        all, {}, [](const OperationDescriptor& descriptor) { return descriptor.schema.typeId; });
    return registry;
}

const OperationDescriptor* OperationRegistry::find(const std::string_view typeId) const noexcept {
    const auto iterator = std::ranges::find(
        descriptors_, typeId, [](const auto& descriptor) { return descriptor.schema.typeId; });
    return iterator == descriptors_.end() ? nullptr : &*iterator;
}

OperationCreateResult OperationRegistry::create(const std::string_view typeId,
                                                const Arguments& arguments) const {
    const auto* descriptor = find(typeId);
    if (descriptor == nullptr) {
        return OperationCreateResult(
            nullptr, OperationDiagnostic{.code = "bloom.scripting.unknown-operation",
                                         .operationId = std::string(typeId),
                                         .argument = {},
                                         .message = "Unknown operation id"});
    }
    for (const auto& argument : descriptor->schema.arguments) {
        if (argument.required && !arguments.contains(argument.name)) {
            return decorate(descriptor->schema,
                            failure(descriptor->schema.typeId, argument.name, "is required"));
        }
    }
    return decorate(descriptor->schema, descriptor->factory(descriptor->schema.typeId, arguments));
}

OperationCreateResult OperationRegistry::create(const std::string_view typeId, Arguments arguments,
                                                const OperationContext& context) const {
    if (const auto* descriptor = find(typeId); descriptor != nullptr) {
        static_cast<void>(applyContextDefaults(descriptor->schema, arguments, context));
    }
    return create(typeId, arguments);
}

} // namespace bloom::scripting
