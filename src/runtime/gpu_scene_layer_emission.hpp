#ifndef BLOOM_RUNTIME_GPU_SCENE_LAYER_EMISSION_HPP
#define BLOOM_RUNTIME_GPU_SCENE_LAYER_EMISSION_HPP

// Private to src/runtime. The shared builder's raster-layer emission: the exact translation path
// and the accepted GpuAffine placement. Header-only and templated on the builder's emit/charge
// closures so the split adds no std::function and no shared state; the builder still owns the
// command vector, the per-operation maps, the budget and the coverage cache.

#include "gpu_scene_preparation_common.hpp"
#include "gpu_scene_preparation_private.hpp"
#include "layer_parent_transform.hpp"

#include <bloom/render/cpu_image_primitives.hpp>
#include <bloom/render/image.hpp>
#include <bloom/runtime/prepared_gpu_scene.hpp>

#include <bit>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace bloom::runtime::detail {

// The exact conversion from the runtime's AUTHOR-space LayerMatrix (pixel-centre + 0.5 convention)
// to GpuAffine's source-local-0-based-pixel-centre to absolute-output-device-index convention,
// folding the proxy scales exactly as ParentedLayerTransform::forwardMap does:
//   X = a*lx + (sx/sy)*b*ly + a*ox + (sx/sy)*b*oy + sx*m.x - 0.5
//   Y = (sy/sx)*c*lx + d*ly + (sy/sx)*c*ox + d*oy + sy*m.y - 0.5
// where (ox, oy) is the source data-window origin and (lx, ly) a 0-based source pixel centre. At
// sx == sy == 1 this is the accepted native affine conversion.
[[nodiscard]] inline render::GpuAffineMatrix
toGpuAffineMatrix(const LayerMatrix& matrix, const render::ImageWindow sourceWindow,
                  const double scaleX, const double scaleY) {
    const double ox = static_cast<double>(sourceWindow.originX()) + 0.5;
    const double oy = static_cast<double>(sourceWindow.originY()) + 0.5;
    const double b = (scaleX / scaleY) * matrix.b;
    const double c = (scaleY / scaleX) * matrix.c;
    return render::GpuAffineMatrix{.a = matrix.a,
                                   .b = b,
                                   .tx = matrix.a * ox + b * oy + scaleX * matrix.x - 0.5,
                                   .c = c,
                                   .d = matrix.d,
                                   .ty = c * ox + matrix.d * oy + scaleY * matrix.y - 0.5};
}

// The output-window-local device translation for a translation-only LayerTransform, or disengaged
// when the transform is not an exact translation.
[[nodiscard]] inline std::optional<std::pair<double, double>>
translationOnlyDeviceDelta(const render::LayerTransform& transform,
                           const render::ImageWindow sourceWindow,
                           const render::ImageWindow outputWindow) {
    const auto device = transform.translationOnlyDeviceTranslation();
    if (!device.has_value()) {
        return std::nullopt;
    }
    return std::make_pair(device->x - (static_cast<double>(outputWindow.originX()) -
                                       static_cast<double>(sourceWindow.originX())),
                          device->y - (static_cast<double>(outputWindow.originY()) -
                                       static_cast<double>(sourceWindow.originY())));
}

template <typename Charge, typename Emit>
[[nodiscard]] inline std::optional<GpuSceneLeafFailure> emitTranslationOpacityCommand(
    const GpuSceneCommandIndex input, const std::string& inputKey,
    const render::ImageWindow sourceWindow, const render::ImageWindow outputWindow,
    const double translationX, const double translationY, const double opacity,
    const core::PixelAspectRatio fullPixelAspect, const OperationIndex operationIndex,
    Charge&& charge, Emit&& emit, GpuSceneCommandIndex& out, std::string& key) {
    OperationKey commandKey;
    commandKey.add(std::string{"gpu-translation-opacity-v1"});
    commandKey.add(inputKey);
    commandKey.add(translationX);
    commandKey.add(translationY);
    commandKey.add(std::bit_cast<std::uint32_t>(static_cast<float>(opacity)));
    addWindowToKey(commandKey, sourceWindow);
    addWindowToKey(commandKey, outputWindow);
    addPixelAspectToKey(commandKey, fullPixelAspect);
    commandKey.add(std::string{kGpuTranslationOpacitySpirvSha256});
    key = commandKey.digest();
    if (const auto error = charge(outputWindow.extent().width(), outputWindow.extent().height(),
                                  sizeof(render::Rgba32f))) {
        return error;
    }
    GpuSceneTranslationCommand command{.sourceOperation = operationIndex,
                                       .input = input,
                                       .sourceWindow = sourceWindow,
                                       .outputWindow = outputWindow,
                                       .translationX = translationX,
                                       .translationY = translationY,
                                       .opacity = static_cast<float>(opacity),
                                       .semanticKey = key};
    out = emit(std::move(command));
    return std::nullopt;
}

template <typename Charge, typename Emit>
[[nodiscard]] inline std::optional<GpuSceneLeafFailure> emitAffineLayerCommand(
    const GpuSceneCommandIndex input, const std::string& inputKey,
    const render::ImageWindow sourceWindow, const render::ImageWindow outputWindow,
    const LayerMatrix& matrix, const double scaleX, const double scaleY, const double opacity,
    const core::PixelAspectRatio fullPixelAspect, const OperationIndex operationIndex,
    Charge&& charge, Emit&& emit, GpuSceneCommandIndex& out, std::string& key) {
    const auto affineMatrix = toGpuAffineMatrix(matrix, sourceWindow, scaleX, scaleY);
    key = makeGpuSceneAffineSemanticKey(inputKey, sourceWindow, affineMatrix,
                                        static_cast<float>(opacity), outputWindow, fullPixelAspect,
                                        std::string{kGpuAffineSpirvSha256});
    if (const auto error = charge(outputWindow.extent().width(), outputWindow.extent().height(),
                                  sizeof(render::Rgba32f))) {
        return error;
    }
    GpuSceneAffineCommand affine{.sourceOperation = operationIndex,
                                 .input = input,
                                 .inputKey = inputKey,
                                 .sourceWindow = sourceWindow,
                                 .outputWindow = outputWindow,
                                 .matrix = affineMatrix,
                                 .opacity = static_cast<float>(opacity),
                                 .pixelAspect = fullPixelAspect,
                                 .semanticKey = key};
    out = emit(std::move(affine));
    return std::nullopt;
}

} // namespace bloom::runtime::detail

#endif // BLOOM_RUNTIME_GPU_SCENE_LAYER_EMISSION_HPP
