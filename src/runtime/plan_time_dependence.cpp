#include <algorithm>
#include <bloom/runtime/compiled_plan.hpp>
#include <optional>
#include <type_traits>

namespace bloom::runtime {
void CompiledCompositionPlan::analyzeTimeDependence() {
    std::vector<std::uint8_t> outputs(valueOutputCount_, 1);
    valueTimeDependent_.reserve(valueOperations_.size());
    for (const auto& operation : valueOperations_) {
        // A Time node, and the one readout whose value is the FRAME rather than the composition.
        // The other three readouts were baked into constants by the compiler and never reach here.
        const auto* utility = std::get_if<CompiledValueUtility>(&operation.kernel);
        bool dependent =
            std::holds_alternative<CompiledValueTime>(operation.kernel) ||
            (utility != nullptr && utility->operation == document::ValueUtilityKernel::FrameNumber);
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
    // Task DRIVE-1. A parameter whose only source is a driver: a String, an Integer or a Boolean
    // has no curve alternative, so "does this vary with time" is entirely the question of whether
    // the value output behind it does.
    const auto driven = [&](const std::optional<ValueOutputIndex>& output) {
        return output.has_value() &&
               (output->value() >= outputs.size() || outputs[output->value()] != 0);
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
                    return parameter(step.color) || parameter(step.width) || parameter(step.height);
                else if constexpr (std::is_same_v<Step, CompiledImageSource>)
                    return step.asset && step.asset->kind == document::AssetKind::Sequence;
                else if constexpr (std::is_same_v<Step, CompiledText>)
                    return parameter(step.color) || parameter(step.size) ||
                           driven(step.drivenContent) ||
                           (parameter(step.layout.lineHeight) ||
                            parameter(step.layout.letterSpacing) ||
                            driven(step.layout.drivenAlignment));
                // A driven blend mode is consumed by the Layer Stack rather than by this step, but
                // it is a property of the LAYER, so the layer's operation is what varies with time
                // -- and the stack's own dependence already follows its inputs.
                else if constexpr (std::is_same_v<Step, CompiledLayerOutput>)
                    return input(step.input) || (step.parent && input(*step.parent)) ||
                           parameter(step.position) || parameter(step.anchor) ||
                           parameter(step.scale) || parameter(step.rotation) ||
                           parameter(step.opacity) || driven(step.drivenBlendMode);
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
