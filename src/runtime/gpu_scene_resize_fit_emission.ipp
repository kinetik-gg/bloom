// Included inside bloom::runtime::detail after the shared effect-emission vocabulary.
template <typename Emit, typename Charge, typename ChargeCoverage>
[[nodiscard]] std::optional<GpuSceneLeafFailure>
emitResizeFitCommand(const ResizeFitKernel& kernel, const OperationIndex input,
                     const OperationIndex operation, const GpuSceneEffectEmissionContext& ctx,
                     const CancellationToken& cancellation, Emit&& emit, Charge&& charge,
                     ChargeCoverage&& chargeCoverage, GpuSceneEffectEmission& result) {
    if (cancellation.isCancellationRequested())
        return fail(PreparedGpuSceneDiagnosticCode::Cancelled,
                    "Resize/Fit preparation was cancelled");
    const auto inputWindow = (*ctx.outputWindowOf)[input.value()];
    if (!inputWindow) {
        result.identity = true;
        return std::nullopt;
    }
    auto inputCommand = (*ctx.commandForOperation)[input.value()];
    auto inputKey = (*ctx.keyOf)[input.value()];
    if (inputCommand == kInvalidGpuSceneCommand) {
        const auto& chain = (*ctx.vectors)[input.value()];
        if (!chain || chain->source >= ctx.plan->operations().size())
            return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                        "Resize/Fit input cannot be materialized");
        if (const auto error = emitVectorLeafLayer(
                ctx.plan->operations()[chain->source], *ctx.plan, *ctx.resolved, chain->matrix,
                *inputWindow, ctx.fullDisplayWindow, ctx.fullPixelAspect, ctx.hScale, ctx.vScale,
                chain->opacity, false, ctx.allowance, operation, nullptr, ctx.coverageCache,
                cancellation, emit, charge, chargeCoverage, inputCommand, inputKey,
                result.consumedText, result.consumedShape))
            return error;
        result.consumedLeaf = chain->source;
    }
    const auto source = gpuSceneCommandDescriptorOf(*ctx.commands, inputCommand, 0);
    if (!source)
        return fail(PreparedGpuSceneDiagnosticCode::InternalInvariant,
                    "Resize/Fit input descriptor is unavailable");
    const auto sourceDescriptor =
        render::Rgba32fImageDescriptor::create(source->data, source->display, source->pixelAspect);
    if (!sourceDescriptor)
        return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                    "Resize/Fit input descriptor is invalid");
    const auto geometry =
        resolveResizeFitGeometry(kernel, *sourceDescriptor.value(), ctx.hScale, ctx.vScale);
    if (!geometry)
        return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                    "Resize/Fit output geometry is not representable");
    const auto output = geometry->descriptor.dataWindow();
    if (const auto error =
            charge(output.extent().width(), output.extent().height(), sizeof(render::Rgba32f)))
        return error;
    const auto matrix = toGpuAffineMatrix(geometry->matrix, source->data, 1.0, 1.0);
    const auto aspect = geometry->descriptor.pixelAspect();
    const auto key =
        makeGpuSceneAffineSemanticKey(inputKey, source->data, matrix, 1.0F, output, aspect,
                                      std::string{kGpuAffineSpirvSha256}, geometry->descriptor);
    result.command = emit(GpuSceneAffineCommand{.sourceOperation = operation,
                                                .input = inputCommand,
                                                .inputKey = std::move(inputKey),
                                                .sourceWindow = source->data,
                                                .outputWindow = output,
                                                .matrix = matrix,
                                                .opacity = 1.0F,
                                                .pixelAspect = aspect,
                                                .semanticKey = key,
                                                .outputDescriptor = geometry->descriptor});
    result.key = key;
    result.window = output;
    result.local = boundsForWindow(output, ctx.hScale, ctx.vScale);
    result.output = result.local;
    return std::nullopt;
}
