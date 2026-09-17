// The value-graph pass (task S7). A second, independent lowering over the reachable node set,
// producing runtime::CompiledValueOperation rather than runtime::CompiledOperation. Ordinary value
// operations run before the image pass; Layer Bounds readouts are patched to image OperationIndex
// values and run in the post-image pass. The address spaces remain separate: an OperationIndex
// names a step that produces pixels, a ValueOutputIndex names one number.
//
// Everything here reuses the patterns the image pass already established -- the same reachable set,
// the same topological order, the same "resolve an operand, or fail with a scoped diagnostic" shape
// -- so there is one way to lower a node in this file, not two.

// Whether this node belongs to the value pass. The Image Reroute is the one exception: it carries
// pixels, so it is ELIDED in the image pass (its consumers read its input's operation directly),
// exactly as a muted node is, rather than compiled here.
// Task FIX1, item I: there is ONE reroute type and it takes the kind of the link it sits on, so
// "does this reroute carry pixels" is a question about a NODE rather than about a definition. A
// reroute carrying an image is ELIDED in the image pass (its consumers read its input's operation
// directly), exactly as a muted node is; one carrying a number is compiled into the value pass.
[[nodiscard]] bool isImageReroute(const document::NodeId nodeId) const {
    const auto* node = findNode(nodeId);
    if (node == nullptr || !document::isRerouteNodeType(node->typeId)) {
        return false;
    }
    const auto kind = composition_->graph().rerouteKind(nodeId, registry_);
    return kind.has_value() && *kind == runtime::SocketValueKind::Image;
}

[[nodiscard]] bool isValueNode(const document::NodeId nodeId) const {
    const auto* node = findNode(nodeId);
    const auto* definition =
        node == nullptr ? nullptr : registry_.find(node->typeId, node->schemaVersion);
    return definition != nullptr && runtime::isValueLowering(definition->lowering) &&
           !isImageReroute(nodeId);
}

// The kind a socket on `nodeId` actually carries: the reroute's resolved kind where the node is
// one, and the registry's declared kind everywhere else.
[[nodiscard]] runtime::SocketValueKind socketKindOf(const document::NodeId nodeId,
                                                    const runtime::SocketValueKind declared) const {
    const auto* node = findNode(nodeId);
    if (node == nullptr || !document::isRerouteNodeType(node->typeId)) {
        return declared;
    }
    return composition_->graph().rerouteKind(nodeId, registry_).value_or(declared);
}

// Every driver binding on a reachable node, as the (value node, output port) pair it names.
// Collected once so reachability, the topological order and operand resolution all read one list.
struct DriverReference final {
    document::NodeId ownerNodeId;
    document::ParameterId parameterId;
    std::string role;
    document::OutputPortRef source;
};

[[nodiscard]] bool validateBoundsReadoutRule(const std::vector<document::NodeId>& order) {
    std::unordered_set<document::NodeId> postImageNodes;
    const auto isImageOperation = [](const runtime::NodeLoweringKind lowering) {
        switch (lowering) {
        case runtime::NodeLoweringKind::Solid:
        case runtime::NodeLoweringKind::Shape:
        case runtime::NodeLoweringKind::Text:
        case runtime::NodeLoweringKind::CompositionSource:
    case runtime::NodeLoweringKind::ImageSource:
        case runtime::NodeLoweringKind::LayerOutput:
        case runtime::NodeLoweringKind::LayerStack:
        case runtime::NodeLoweringKind::CompositionOutput:
            return true;
        default:
            return false;
        }
    };
    for (const auto nodeId : order) {
        const auto* node = findNode(nodeId);
        const auto definition = definitions_.find(nodeId);
        if (node == nullptr || definition == definitions_.end())
            continue;
        if (definition->second->lowering == runtime::NodeLoweringKind::ValueBoundsReadout)
            postImageNodes.insert(nodeId);
        for (const auto& reference : driverReferences(*node)) {
            if (!postImageNodes.contains(reference.source.nodeId))
                continue;
            if (isValueNode(nodeId)) {
                postImageNodes.insert(nodeId);
                break;
            }
            if (isImageOperation(definition->second->lowering)) {
                auto diagnosticSubject = subject(nodeId, "parameter." + reference.role);
                diagnosticSubject.parameterId = reference.parameterId;
                addFailure(
                    runtime::CompileDiagnosticCode::BoundsReadoutDrivesImageOperation,
                    std::move(diagnosticSubject),
                    "Layer Bounds cannot drive an image operation",
                    "A Layer Bounds readout and every value node downstream of it may only feed "
                    "value readback, another readout, or a value-graph output.");
                return false;
            }
        }
    }
    return !hasFailure_;
}

