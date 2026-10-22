// Included by image_effect_tests.cpp inside its anonymous namespace.
void testPointwisePremultipliedHdr() {
    const auto window = render::ImageWindow::create(0, 0, 1, 1);
    const auto descriptor = render::Rgba32fImageDescriptor::create(
        *window.value(), *window.value(), core::PixelAspectRatio::square());
    for (const auto sample : {std::array<float, 2>{1.0e20F, 1.0e-20F},
                              std::array<float, 2>{2.0e38F, 0.5F}}) {
        auto builder = render::Rgba32fImageBuilder::create(*descriptor.value(), 1024);
        const auto pixel = render::Rgba32f::fromPremultiplied(
            sample[0], -sample[0], sample[0], sample[1]);
        auto row = builder.value()->row(0);
        (*row.value())[0] = *pixel.value();
        auto input = std::move(*builder.value()).freeze();
        for (const auto affine : {runtime::detail::PointwiseAffine{0.5F, 0.0F},
                                   runtime::detail::PointwiseAffine{1.0F, 2.0F},
                                   runtime::detail::PointwiseAffine{0.0F, 0.0F},
                                   runtime::detail::PointwiseAffine{-1.0F, 1.0F}}) {
            runtime::detail::PreparedImageEffect effect;
            effect.pointwise = affine;
            const auto result = runtime::detail::applyImageEffect(
                effect, *input.value(), nullptr, 1024, {},
                runtime::OperationIndex::fromRaw(1), {});
            expect(result.image && !result.diagnostic,
                   "pointwise HDR remains finite when straight RGB would overflow");
            if (result.image) {
                const auto actual = result.image->pixels().front();
                const float offset = sample[1] * affine.offset;
                expect(actual.red() == sample[0] * affine.scale + offset &&
                           actual.green() == -sample[0] * affine.scale + offset &&
                           actual.blue() == sample[0] * affine.scale + offset &&
                           actual.alpha() == sample[1],
                       "pointwise HDR uses the associated affine result and unchanged alpha");
            }
        }
    }
}
