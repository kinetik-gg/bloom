#include "value_utility_support.hpp"

#include <algorithm>
#include <cmath>
#include <type_traits>

namespace bloom::runtime {
namespace {
using Kernel = document::ValueUtilityKernel;

ValueUtilityOutcome failure(std::string_view detail, const std::size_t operand = 0) {
    ValueUtilityOutcome result;
    result.failed = true;
    result.summary = "Data block read failed";
    result.detail = detail;
    result.failedOperand = operand;
    return result;
}

const CompiledValue* operand(const ValueUtilityInvocation& invocation, const std::size_t index) {
    return index < invocation.operands.size() ? invocation.operands[index] : nullptr;
}

bool scalar(const CompiledValue* value, double& result) {
    if (const auto* number = value == nullptr ? nullptr : std::get_if<double>(value)) {
        result = *number;
        return true;
    }
    if (const auto* integer = value == nullptr ? nullptr : std::get_if<std::int64_t>(value)) {
        result = static_cast<double>(*integer);
        return true;
    }
    return false;
}
} // namespace

ValueUtilityOutcome detail::evaluateValueDataBlock(const ValueUtilityInvocation& invocation) {
    if (invocation.dataBlock == nullptr)
        return failure("The selected data block is unavailable");
    const auto& block = *invocation.dataBlock;
    if (invocation.operation == Kernel::DataSample) {
        const auto* curve = std::get_if<document::DataBlockCurve>(&block.payload);
        double time = 0.0;
        if (curve == nullptr || !scalar(operand(invocation, 0), time) || curve->samples.empty() ||
            !std::isfinite(time) || !std::isfinite(curve->domainStart) ||
            !std::isfinite(curve->domainEnd) || !(curve->domainEnd > curve->domainStart))
            return failure("Data Sample requires a valid Curve block and scalar time");
        if (time <= curve->domainStart || time >= curve->domainEnd) {
            const auto endpoint =
                time <= curve->domainStart ? curve->samples.front() : curve->samples.back();
            if (!std::isfinite(endpoint))
                return failure("Data Sample endpoint is not finite");
            ValueUtilityOutcome result;
            result.outputs[0] = endpoint;
            return result;
        }
        // Use direct differences when finite: half-scaling a subnormal interval would round both
        // endpoints to zero. Only the genuinely overflowing difference needs scaled operands.
        const double span = curve->domainEnd - curve->domainStart;
        const double offset = time - curve->domainStart;
        const double normalized = std::isfinite(span) && std::isfinite(offset)
                                      ? offset / span
                                      : ((time * 0.5) - (curve->domainStart * 0.5)) /
                                            ((curve->domainEnd * 0.5) - (curve->domainStart * 0.5));
        if (!std::isfinite(normalized) || normalized < 0.0 || normalized > 1.0)
            return failure("Data Sample position is not representable");
        const double position = normalized * static_cast<double>(curve->samples.size() - 1);
        if (!std::isfinite(position) || position < 0.0 ||
            position >= static_cast<double>(curve->samples.size()))
            return failure("Data Sample index is not representable");
        const auto lower = static_cast<std::size_t>(position);
        const auto upper = std::min(lower + 1, curve->samples.size() - 1);
        const double fraction = position - static_cast<double>(lower);
        ValueUtilityOutcome result;
        const auto left = curve->samples[lower];
        const auto right = curve->samples[upper];
        if (!std::isfinite(left) || !std::isfinite(right))
            return failure("Data Sample values must be finite");
        const auto interpolated = std::lerp(left, right, fraction);
        if (!std::isfinite(interpolated))
            return failure("Data Sample interpolation is not representable");
        result.outputs[0] = interpolated;
        return result;
    }
    if (invocation.operation == Kernel::RampSample) {
        const auto* ramp = std::get_if<document::DataBlockRamp>(&block.payload);
        double time = 0.0;
        if (ramp == nullptr || ramp->stops.empty() || !scalar(operand(invocation, 0), time))
            return failure("Ramp Sample requires a valid Ramp block and scalar time");
        const auto& first = ramp->stops.front();
        const auto& last = ramp->stops.back();
        if (time <= first.position) {
            ValueUtilityOutcome result;
            result.outputs[0] = first.color;
            return result;
        }
        if (time >= last.position) {
            ValueUtilityOutcome result;
            result.outputs[0] = last.color;
            return result;
        }
        for (std::size_t index = 1; index < ramp->stops.size(); ++index) {
            if (time <= ramp->stops[index].position) {
                const auto& left = ramp->stops[index - 1];
                const auto& right = ramp->stops[index];
                const double f = (time - left.position) / (right.position - left.position);
                ValueUtilityOutcome result;
                result.outputs[0] =
                    core::Color4d{left.color.red + (right.color.red - left.color.red) * f,
                                  left.color.green + (right.color.green - left.color.green) * f,
                                  left.color.blue + (right.color.blue - left.color.blue) * f,
                                  left.color.alpha + (right.color.alpha - left.color.alpha) * f};
                return result;
            }
        }
        return failure("Ramp stops are not ordered");
    }
    if (invocation.operation == Kernel::TableLookup) {
        const auto* table = std::get_if<document::DataBlockTable>(&block.payload);
        const auto* row = operand(invocation, 0);
        const auto* column = operand(invocation, 1);
        const auto* rowValue = row == nullptr ? nullptr : std::get_if<std::int64_t>(row);
        const auto* columnValue = column == nullptr ? nullptr : std::get_if<std::int64_t>(column);
        if (table == nullptr || rowValue == nullptr || columnValue == nullptr || *rowValue < 0 ||
            *columnValue < 0 || static_cast<std::size_t>(*columnValue) >= table->columns.size())
            return failure("Table Lookup row or column is out of range");
        const auto& values = table->columns[static_cast<std::size_t>(*columnValue)].values;
        if (static_cast<std::size_t>(*rowValue) >= values.size())
            return failure("Table Lookup row is out of range", 0);
        double resultValue = 0.0;
        const auto& value = values[static_cast<std::size_t>(*rowValue)];
        const bool numeric = std::visit(
            [&resultValue](const auto& candidate) {
                using Candidate = std::decay_t<decltype(candidate)>;
                if constexpr (std::is_same_v<Candidate, double>) {
                    resultValue = candidate;
                    return true;
                } else if constexpr (std::is_same_v<Candidate, std::int64_t>) {
                    resultValue = static_cast<double>(candidate);
                    return true;
                } else {
                    return false;
                }
            },
            value);
        if (!numeric)
            return failure("Table Lookup selected value is not numeric", 1);
        ValueUtilityOutcome result;
        result.outputs[0] = resultValue;
        return result;
    }
    if (invocation.operation == Kernel::PointSetRead) {
        const auto* points = std::get_if<document::DataBlockPointSet>(&block.payload);
        const auto* index = operand(invocation, 0);
        const auto* indexValue = index == nullptr ? nullptr : std::get_if<std::int64_t>(index);
        if (points == nullptr || indexValue == nullptr || *indexValue < 0 ||
            static_cast<std::size_t>(*indexValue) >= points->points.size())
            return failure("Point Set index is out of range", 0);
        ValueUtilityOutcome result;
        result.outputs[0] = static_cast<std::int64_t>(points->points.size());
        result.outputs[1] = points->points[static_cast<std::size_t>(*indexValue)];
        result.outputCount = 2;
        return result;
    }
    return failure("Unsupported data block reader");
}
} // namespace bloom::runtime
