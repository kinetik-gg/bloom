#pragma once

#include <bloom/core/sha256.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace bloom::render {

// The portable, render-owned product of one runtime GLSL -> SPIR-V compile. It carries only
// immutable bytes and provenance; it never names a Vulkan object, descriptor set, or pipeline.
// The native consumer owns wrapping this artifact into a pipeline and validating descriptors.
enum class GpuShaderStage : std::uint8_t { Compute = 0 };

// Exact identity of one pinned tool executable actually invoked. `executableDigest` binds the
// exact bytes, so two different paths to the same build share an identity and a rebuilt tool
// does not reuse a cache entry.
struct GpuShaderToolIdentity {
    std::string role;
    std::string path;
    std::string version;
    core::Sha256Digest executableDigest{};

    friend bool operator==(const GpuShaderToolIdentity&, const GpuShaderToolIdentity&) = default;
};

struct CompiledGpuShader {
    std::vector<std::uint8_t> spirv;
    core::Sha256Digest spirvDigest{};
    std::string entryPoint;
    std::string targetEnvironment;
    GpuShaderStage stage = GpuShaderStage::Compute;
    GpuShaderToolIdentity compiler;
    GpuShaderToolIdentity validator;
};

} // namespace bloom::render
