#pragma once

// The scalar value readers every operation factory shares, so a time, an id or a whole number
// reads the same way in every registry file.

#include <bloom/core/rational_time.hpp>
#include <bloom/scripting/operation_registry.hpp>
#include <bloom/scripting/value.hpp>

#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>

namespace bloom::scripting::detail {

[[nodiscard]] inline OperationCreateResult
argumentFailure(const std::string& operationId, std::string argument, std::string message) {
    return OperationCreateResult(nullptr,
                                 OperationDiagnostic{.code = "bloom.scripting.invalid-argument",
                                                     .operationId = operationId,
                                                     .argument = std::move(argument),
                                                     .message = std::move(message)});
}

[[nodiscard]] inline std::optional<double> numericOf(const Value& value) {
    if (const auto* result = std::get_if<double>(&value.storage)) {
        return *result;
    }
    if (const auto* result = std::get_if<std::int64_t>(&value.storage)) {
        return static_cast<double>(*result);
    }
    return std::nullopt;
}

[[nodiscard]] inline std::optional<std::int64_t> integerOf(const Value& value) {
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

[[nodiscard]] inline std::optional<std::uint64_t> idOf(const Value& value) {
    const auto number = integerOf(value);
    return number.has_value() && *number > 0 ? std::optional(static_cast<std::uint64_t>(*number))
                                             : std::nullopt;
}

[[nodiscard]] inline std::optional<core::RationalTime> rationalOf(const Value& value) {
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

// Creates an operation through `factory` after turning every whole-frame time among its arguments
// into exact seconds (operation_frame_times.cpp): at the rate the operation itself sets, else at
// its composition's rate when it applies. `measuresNow` factories need seconds at creation and use
// the context's compositionFrameRate lookup, or refuse a frame count without one.
[[nodiscard]] OperationCreateResult
createWithFrameTimes(const OperationSchema& schema, const Arguments& arguments,
                     const OperationContext* context, bool measuresNow,
                     const std::function<OperationCreateResult(const Arguments&)>& factory);

// Registers bloom.layer.align-layers and bloom.layer.distribute-layers (ADR 0031).
void addLayerAlignmentOperations(std::vector<OperationDescriptor>& descriptors);
// Registers the bloom.export.* project-truth operations (ADR 0033).
void addExportOperations(std::vector<OperationDescriptor>& descriptors);

} // namespace bloom::scripting::detail
