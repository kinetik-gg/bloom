// Runs the real Solid -> resident display path at the exact trusted geometry and returns the
// published native image. Readback is never called.
[[nodiscard]] std::shared_ptr<const GpuDisplayImage>
produceResidentDisplay(GpuDevice& device, GpuSolid& solid, GpuResidentDisplay& display,
                       const ImageWindow& dataWindow, const ImageWindow& displayWindow,
                       const PixelAspectRatio& pixelAspect, const std::uint64_t budget) {
    const auto pixelResult =
        bloom::render::solidPixelFromStraightLinearRec709Scene(Color4d{1.0, 0.25, 0.0, 1.0});
    if (!pixelResult.hasValue()) {
        std::cerr << "the solid pixel is invalid\n";
        return nullptr;
    }
    const auto solidBegin = solid.begin({.pixel = *pixelResult.value(),
                                         .dataWindow = dataWindow,
                                         .displayWindow = displayWindow,
                                         .pixelAspect = pixelAspect},
                                        budget);
    if (solidBegin.code != bloom::render::GpuSolidDiagnosticCode::None) {
        std::cerr << "solid begin refused: " << solidBegin.message << '\n';
        return nullptr;
    }
    for (int spin = 0; spin < 1'000'000; ++spin) {
        const auto poll = solid.poll();
        if (poll == bloom::render::GpuSolidPollResult::Ready) {
            break;
        }
        if (poll == bloom::render::GpuSolidPollResult::Failure) {
            std::cerr << "solid dispatch failed\n";
            return nullptr;
        }
    }
    GpuImage processImage = solid.takeImage();
    if (!processImage.isValid() || !processImage.isBoundTo(device)) {
        std::cerr << "solid produced no valid resident image\n";
        return nullptr;
    }
    auto sharedProcess = std::make_shared<const GpuImage>(std::move(processImage));

    const auto displayBegin = display.begin(sharedProcess, budget);
    if (displayBegin.code != bloom::render::GpuResidentDisplayDiagnosticCode::None) {
        std::cerr << "resident display begin refused: " << displayBegin.message << '\n';
        return nullptr;
    }
    for (int spin = 0; spin < 1'000'000; ++spin) {
        const auto poll = display.poll();
        if (poll == bloom::render::GpuResidentDisplayPollResult::Ready) {
            break;
        }
        if (poll == bloom::render::GpuResidentDisplayPollResult::Failure) {
            std::cerr << "resident display dispatch failed\n";
            return nullptr;
        }
    }
    GpuDisplayImage image = display.takeImage();
    if (!image.isValid() || !image.isBoundTo(device)) {
        std::cerr << "resident display produced no valid image\n";
        return nullptr;
    }
    return std::make_shared<const GpuDisplayImage>(std::move(image));
}

struct CaseSpec final {
    bloom::document::CompositionFormat compositionFormat;
    EvaluationResolution resolution = CompositionFormatResolution{};
    PreviewResolutionPolicy policy = PreviewResolutionPolicy::Auto;
    ImageWindow dataWindow;
    ImageWindow displayWindow;
    PixelAspectRatio pixelAspect = PixelAspectRatio::square();
    std::optional<ImageWindow> roi = std::nullopt;
};

[[nodiscard]] PreviewRequestIdentity requestIdentityFor(const CaseSpec& spec,
                                                        const CompiledCompositionPlan& plan) {
    return PreviewRequestIdentity{.projectId = plan.projectId(),
                                  .compositionId = plan.compositionId(),
                                  .sourceRevision = plan.sourceRevision(),
                                  .requestGeneration = 1,
                                  .time = RationalTime::fromInteger(0),
                                  .output = PreviewOutput::Composition,
                                  .resolution = spec.resolution,
                                  .quality = EvaluationQuality::Reference,
                                  .colorIntent = EvaluationColorIntent::LinearRec709Scene,
                                  .resolutionPolicy = spec.policy,
                                  .roi = spec.roi,
                                  .viewAdjust = ViewAdjust{},
                                  .displayName = {},
                                  .viewName = {},
                                  .showLook = true};
}