[[nodiscard]] std::vector<DriverReference>
driverReferences(const document::NodeRecord& node) const {
    std::vector<DriverReference> references;
    for (const auto& binding : node.parameters) {
        const auto* parameter = findParameter(binding.parameterId);
        const auto* driver = parameter == nullptr
                                 ? nullptr
                                 : std::get_if<document::DriverBindingSource>(&parameter->source);
        if (driver == nullptr) {
            continue;
        }
        references.push_back({node.id, binding.parameterId, binding.role,
                              document::OutputPortRef{driver->sourceNodeId, driver->outputPort}});
    }
    return references;
}

// A document constant as a lowered value. RationalTime is the one alternative with no lowered form:
// a rational is how Bloom names an INSTANT, and a value graph carries quantities -- which is also
// why a Time node's seconds output is a Scalar rather than a rational.
[[nodiscard]] static std::optional<runtime::CompiledValue>
compiledValueFrom(const document::ParameterValue& value) {
    return std::visit(
        [](const auto& held) -> std::optional<runtime::CompiledValue> {
            using Held = std::decay_t<decltype(held)>;
            if constexpr (std::is_same_v<Held, core::RationalTime> || std::is_same_v<Held, document::PathValue>) {
                return std::nullopt;
            } else {
                return runtime::CompiledValue{held};
            }
        },
        value);
}

