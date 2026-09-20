[[nodiscard]] std::shared_ptr<const bloom::runtime::CompiledCompositionPlan>
makePlan(const std::uint32_t w, const std::uint32_t h) {
    const auto compositionFormat = format(w, h);
    std::vector<bloom::runtime::CompiledOperation> ops;
    ops.emplace_back(bloom::runtime::CompiledSolid{
        kSolid,
        {kColorP, Color4d{1.0, 0.25, 0.5, 1.0}},
        {bloom::document::ParameterId::fromRaw(kSolid.value() * 100 + 1000),
         static_cast<double>(compositionFormat.width())},
        {bloom::document::ParameterId::fromRaw(kSolid.value() * 100 + 1001),
         static_cast<double>(compositionFormat.height())}});
    ops.emplace_back(bloom::runtime::CompiledLayerOutput{
        kLayerNode, kLayer, bloom::runtime::OperationIndex::fromRaw(0),
        bloom::runtime::CompiledVec2Parameter{kPosP, bloom::document::Vec2d{2.0, 1.0}},
        bloom::runtime::CompiledVec2Parameter{kAnchorP, bloom::document::kDefaultAnchor},
        bloom::runtime::CompiledVec2Parameter{kScaleP, bloom::document::kDefaultScale},
        bloom::runtime::CompiledScalarParameter{kRotP, bloom::document::kDefaultRotationDegrees},
        bloom::runtime::CompiledScalarParameter{kOpacityP, 1.0}, kBlendP,
        bloom::core::kDefaultBlendMode});
    ops.emplace_back(bloom::runtime::CompiledMerge{
        kStack, {{kSlot, kLayer, bloom::runtime::OperationIndex::fromRaw(1)}}});
    ops.emplace_back(bloom::runtime::CompiledCompositionOutput{
        kOutput, bloom::runtime::OperationIndex::fromRaw(2)});
    return std::make_shared<const bloom::runtime::CompiledCompositionPlan>(
        bloom::runtime::CompiledCompositionPlanDefinition{
            bloom::document::Revision::fromRaw(7), kProjectId, kCompositionId, compositionFormat,
            std::move(ops), bloom::runtime::OperationIndex::fromRaw(3)});
}

[[nodiscard]] std::shared_ptr<const bloom::color::PreparedCpuDisplayProcessorHandle>
makeProcessor() {
    auto resolution = bloom::color::resolveBloomNeutralV1BuiltIn(
        bloom::color::OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kBloomNeutralV1ConfigUri,
        bloom::color::kBloomNeutralV1ConfigDigest);
    if (!resolution.ready()) {
        return nullptr;
    }
    auto resolved = std::move(resolution).takeResolved();
    if (!resolved.has_value()) {
        return nullptr;
    }
    auto built = bloom::color::buildBloomNeutralCpuDisplayProcessor(*resolved);
    if (!built) {
        return nullptr;
    }
    auto handle = std::move(built).takeHandle();
    if (!handle.has_value()) {
        return nullptr;
    }
    return std::make_shared<const bloom::color::PreparedCpuDisplayProcessorHandle>(
        std::move(*handle));
}

[[nodiscard]] PreviewRequestIdentity
makeIdentity(const bloom::runtime::CompiledCompositionPlan& plan, const std::uint64_t generation) {
    return PreviewRequestIdentity{.projectId = plan.projectId(),
                                  .compositionId = plan.compositionId(),
                                  .sourceRevision = plan.sourceRevision(),
                                  .requestGeneration = generation,
                                  .time = RationalTime::fromInteger(0),
                                  .output = bloom::runtime::PreviewOutput::Composition,
                                  .resolution = bloom::runtime::CompositionFormatResolution{},
                                  .quality = EvaluationQuality::Reference,
                                  .colorIntent = EvaluationColorIntent::LinearRec709Scene,
                                  .resolutionPolicy = PreviewResolutionPolicy::Auto,
                                  .viewAdjust = bloom::runtime::ViewAdjust{},
                                  .displayName = {},
                                  .viewName = {},
                                  .showLook = true};
}

[[nodiscard]] TaskDiagnostic failDiag(const char* code) {
    return TaskDiagnostic{.code = code,
                          .severity = bloom::runtime::DiagnosticSeverity::Error,
                          .summary = "resident service acceptance harness failure.",
                          .detail = {},
                          .suggestedAction = {}};
}

