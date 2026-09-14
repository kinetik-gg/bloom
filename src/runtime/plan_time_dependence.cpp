#include <algorithm>
#include <bloom/runtime/compiled_plan.hpp>
#include <type_traits>

namespace bloom::runtime {
void CompiledCompositionPlan::analyzeTimeDependence() {
    std::vector<std::uint8_t> outputs(valueOutputCount_, 1);
    valueTimeDependent_.reserve(valueOperations_.size());
    for (const auto& operation : valueOperations_) {
        bool dependent = std::holds_alternative<CompiledValueTime>(operation.kernel);
        forEachValueOperand(operation.kernel, [&](const CompiledValueOperand& operand) {
            if (const auto* input = std::get_if<ValueOutputIndex>(&operand.source))
                dependent =
                    dependent || input->value() >= outputs.size() || outputs[input->value()] != 0;
            else
                dependent = dependent || !std::holds_alternative<CompiledValue>(operand.source);
        });
        valueTimeDependent_.push_back(dependent ? 1 : 0);
        for (std::size_t slot = 0; slot < operation.outputCount; ++slot) {
            const auto index = operation.firstOutput.value();
            if (index < outputs.size() && slot < outputs.size() - index)
                outputs[index + slot] = dependent ? 1 : 0;
        }
    }
    const auto parameter = [&](const auto& operand) {
        if (const auto* input = std::get_if<ValueOutputIndex>(&operand.source))
            return input->value() >= outputs.size() || outputs[input->value()] != 0;
        return operand.source.index() != 0;
    };
    const auto input = [&](OperationIndex index) {
        return index.value() >= operationTimeDependent_.size() ||
               operationTimeDependent_[index.value()] != 0;
    };
    operationTimeDependent_.reserve(operations_.size());
    for (const auto& operation : operations_) {
        const bool dependent = std::visit(
            [&](const auto& step) {
                using Step = std::decay_t<decltype(step)>;
                if constexpr (std::is_same_v<Step, CompiledSolid>)
                    return parameter(step.color) || (step.width && parameter(*step.width)) ||
                           (step.height && parameter(*step.height));
                else if constexpr (std::is_same_v<Step, CompiledText>)
                    return parameter(step.color) || parameter(step.size) ||
                           (step.layout && (parameter(step.layout->lineHeight) ||
                                            parameter(step.layout->letterSpacing)));
                else if constexpr (std::is_same_v<Step, CompiledLayerOutput>)
                    return input(step.input) || parameter(step.position) ||
                           parameter(step.anchor) || parameter(step.scale) ||
                           parameter(step.rotation) || parameter(step.opacity);
                else if constexpr (std::is_same_v<Step, CompiledMerge>)
                    return std::ranges::any_of(
                        step.entries, [&](const auto& entry) { return input(entry.input); });
                else
                    return input(step.input);
            },
            operation);
        operationTimeDependent_.push_back(dependent ? 1 : 0);
    }
}
} // namespace bloom::runtime