[[nodiscard]] ProcessFrameIdentity
gpuProcessIdentityFor(const CaseSpec& spec,
                      const std::shared_ptr<const CompiledCompositionPlan>& plan) {
    return ProcessFrameIdentity{.plan = plan,
                                .time = RationalTime::fromInteger(0),
                                .output = plan->output(),
                                .resolution = spec.resolution,
                                .quality = EvaluationQuality::Reference,
                                .colorIntent = EvaluationColorIntent::LinearRec709Scene,
                                .provider = EvaluationProvider::GpuResident,
                                .roi = spec.roi};
}

[[nodiscard]] std::vector<EvaluatedOperationBounds> boundsFor(const ImageWindow& dataWindow) {
    EvaluatedOperationBounds bounds;
    bounds.local = ContentBounds{0.0, 0.0, static_cast<double>(dataWindow.extent().width()),
                                 static_cast<double>(dataWindow.extent().height())};
    bounds.output = bounds.local;
    bounds.layerId = kLayer;
    bounds.anchor = bloom::document::Vec2d{0.0, 0.0};
    return {bounds};
}

// Runs one accepted case end to end and asserts the product carries the trusted geometry.
void runAcceptedCase(Expectations& expectations, GpuDevice& device, PipelineSet& pipelines,
                     GpuResidentFrameLeaseRegistry& registry,
                     const PreparedCpuDisplayProcessorHandle& processor,
                     const std::shared_ptr<const GpuResidentPreviewQualificationReport>& report,
                     const CaseSpec& spec, const std::string& label, const std::size_t budget) {
    const auto plan = oneSolidPlan(spec.compositionFormat);
    const auto expected = descriptor(spec.dataWindow, spec.displayWindow, spec.pixelAspect);
    expectations.expect(expected.has_value(), label + ": trusted descriptor builds");
    if (!expected.has_value()) {
        return;
    }
    const auto identity = requestIdentityFor(spec, *plan);
    const auto processIdentity = gpuProcessIdentityFor(spec, plan);
    const auto bounds = boundsFor(spec.dataWindow);

    auto image =
        produceResidentDisplay(device, *pipelines.solid, *pipelines.display, spec.dataWindow,
                               spec.displayWindow, spec.pixelAspect, budget);
    expectations.expect(image != nullptr, label + ": the real display image is produced");
    if (image == nullptr) {
        return;
    }

    GpuResidentDisplayProductRequest request;
    request.identity = identity;
    request.processIdentity = processIdentity;
    request.bounds = bounds;
    request.expectedDescriptor = *expected;
    request.display = image;
    request.pixelStorageByteLimit = budget;

    std::string reason;
    expectations.expect(bloom::runtime::gpuResidentDisplayProductIsEligible(
                            device, registry, processor, *report, request, reason),
                        label + ": eligible: " + reason);
    auto product =
        bloom::runtime::makeGpuResidentDisplayPreview(device, registry, processor, report, request);
    expectations.expect(product.has_value(), label + ": product published");
    if (!product.has_value()) {
        return;
    }
    const auto* resident = product->residentFrame().get();
    expectations.expect(resident != nullptr, label + ": resident frame present");
    if (resident == nullptr) {
        return;
    }
    expectations.expect(product->provenance().provider ==
                            bloom::runtime::PreviewDisplayProvider::GpuResident,
                        label + ": GpuResident display provenance");
    expectations.expect(resident->processProvider() == EvaluationProvider::GpuResident,
                        label + ": GPU-evaluated process provenance, never CpuReference");
    expectations.expect(resident->qualification() == report,
                        label + ": the genuine report is retained");
    expectations.expect(!product->hasProcessFrame() && product->processFrame() == nullptr,
                        label + ": no process frame is retained");
    expectations.expect(!product->displayBufferView().has_value(),
                        label + ": no CPU pixels are exposed");
    expectations.expect(product->isDisplayValid() && resident->lease().isValid(),
                        label + ": the lease is live");
    expectations.expect(resident->retainedByteCost() >= resident->lease().allocationBytes(),
                        label + ": retained cost includes the native allocation");
    expectations.expect(product->desiredIdentity() == identity,
                        label + ": the request identity is preserved");
    expectations.expect(product->processIdentity() == processIdentity,
                        label + ": the process identity is preserved");
    expectations.expect(product->evaluatedBounds().size() == bounds.size(),
                        label + ": evaluated geometry is preserved");
}