// The injected GPU-scene stage function: a real CpuGpuSceneBuilder over the real plan. Generation 2
// simulates an unsupported GPU-subset graph (text/rotation/non-normal), generation 3 is gated so a
// cancellation can be delivered deterministically before it returns.
[[nodiscard]] PreviewGpuSceneStageFunction residentStageFunction(
    std::shared_ptr<const bloom::runtime::CompiledCompositionPlan> plan,
    std::shared_ptr<const bloom::color::PreparedCpuDisplayProcessorHandle> processor,
    std::shared_ptr<CpuGpuSceneBuilder> builder, std::shared_ptr<std::atomic<bool>> gate_entered,
    std::shared_ptr<std::atomic<bool>> gate_release) {
    return
        [plan = std::move(plan), processor = std::move(processor), builder = std::move(builder),
         gate_entered = std::move(gate_entered), gate_release = std::move(gate_release)](
            const bloom::document::Snapshot&, const PreviewRequestIdentity& identity,
            const std::size_t limit, const std::vector<bloom::runtime::SnapshotParameterOverride>&,
            TaskContext& context) -> TaskResult<PreviewGpuSceneStageOutcomeHandle> {
            using R = TaskResult<PreviewGpuSceneStageOutcomeHandle>;
            if (context.isCancellationRequested()) {
                return R::cancelled();
            }
            if (identity.requestGeneration == kGenerationUnsupportedSubset) {
                auto outcome = std::make_shared<const PreviewGpuSceneStageOutcome>(
                    PreviewGpuSceneStageOutcome{PreviewGpuSceneStageStatus::UnsupportedGpuSubset,
                                                nullptr,
                                                {failDiag("harness.unsupported-gpu-subset")}});
                return R::succeeded(std::move(outcome));
            }
            if (identity.requestGeneration == kGenerationGated) {
                gate_entered->store(true, std::memory_order_release);
                while (!gate_release->load(std::memory_order_acquire)) {
                    if (context.isCancellationRequested()) {
                        return R::cancelled();
                    }
                    std::this_thread::sleep_for(1ms);
                }
            }
            if (context.isCancellationRequested()) {
                return R::cancelled();
            }
            EvaluationRequest request{.time = identity.time,
                                      .output = plan->output(),
                                      .resolution = bloom::runtime::CompositionFormatResolution{},
                                      .quality = EvaluationQuality::Reference,
                                      .colorIntent = EvaluationColorIntent::LinearRec709Scene,
                                      .pixelStorageByteLimit = limit};
            auto built = builder->build(plan, request, context.cancellation());
            if (!built.hasValue() || built.scene == nullptr) {
                auto outcome = std::make_shared<const PreviewGpuSceneStageOutcome>(
                    PreviewGpuSceneStageOutcome{PreviewGpuSceneStageStatus::UnsupportedGpuSubset,
                                                nullptr,
                                                {failDiag("harness.scene-build-refused")}});
                return R::succeeded(std::move(outcome));
            }
            auto stage = std::make_shared<const PreviewGpuSceneStage>(
                identity, built.scene, processor, limit, std::vector<TaskDiagnostic>{});
            auto outcome =
                std::make_shared<const PreviewGpuSceneStageOutcome>(PreviewGpuSceneStageOutcome{
                    PreviewGpuSceneStageStatus::Prepared, std::move(stage), {}});
            return R::succeeded(std::move(outcome));
        };
}

// The full original CPU path used by the resident fallback: a real evaluation into a
// PreviewCpuStage.
[[nodiscard]] PreviewCpuStageFunction residentCpuStageFunction(
    std::shared_ptr<const bloom::runtime::CompiledCompositionPlan> plan,
    std::shared_ptr<const bloom::color::PreparedCpuDisplayProcessorHandle> processor,
    std::shared_ptr<CpuCompositionEvaluator> evaluator) {
    return [plan = std::move(plan), processor = std::move(processor),
            evaluator = std::move(evaluator)](
               const bloom::document::Snapshot&, const PreviewRequestIdentity& identity,
               const std::size_t limit,
               const std::vector<bloom::runtime::SnapshotParameterOverride>&,
               TaskContext& context) -> TaskResult<PreviewCpuStageOutcomeHandle> {
        using R = TaskResult<PreviewCpuStageOutcomeHandle>;
        if (context.isCancellationRequested()) {
            return R::cancelled();
        }
        auto result =
            evaluator->evaluate(plan,
                                {.time = identity.time,
                                 .output = plan->output(),
                                 .resolution = bloom::runtime::CompositionFormatResolution{},
                                 .quality = EvaluationQuality::Reference,
                                 .colorIntent = EvaluationColorIntent::LinearRec709Scene,
                                 .pixelStorageByteLimit = limit},
                                context.cancellation());
        if (result.status() != bloom::runtime::EvaluationStatus::Evaluated ||
            result.frame() == nullptr) {
            return R::cancelled();
        }
        auto stage = std::make_shared<const PreviewCpuStage>(identity, result.frame(), processor,
                                                             limit, std::vector<TaskDiagnostic>{});
        return R::succeeded(std::make_shared<const PreviewCpuStageOutcome>(
            PreviewCpuStageOutcome{PreviewCpuStageStatus::Evaluated, std::move(stage), {}}));
    };
}
