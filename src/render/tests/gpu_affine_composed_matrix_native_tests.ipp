// Included by gpu_affine_native_tests.cpp inside its anonymous namespace. Extracted verbatim to
// keep the host translation unit within the source-size budget; the composed-matrix fixture,
// tolerance, map, and kernel are unchanged.

void testComposedMatrixForm(Expectations& expectations, GpuDevice& device) {
    auto affine = GpuAffine::create(device);
    auto uploader = GpuImageUpload::create(device);
    if (!affine || !uploader) {
        expectations.expect(false, "composed-matrix hosts created");
        return;
    }
    const auto sourceWindow = window(0, 0, 6, 4);
    const auto outputWindow = window(-4, -4, 16, 14);
    const auto sourcePixels = semanticPixels(6, 4);
    auto sourceImage =
        makeImage(sourceWindow, sourceWindow, PixelAspectRatio::square(), sourcePixels);
    if (!sourceImage) {
        return;
    }
    auto sourceResident =
        upload(*uploader.upload, std::make_shared<const Rgba32fImage>(std::move(*sourceImage)));
    if (!sourceResident) {
        return;
    }
    auto source = std::make_shared<const GpuImage>(std::move(*sourceResident));
    // A composed rotation+nonuniform-scale+translation matrix (with the anchor folded in), in
    // absolute output coordinates. This is the "precomposed parent matrix" input shape.
    const float opacity = 0.6F;
    const double radians = 25.0 * 3.14159265358979323846 / 180.0;
    const double cosine = std::cos(radians);
    const double sine = std::sin(radians);
    const bloom::render::GpuAffineMatrix matrix{.a = cosine * 1.5,
                                                .b = -sine * 0.8,
                                                .tx = 0.75,
                                                .c = sine * 1.5,
                                                .d = cosine * 0.8,
                                                .ty = -1.25};
    const auto begin =
        affine.affine->beginAffineMatrix({source, outputWindow, matrix, opacity}, kBudget);
    expectations.expect(begin.code == GpuAffineDiagnosticCode::None,
                        "the composed-matrix request begins: " + begin.message);
    if (begin.code != GpuAffineDiagnosticCode::None) {
        return;
    }
    GpuAffinePollResult poll = GpuAffinePollResult::Pending;
    while (poll == GpuAffinePollResult::Pending) {
        poll = affine.affine->poll();
    }
    expectations.expect(poll == GpuAffinePollResult::Ready,
                        "the composed-matrix request completes");
}
