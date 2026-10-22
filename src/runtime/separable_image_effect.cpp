#include "separable_image_effect.hpp"
#include "cpu_composition_evaluator_support.hpp"
#include "image_effect.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <span>

namespace bloom::runtime::detail {
namespace {
std::vector<float> weights(const SeparableBlurKind kind, const double radius) {
    const int support = static_cast<int>(std::ceil(radius));
    std::vector<float> result(static_cast<std::size_t>(support * 2 + 1), 1.0F);
    if (support == 0)
        return result;
    double total = 0.0;
    for (int i = -support; i <= support; ++i) {
        const double value = kind == SeparableBlurKind::Box || i == 0
                                 ? 1.0
                                 : std::exp(-0.5 * std::pow(double(i) / (radius / 3.0), 2.0));
        result[static_cast<std::size_t>(i + support)] = static_cast<float>(value);
        total += static_cast<double>(static_cast<float>(value));
    }
    for (auto& value : result)
        value = static_cast<float>(static_cast<double>(value) / total);
    return result;
}
ImageEffectResult failed(const EvaluationDiagnosticCode code, const char* text) {
    ImageEffectResult result;
    result.diagnostic = EvaluationDiagnostic{code, DiagnosticSeverity::Error, {}, text, text};
    return result;
}
} // namespace
std::optional<PreparedSeparableBlur> prepareSeparableBlur(const SeparableBlurKernel& kernel,
                                                          const std::optional<double> radius,
                                                          const double horizontalScale,
                                                          const double verticalScale) {
    if (!radius || !std::isfinite(*radius) || *radius < 0.0 ||
        *radius > document::kMaximumBlurRadiusPixels || !std::isfinite(horizontalScale) ||
        !std::isfinite(verticalScale) || horizontalScale <= 0.0 || verticalScale <= 0.0 ||
        *radius * horizontalScale > static_cast<double>(kMaximumSeparableBlurSupport) ||
        *radius * verticalScale > static_cast<double>(kMaximumSeparableBlurSupport) ||
        static_cast<unsigned>(kernel.kind) > 1 || static_cast<unsigned>(kernel.edge) > 1)
        return {};
    return PreparedSeparableBlur{weights(kernel.kind, *radius * horizontalScale),
                                 weights(kernel.kind, *radius * verticalScale), kernel.edge};
}
std::optional<render::ImageWindow> blurOutputWindow(const PreparedSeparableBlur& blur,
                                                    const render::ImageWindow input) {
    if (blur.edge == BlurEdgeMode::Clamp)
        return input;
    const auto x = static_cast<std::int64_t>(blur.horizontal.size() / 2);
    const auto y = static_cast<std::int64_t>(blur.vertical.size() / 2);
    if (input.originX() < std::numeric_limits<std::int64_t>::min() + x ||
        input.originY() < std::numeric_limits<std::int64_t>::min() + y)
        return {};
    auto window =
        render::ImageWindow::create(input.originX() - x, input.originY() - y,
                                    input.extent().width() + static_cast<std::uint64_t>(x * 2),
                                    input.extent().height() + static_cast<std::uint64_t>(y * 2));
    return window ? std::optional{*window.value()} : std::nullopt;
}
ImageEffectResult applySeparableBlur(const PreparedSeparableBlur& blur,
                                     const render::Rgba32fImage& input, const std::size_t budget,
                                     const CancellationToken& cancellation,
                                     const OperationIndex operation,
                                     const EvaluationProgressCallback& progress) {
    const auto window = blurOutputWindow(blur, input.descriptor()->dataWindow());
    if (!window)
        return failed(EvaluationDiagnosticCode::ArithmeticOverflow, "Blur halo overflows");
    const auto descriptor = render::Rgba32fImageDescriptor::create(
        *window, input.descriptor()->displayWindow(), input.descriptor()->pixelAspect());
    if (!descriptor)
        return failed(EvaluationDiagnosticCode::ArithmeticOverflow, "Blur descriptor overflows");
    const auto bytes = descriptor.value()->layout().pixelStorageBytes;
    if (bytes > budget / 2)
        return failed(EvaluationDiagnosticCode::PixelStorageBudgetExceeded,
                      "Blur intermediate and output exceed the pixel budget");
    std::shared_ptr<const render::Rgba32fImage> intermediate;
    for (unsigned axis = 0; axis < 2; ++axis) {
        if (cancellation.isCancellationRequested())
            return ImageEffectResult{{}, {}, true};
        const auto& source = axis == 0 ? input : *intermediate;
        const auto sourceWindow = source.descriptor()->dataWindow();
        const auto& coefficients = axis == 0 ? blur.horizontal : blur.vertical;
        const auto support = static_cast<std::int64_t>(coefficients.size() / 2);
        auto builder = render::Rgba32fImageBuilder::create(*descriptor.value(), budget - bytes);
        if (!builder)
            return failed(EvaluationDiagnosticCode::PixelStorageBudgetExceeded,
                          "Blur allocation refused");
        for (auto y = window->originY(); y < window->maxYExclusive(); ++y) {
            if (cancellation.isCancellationRequested())
                return ImageEffectResult{{}, {}, true};
            auto row = builder.value()->row(y);
            for (auto x = window->originX(); x < window->maxXExclusive(); ++x) {
                if ((static_cast<std::uint64_t>(x - window->originX()) & 63U) == 0 &&
                    cancellation.isCancellationRequested())
                    return ImageEffectResult{{}, {}, true};
                std::array<float, 4> sum{};
                for (std::int64_t tap = -support; tap <= support; ++tap) {
                    const auto shifted = [](const std::int64_t value, const std::int64_t delta) {
                        if (delta < 0 && value < std::numeric_limits<std::int64_t>::min() - delta)
                            return std::numeric_limits<std::int64_t>::min();
                        if (delta > 0 && value > std::numeric_limits<std::int64_t>::max() - delta)
                            return std::numeric_limits<std::int64_t>::max();
                        return value + delta;
                    };
                    auto sx = shifted(x, axis == 0 ? tap : 0);
                    auto sy = shifted(y, axis == 1 ? tap : 0);
                    if (blur.edge == BlurEdgeMode::Clamp) {
                        sx = std::clamp(sx, sourceWindow.originX(),
                                        sourceWindow.maxXExclusive() - 1);
                        sy = std::clamp(sy, sourceWindow.originY(),
                                        sourceWindow.maxYExclusive() - 1);
                    }
                    const auto sample = source.read(sx, sy);
                    if (!sample)
                        continue;
                    const auto p = *sample.value();
                    const std::array channels{p.red(), p.green(), p.blue(), p.alpha()};
                    const auto weight = coefficients[static_cast<std::size_t>(tap + support)];
                    for (std::size_t channel = 0; channel < 4; ++channel)
                        sum[channel] = std::fma(channels[channel], weight, sum[channel]);
                }
                auto pixel = render::Rgba32f::fromPremultiplied(sum[0], sum[1], sum[2],
                                                                std::clamp(sum[3], 0.0F, 1.0F));
                if (!pixel)
                    return failed(EvaluationDiagnosticCode::InvalidPixel,
                                  "Blur produced nonfinite HDR pixels");
                (*row.value())[static_cast<std::size_t>(x - window->originX())] = *pixel.value();
            }
            reportProgress(progress, {EvaluationProgressStage::Operation, operation,
                                      axis * window->extent().height() +
                                          static_cast<std::uint64_t>(y - window->originY()) + 1,
                                      2 * window->extent().height()});
        }
        if (cancellation.isCancellationRequested())
            return ImageEffectResult{{}, {}, true};
        auto frozen = std::move(*builder.value()).freeze();
        if (!frozen)
            return failed(EvaluationDiagnosticCode::InvalidPixel, "Blur image publication failed");
        intermediate = std::make_shared<const render::Rgba32fImage>(std::move(*frozen.value()));
    }
    return {std::move(intermediate), {}, false};
}
render::OcioGpuProgramResult buildSeparableBlurProgram(const std::span<const float> coefficients,
                                                       const bool vertical,
                                                       const BlurEdgeMode edge) {
    if (coefficients.empty() || coefficients.size() > 2 * kMaximumSeparableBlurSupport + 1 ||
        coefficients.size() % 2 != 1 || static_cast<unsigned>(edge) > 1)
        return render::OcioGpuProgramResult::failure(render::OcioGpuProgramError::InvalidRequest);
    std::string shader = "const float bloom_blur_weights[] = float[](";
    for (std::size_t i = 0; i < coefficients.size(); ++i) {
        if (!std::isfinite(coefficients[i]) || coefficients[i] < 0.0F)
            return render::OcioGpuProgramResult::failure(
                render::OcioGpuProgramError::InvalidRequest);
        char literal[64];
        const auto text = std::to_chars(literal, literal + sizeof(literal), coefficients[i],
                                        std::chars_format::scientific, 9);
        if (i)
            shader += ',';
        shader.append(literal, text.ptr);
    }
    const auto support = std::to_string(coefficients.size() / 2);
    shader += ");\nvec4 bloom_separable_blur(ivec2 c) {\n"
              " ivec2 size = imageSize(bloom_ocio_input);\n precise vec4 sum = vec4(0.0);\n"
              " for (int i = -" +
              support + "; i <= " + support +
              "; ++i) {\n"
              "  ivec2 p = c + ivec2(" +
              (vertical ? std::string("0, i") : std::string("i, 0")) + ");\n";
    if (edge == BlurEdgeMode::Clamp)
        shader += "  p = clamp(p, ivec2(0), size - ivec2(1));\n";
    else
        shader += "  if (any(lessThan(p, ivec2(0))) || any(greaterThanEqual(p, size))) continue;\n";
    shader += "  sum = fma(imageLoad(bloom_ocio_input, p), vec4(bloom_blur_weights[i + " + support +
              "]), sum);\n"
              " }\n sum.a = clamp(sum.a, 0.0, 1.0);\n return sum;\n}\n";
    render::OcioGpuProgramDesc program;
    program.stage = render::OcioGpuProgramStage::ProcessNeighborhood;
    program.functionName = "bloom_separable_blur";
    program.resourcePrefix = "bloom_blur_";
    program.shaderText = std::move(shader);
    program.semanticsId = "bloom.render.separable-blur.v1";
    program.shaderTextDigest = render::computeOcioGpuShaderTextDigest(program.shaderText);
    program.resourceDigest = render::computeOcioGpuResourceDigest(program);
    program.contentIdentity = render::computeOcioGpuContentIdentity(
        {program.semanticsId, {}, {}, program.shaderTextDigest, program.resourceDigest});
    return render::OcioGpuProgramResult::success(std::move(program));
}
} // namespace bloom::runtime::detail