[[nodiscard]] static std::optional<runtime::ValuePromotion>
valuePromotionFor(const runtime::SocketValueKind source,
                  const runtime::SocketValueKind destination) noexcept {
    using runtime::SocketValueKind;
    if (source == SocketValueKind::Integer && destination == SocketValueKind::Scalar) {
        return runtime::ValuePromotion::IntegerToScalar;
    }
    if (source == SocketValueKind::Boolean && destination == SocketValueKind::Integer) {
        return runtime::ValuePromotion::BooleanToInteger;
    }
    if (source == SocketValueKind::Boolean && destination == SocketValueKind::Scalar) {
        return runtime::ValuePromotion::BooleanToScalar;
    }
    if (source == SocketValueKind::Scalar && destination == SocketValueKind::Vector2) {
        return runtime::ValuePromotion::ScalarToVector2;
    }
    if (source == SocketValueKind::Scalar && destination == SocketValueKind::Vector3) {
        return runtime::ValuePromotion::ScalarToVector3;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<runtime::SocketValueKind>
outputKindOf(const document::OutputPortRef& output) const {
    const auto* node = findNode(output.nodeId);
    // A reroute answers with the kind of the link it sits on, not with its declared placeholder.
    if (node != nullptr && document::isRerouteNodeType(node->typeId)) {
        return composition_->graph().rerouteKind(output.nodeId, registry_);
    }
    const auto* definition =
        node == nullptr ? nullptr : registry_.find(node->typeId, node->schemaVersion);
    if (definition == nullptr) {
        return std::nullopt;
    }
    const auto match =
        std::ranges::find(definition->outputs, output.port, &runtime::OutputPortDefinition::name);
    return match == definition->outputs.end() ? std::nullopt : std::optional(match->valueKind);
}

// Resolve a value-graph output to the index it was compiled at, inserting one explicit promotion
// operation when the source kind is not already the destination kind. The promotion is its own step
// rather than a conversion folded into whoever reads the value: a widening that appears in the plan
// is a widening that can be read back and diagnosed.
[[nodiscard]] std::optional<runtime::ValueOutputIndex>
resolveValueOutput(const document::OutputPortRef& source,
                   const runtime::SocketValueKind destinationKind) {
    const auto produced = valueOutputs_.find(ValueOutputKey{source.nodeId, source.port});
    if (produced == valueOutputs_.end()) {
        return std::nullopt;
    }
    const auto sourceKind = outputKindOf(source);
    if (!sourceKind.has_value()) {
        return std::nullopt;
    }
    if (*sourceKind == destinationKind) {
        return produced->second;
    }
    const auto promotion = valuePromotionFor(*sourceKind, destinationKind);
    if (!promotion.has_value()) {
        return std::nullopt;
    }
    const auto index = runtime::ValueOutputIndex::fromRaw(valueOutputCount_);
    ++valueOutputCount_;
    const bool requiresPostImage = produced->second.value() < postImageValueOutputs_.size() &&
                                   postImageValueOutputs_[produced->second.value()] != 0;
    valueOperations_.push_back(runtime::CompiledValueOperation{
        {},
        index,
        1,
        runtime::CompiledValuePromotion{*promotion,
                                        runtime::CompiledValueOperand{{}, produced->second}},
        requiresPostImage});
    postImageValueOutputs_.push_back(requiresPostImage ? 1 : 0);
    return index;
}

// One operand of a value node: the value-graph output its parameter is driven by if there is one,
// otherwise the authored constant behind it. This is the single place the "unlinked means the
// widget's value, linked means the wire's value" rule is implemented for evaluation, matching the
// editor's own rule exactly. A curve-backed authored value, as the index of its compiled curve
// (task FIX1, item G). Answers nothing when the parameter is not on a curve, or when its kind has
// no curve table -- the caller then falls through to the constant it must be.
[[nodiscard]] std::optional<runtime::CompiledValueOperand>
curveOperand(const document::ParameterRecord& parameter) const {
    const auto* source = std::get_if<document::AnimationCurveSource>(&parameter.source);
    if (source == nullptr) {
        return std::nullopt;
    }
    if (const auto scalar = scalarCurveIndices_.find(source->curveId);
        scalar != scalarCurveIndices_.end()) {
        return runtime::CompiledValueOperand{parameter.id, scalar->second};
    }
    if (const auto vector = vec2CurveIndices_.find(source->curveId);
        vector != vec2CurveIndices_.end()) {
        return runtime::CompiledValueOperand{parameter.id, vector->second};
    }
    if (const auto vector = vec3CurveIndices_.find(source->curveId);
        vector != vec3CurveIndices_.end()) {
        return runtime::CompiledValueOperand{parameter.id, vector->second};
    }
    if (const auto color = color4CurveIndices_.find(source->curveId);
        color != color4CurveIndices_.end()) {
        return runtime::CompiledValueOperand{parameter.id, color->second};
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<runtime::CompiledValueOperand>
valueOperand(const document::NodeRecord& node, const runtime::NodeDefinition& definition,
             const std::string_view port) {
    const auto declared =
        std::ranges::find(definition.inputs, port, &runtime::InputPortDefinition::name);
    if (declared == definition.inputs.end()) {
        return std::nullopt;
    }
    const auto* binding = runtime::detail::findParameterBinding(node, port);
    if (binding == nullptr) {
        // A socket with no parameter behind it is a Reroute's pass-through, and only a Reroute's:
        // it carries someone else's value, so it IS reached by an ordinary edge.
        const auto* edge = fixedInputEdge(node.id, port);
        if (edge == nullptr) {
            return std::nullopt;
        }
        const auto resolved =
            resolveValueOutput(edge->source, socketKindOf(node.id, declared->valueKind));
        return resolved.has_value() ? std::optional(runtime::CompiledValueOperand{
                                          document::ParameterId{}, *resolved})
                                    : std::nullopt;
    }
    const auto* parameter = findParameter(binding->parameterId);
    if (parameter == nullptr) {
        return std::nullopt;
    }
    // An operand socket is LINKED when its parameter carries a driver binding, not when an edge
    // terminates on it. A parameter and the socket that can fill it are one authored value, so
    // there is one durable record of where that value comes from -- the parameter's own source --
    // rather than an edge and a binding that could disagree. CanonicalGraph::validate() refuses an
    // edge into a parameter socket for exactly that reason.
    if (const auto driven = driverOutput(*parameter, declared->valueKind)) {
        return runtime::CompiledValueOperand{binding->parameterId, *driven};
    }
    if (auto curve = curveOperand(*parameter)) {
        return curve;
    }
    const auto* constant = std::get_if<document::ConstantValueSource>(&parameter->source);
    if (constant == nullptr) {
        return std::nullopt;
    }
    auto value = compiledValueFrom(constant->value);
    if (!value.has_value()) {
        return std::nullopt;
    }
    return runtime::CompiledValueOperand{binding->parameterId, *std::move(value)};
}

// An inline selector's authored integer, as the closed enumeration it names. A selector is never
// socket-linkable, so it is always a constant here -- and a stored integer naming no implemented
// operation was already refused by document validation, which is why failing to map one is a
// topology failure rather than a fallback.
template <typename Enumeration, typename Mapper>
[[nodiscard]] std::optional<Enumeration>
selector(const document::NodeRecord& node, const std::string_view role, Mapper&& mapper) const {
    const auto* stored =
        parameterConstant<std::int64_t>(runtime::detail::findParameterBinding(node, role));
    return stored == nullptr ? std::nullopt : std::forward<Mapper>(mapper)(*stored);
}

[[nodiscard]] std::optional<bool> booleanSelector(const document::NodeRecord& node,
                                                  const std::string_view role) const {
    const auto* stored = parameterConstant<bool>(runtime::detail::findParameterBinding(node, role));
    return stored == nullptr ? std::nullopt : std::optional(*stored);
}

[[nodiscard]] std::optional<runtime::CompiledValueKernel>
lowerValueKernel(const document::NodeRecord& node, const runtime::NodeDefinition& definition) {
    using namespace document;
    const auto operandOf = [&](const std::string_view port) {
        return valueOperand(node, definition, port);
    };
    switch (definition.lowering) {
    case runtime::NodeLoweringKind::ValueConstant: {
        const auto* binding = runtime::detail::findParameterBinding(node, kValueParameterRole);
        const auto* parameter = binding == nullptr ? nullptr : findParameter(binding->parameterId);
        if (parameter == nullptr) {
            return std::nullopt;
        }
        // Task FIX1, item G: a literal whose value is on a curve lowers to its curve index, and the
        // evaluator samples it at the frame being rendered -- so a Scalar node keyed 0 to 1 over
        // ten frames drives a layer's opacity per frame, exactly as a Time node already could.
        if (auto curve = curveOperand(*parameter)) {
            return runtime::CompiledValueKernel{runtime::CompiledValuePassthrough{*curve}};
        }
        const auto* constant = std::get_if<ConstantValueSource>(&parameter->source);
        if (constant == nullptr) {
            return std::nullopt;
        }
        auto value = compiledValueFrom(constant->value);
        if (!value.has_value()) {
            return std::nullopt;
        }
        return runtime::CompiledValueKernel{runtime::CompiledValuePassthrough{
            runtime::CompiledValueOperand{binding->parameterId, *std::move(value)}}};
    }
    case runtime::NodeLoweringKind::ValueTime:
        return runtime::CompiledValueKernel{runtime::CompiledValueTime{}};
    case runtime::NodeLoweringKind::ValueReroute: {
        auto value = operandOf(kValuePortName);
        if (!value.has_value()) {
            return std::nullopt;
        }
        return runtime::CompiledValueKernel{runtime::CompiledValuePassthrough{*std::move(value)}};
    }
    case runtime::NodeLoweringKind::ValueBoundsReadout: {
        const auto* edge = fixedInputEdge(node.id, kLayerBoundsImagePortName);
        if (edge == nullptr) {
            return std::nullopt;
        }
        pendingBoundsReadouts_[node.id] = edge->source.nodeId;
        return runtime::CompiledValueKernel{runtime::CompiledBoundsReadout{
            runtime::OperationIndex::fromRaw(0)}};
    }
    case runtime::NodeLoweringKind::ValueScalarMath: {
        const auto operation = selector<core::primitives::ScalarPrimitive>(
            node, kOperationParameterRole, scalarOperationFromStoredValue);
        const auto clamp = booleanSelector(node, kClampResultParameterRole);
        if (!operation.has_value() || !clamp.has_value()) {
            return std::nullopt;
        }
        // Exactly the chosen operation's own arity: the node declares five operand sockets so one
        // definition serves all 24 operations, and lowering only the ones this operation reads is
        // what keeps the unused sockets from reaching a kernel that would reject the call.
        const auto arity = scalarOperationOperandCount(*operation);
        std::vector<runtime::CompiledValueOperand> operands;
        operands.reserve(arity);
        for (std::uint8_t index = 0; index < arity; ++index) {
            auto value = operandOf(kScalarOperandPortNames[index]);
            if (!value.has_value()) {
                return std::nullopt;
            }
            operands.push_back(*std::move(value));
        }
        return runtime::CompiledValueKernel{
            runtime::CompiledValueScalarMath{*operation, *clamp, std::move(operands)}};
    }
    case runtime::NodeLoweringKind::ValueVectorMath: {
        const auto operation = selector<VectorOperation>(node, kOperationParameterRole,
                                                         vectorOperationFromStoredValue);
        auto left = operandOf(kFirstOperandPortName);
        auto right = operandOf(kSecondOperandPortName);
        auto factor = operandOf(kScaleFactorPortName);
        const auto components = vectorComponentCount(definition.outputs.front().valueKind);
        if (!operation.has_value() || !left.has_value() || !right.has_value() ||
            !factor.has_value() || components == 0 ||
            !isVectorOperationValidAt(*operation, components)) {
            return std::nullopt;
        }
        return runtime::CompiledValueKernel{runtime::CompiledValueVectorMath{
            *operation, components, *std::move(left), *std::move(right), *std::move(factor)}};
    }
    case runtime::NodeLoweringKind::ValueVectorReduce: {
        const auto reduction = selector<VectorReduction>(node, kOperationParameterRole,
                                                         vectorReductionFromStoredValue);
        auto left = operandOf(kFirstOperandPortName);
        auto right = operandOf(kSecondOperandPortName);
        const auto components = vectorComponentCount(definition.inputs.front().valueKind);
        if (!reduction.has_value() || !left.has_value() || !right.has_value() || components == 0) {
            return std::nullopt;
        }
        return runtime::CompiledValueKernel{runtime::CompiledValueVectorReduce{
            *reduction, components, *std::move(left), *std::move(right)}};
    }
    case runtime::NodeLoweringKind::ValueMapRange: {
        const auto interpolation = selector<RangeInterpolation>(node, kInterpolationParameterRole,
                                                                rangeInterpolationFromStoredValue);
        const auto clamp = booleanSelector(node, kClampResultParameterRole);
        auto value = operandOf(kMapRangeValuePortName);
        auto fromMinimum = operandOf(kMapRangeFromMinimumPortName);
        auto fromMaximum = operandOf(kMapRangeFromMaximumPortName);
        auto toMinimum = operandOf(kMapRangeToMinimumPortName);
        auto toMaximum = operandOf(kMapRangeToMaximumPortName);
        if (!interpolation.has_value() || !clamp.has_value() || !value.has_value() ||
            !fromMinimum.has_value() || !fromMaximum.has_value() || !toMinimum.has_value() ||
            !toMaximum.has_value()) {
            return std::nullopt;
        }
        return runtime::CompiledValueKernel{runtime::CompiledValueMapRange{
            *interpolation, *clamp, *std::move(value), *std::move(fromMinimum),
            *std::move(fromMaximum), *std::move(toMinimum), *std::move(toMaximum)}};
    }
    case runtime::NodeLoweringKind::ValueClamp: {
        auto value = operandOf(kClampValuePortName);
        auto minimum = operandOf(kClampMinimumPortName);
        auto maximum = operandOf(kClampMaximumPortName);
        if (!value.has_value() || !minimum.has_value() || !maximum.has_value()) {
            return std::nullopt;
        }
        return runtime::CompiledValueKernel{runtime::CompiledValueClamp{
            *std::move(value), *std::move(minimum), *std::move(maximum)}};
    }
    case runtime::NodeLoweringKind::ValueMix:
    case runtime::NodeLoweringKind::ValueColorMix: {
        auto factor = operandOf(kMixFactorPortName);
        auto start = operandOf(kFirstOperandPortName);
        auto end = operandOf(kSecondOperandPortName);
        if (!factor.has_value() || !start.has_value() || !end.has_value()) {
            return std::nullopt;
        }
        const bool color = definition.lowering == runtime::NodeLoweringKind::ValueColorMix;
        return runtime::CompiledValueKernel{runtime::CompiledValueMix{
            color, *std::move(factor), *std::move(start), *std::move(end)}};
    }
    case runtime::NodeLoweringKind::ValueCompare: {
        const auto operation = selector<CompareOperation>(node, kOperationParameterRole,
                                                          compareOperationFromStoredValue);
        auto left = operandOf(kFirstOperandPortName);
        auto right = operandOf(kSecondOperandPortName);
        auto epsilon = operandOf(kEpsilonParameterRole);
        if (!operation.has_value() || !left.has_value() || !right.has_value() ||
            !epsilon.has_value()) {
            return std::nullopt;
        }
        return runtime::CompiledValueKernel{runtime::CompiledValueCompare{
            *operation, *std::move(left), *std::move(right), *std::move(epsilon)}};
    }
    case runtime::NodeLoweringKind::ValueSwitch: {
        auto condition = operandOf(kSwitchConditionPortName);
        auto ifFalse = operandOf(kSwitchFalsePortName);
        auto ifTrue = operandOf(kSwitchTruePortName);
        if (!condition.has_value() || !ifFalse.has_value() || !ifTrue.has_value()) {
            return std::nullopt;
        }
        return runtime::CompiledValueKernel{runtime::CompiledValueSwitch{
            *std::move(condition), *std::move(ifFalse), *std::move(ifTrue)}};
    }
    case runtime::NodeLoweringKind::ValueSeparate: {
        const bool color = definition.inputs.front().valueKind == runtime::SocketValueKind::Color;
        auto value = operandOf(definition.inputs.front().name);
        if (!value.has_value()) {
            return std::nullopt;
        }
        return runtime::CompiledValueKernel{runtime::CompiledValueSeparate{
            *std::move(value), static_cast<std::uint8_t>(definition.outputs.size()), color}};
    }
    case runtime::NodeLoweringKind::ValueCombine: {
        const bool color = definition.outputs.front().valueKind == runtime::SocketValueKind::Color;
        std::vector<runtime::CompiledValueOperand> components;
        components.reserve(definition.inputs.size());
        for (const auto& input : definition.inputs) {
            auto value = operandOf(input.name);
            if (!value.has_value()) {
                return std::nullopt;
            }
            components.push_back(*std::move(value));
        }
        return runtime::CompiledValueKernel{
            runtime::CompiledValueCombine{std::move(components), color}};
    }
    case runtime::NodeLoweringKind::ValueRandom: {
        auto seed = operandOf(kRandomSeedPortName);
        auto minimum = operandOf(kRandomMinimumPortName);
        auto maximum = operandOf(kRandomMaximumPortName);
        if (!seed.has_value() || !minimum.has_value() || !maximum.has_value()) {
            return std::nullopt;
        }
        return runtime::CompiledValueKernel{runtime::CompiledValueRandom{
            *std::move(seed), *std::move(minimum), *std::move(maximum)}};
    }
    case runtime::NodeLoweringKind::ValueUtility: {
        // Wholly table-driven: the node's descriptor names its operands and its selectors, and this
        // reads them in that order. There is no per-node branch here at all, which is the point --
        // a new library node is a descriptor plus a kernel, never a third edit in the compiler.
        const auto* descriptor = findValueUtilityDescriptor(node.typeId);
        if (descriptor == nullptr) {
            return std::nullopt;
        }
        // A composition readout is a CONSTANT in this plan. The plan is compiled from one document
        // snapshot, and changing a composition's format or duration is a document edit that
        // recompiles it, so baking the value here costs nothing per frame and makes it impossible
        // for the graph and the settings to disagree.
        if (isCompositionConstantReadout(descriptor->kernel)) {
            return runtime::CompiledValueKernel{runtime::CompiledValuePassthrough{
                runtime::CompiledValueOperand{{}, compositionReadout(descriptor->kernel)}}};
        }
        std::vector<runtime::CompiledValueOperand> operands;
        operands.reserve(descriptor->operands.size());
        for (const auto& declared : descriptor->operands) {
            auto value = operandOf(declared.role);
            if (!value.has_value()) {
                return std::nullopt;
            }
            operands.push_back(*std::move(value));
        }
        std::vector<std::int64_t> selectors;
        selectors.reserve(descriptor->selectors.size());
        for (const auto& declared : descriptor->selectors) {
            const auto* stored = parameterConstant<std::int64_t>(
                runtime::detail::findParameterBinding(node, declared.role));
            if (stored == nullptr) {
                return std::nullopt;
            }
            selectors.push_back(*stored);
        }
        return runtime::CompiledValueKernel{runtime::CompiledValueUtility{
            descriptor->kernel, std::move(operands), std::move(selectors)}};
    }
    case runtime::NodeLoweringKind::Shape:
    case runtime::NodeLoweringKind::Solid:
    case runtime::NodeLoweringKind::CompositionSource:
    case runtime::NodeLoweringKind::ImageSource:
    case runtime::NodeLoweringKind::AudioSource:
    case runtime::NodeLoweringKind::Text:
    case runtime::NodeLoweringKind::LayerOutput:
    case runtime::NodeLoweringKind::LayerStack:
    case runtime::NodeLoweringKind::CompositionOutput:
    case runtime::NodeLoweringKind::Unsupported:
        break;
    }
    return std::nullopt;
}

// One composition readout's baked value. The format's width and height are its PIXEL extent, and
// the duration is in seconds so that it can be compared with a Time node's own seconds output
// without a conversion.
[[nodiscard]] runtime::CompiledValue
compositionReadout(const document::ValueUtilityKernel kernel) const {
    const auto format = composition_->format();
    switch (kernel) {
    case document::ValueUtilityKernel::FrameRate:
        return static_cast<double>(format.frameRate().numerator()) /
               static_cast<double>(format.frameRate().denominator());
    case document::ValueUtilityKernel::CompositionDuration:
        return composition_->duration().toSeconds();
    case document::ValueUtilityKernel::CompositionSize:
        return document::Vec2d{static_cast<double>(format.width()),
                               static_cast<double>(format.height())};
    default:
        break;
    }
    return 0.0;
}

[[nodiscard]] static std::uint8_t
vectorComponentCount(const runtime::SocketValueKind kind) noexcept {
    if (kind == runtime::SocketValueKind::Vector2) {
        return 2;
    }
    if (kind == runtime::SocketValueKind::Vector3) {
        return 3;
    }
    return 0;
}

// The pass itself: one sweep in the image pass's own topological order, so an operand can only ever
// name an output that has already been compiled. Reachability pruning comes for free -- the order
// only contains nodes the composition output actually depends on, through edges or through drivers.
[[nodiscard]] bool compileValueGraph(const std::vector<document::NodeId>& order) {
    for (const auto nodeId : order) {
        if (cancelled()) {
            return false;
        }
        const auto* node = findNode(nodeId);
        const auto definition = definitions_.find(nodeId);
        if (node == nullptr || definition == definitions_.end() || !isValueNode(nodeId)) {
            continue;
        }
        auto kernel = lowerValueKernel(*node, *definition->second);
        if (!kernel.has_value()) {
            addTopologyFailure(nodeId, "A reachable value node could not be lowered.");
            return false;
        }
        const auto outputCount = definition->second->outputs.size();
        const auto first = runtime::ValueOutputIndex::fromRaw(valueOutputCount_);
        bool requiresPostImage =
            std::holds_alternative<runtime::CompiledBoundsReadout>(*kernel);
        runtime::forEachValueOperand(*kernel, [&](const runtime::CompiledValueOperand& operand) {
            if (const auto* output = std::get_if<runtime::ValueOutputIndex>(&operand.source);
                output != nullptr && output->value() < postImageValueOutputs_.size())
                requiresPostImage = requiresPostImage || postImageValueOutputs_[output->value()] != 0;
        });
        valueOperations_.push_back(runtime::CompiledValueOperation{
            nodeId, first, static_cast<std::uint8_t>(outputCount), *std::move(kernel),
            requiresPostImage});
        for (std::size_t slot = 0; slot < outputCount; ++slot) {
            valueOutputs_.emplace(ValueOutputKey{nodeId, definition->second->outputs[slot].name},
                                  runtime::ValueOutputIndex::fromRaw(valueOutputCount_ + slot));
        }
        postImageValueOutputs_.resize(valueOutputCount_ + outputCount, 0);
        if (requiresPostImage)
            std::fill(postImageValueOutputs_.begin() +
                          static_cast<std::ptrdiff_t>(valueOutputCount_),
                      postImageValueOutputs_.end(), 1);
        valueOutputCount_ += outputCount;
    }
    return true;
}

[[nodiscard]] bool resolveBoundsReadouts(
    const std::unordered_map<document::NodeId, runtime::OperationIndex>& indices) {
    for (auto& operation : valueOperations_) {
        auto* readout = std::get_if<runtime::CompiledBoundsReadout>(&operation.kernel);
        if (readout == nullptr)
            continue;
        const auto source = pendingBoundsReadouts_.find(operation.sourceNodeId);
        if (source == pendingBoundsReadouts_.end()) {
            addTopologyFailure(operation.sourceNodeId,
                               "Layer Bounds did not retain its connected image source.");
            return false;
        }
        const auto image = indices.find(source->second);
        if (image == indices.end()) {
            addTopologyFailure(operation.sourceNodeId,
                               "Layer Bounds image input was not lowered into the image plan.");
            return false;
        }
        readout->operationIndex = image->second;
    }
    return true;
}

// A driven parameter's compiled source. The driver names a value node's output, and the parameter's
// own socket kind is what that output has to reach -- through a promotion if the kinds differ,
// exactly as an edge into the same socket would.
[[nodiscard]] std::optional<runtime::ValueOutputIndex>
driverOutput(const document::ParameterRecord& parameter,
             const runtime::SocketValueKind destinationKind) {
    const auto* driver = std::get_if<document::DriverBindingSource>(&parameter.source);
    if (driver == nullptr) {
        return std::nullopt;
    }
    return resolveValueOutput(document::OutputPortRef{driver->sourceNodeId, driver->outputPort},
                              destinationKind);
}
