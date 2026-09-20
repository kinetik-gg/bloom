#ifndef BLOOM_RUNTIME_GPU_SCENE_PREPARATION_PRIVATE_HPP
#define BLOOM_RUNTIME_GPU_SCENE_PREPARATION_PRIVATE_HPP

// Private to src/runtime. The scene builder reuses the REAL six-argument detail::preflight through
// cpu_composition_evaluator_support.hpp (the four-argument overload has no definition), so the
// evaluator and GPU preparation resolve every parameter and geometry identically.
//
// Shader pins are the SPIR-V digests of the render-owned kernel shaders. They enter command
// semantic keys so a kernel change can never serve a stale image. The ordinary SolidV1,
// TranslationOpacityBilinearV1, and SourceOverV1 pins are unchanged; the CoveredSolidV1 digest is
// the actual embedded solid_covered_spirv.inc digest from the render-owned shader.

#include "cpu_composition_evaluator_support.hpp"
#include "cpu_composition_resolution.hpp"
#include "layer_parent_transform.hpp"

#include <bloom/render/path_raster.hpp>
#include <bloom/runtime/gpu_scene_coverage_cache.hpp>
#include <bloom/runtime/operation_cache.hpp>

#include <memory>

namespace bloom::runtime::detail {

inline constexpr std::string_view kGpuSolidSpirvSha256 =
    "2fae4aef67267e98a53034448639bfb00a69568ec3be1d8ed1a1bc60b31a0318";
inline constexpr std::string_view kGpuTranslationOpacitySpirvSha256 =
    "0049b132bf98214375023a82472f2839a0e8f30e1b1209a0f214ef283b97c3b8";
inline constexpr std::string_view kGpuSourceOverSpirvSha256 =
    "2aea19b4e3620e9e99f09f7f0e22f97822119ea798b44a1077179c4fc58194ee";
inline constexpr std::string_view kGpuCoveredSolidSpirvSha256 =
    "3d54bcb0b9394b381df9f271cdfd05af4cb2f80620ba142227fec53ba26e1a9b";
// The BlendV1 f32/universal artifact digest. Advisory only in the command key: the executor
// canonicalizes the effective f32/f64 pin from the explicit mode and the device capability, so a
// stale producer value can never bless another shader's cached output.
inline constexpr std::string_view kGpuBlendSpirvSha256 =
    "4b9cc2009f9a1fcbdc05558bbbbb9e9aeb7573a4fc88a8127e25e22bc6c208f8";
inline constexpr std::string_view kGpuAffineSpirvSha256 =
    "ee28424e6b4b7d3c4e437f6ce438f75c7dfbf57e78b2a06b1e4aed1cb71f9373";

} // namespace bloom::runtime::detail

#endif // BLOOM_RUNTIME_GPU_SCENE_PREPARATION_PRIVATE_HPP
