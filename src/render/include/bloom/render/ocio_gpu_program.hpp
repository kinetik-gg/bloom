#pragma once

// Portable, immutable description of one extracted OpenColorIO GPU program. This header is
// deliberately OCIO-, Vulkan-, and Qt-free: it carries only Bloom value types so a GPU consumer
// (native pipeline, isolated compiler helper) and the colour producer can share one value without
// creating a module cycle. The colour producer owns the semantic identity; this layer owns the
// shader text, the resource/descriptor reflection, and the device-independent content digests.
//
// Wrapper semantics contract (enforced by the native lane, oracle here):
//  * Process effect arm reads finite premultiplied RGBA32F. It reproduces the CPU image-effect
//    flow exactly: un-premultiply RGB with one binary32 divide (alpha == 0 => straight +0 RGB and
//    the input pixel is copied unchanged), apply the OCIO function to straight RGB only, keep the
//    original alpha, re-premultiply, and publish finite premultiplied RGBA32F.
//  * Display arm reads finite premultiplied RGBA32F, un-premultiplies the same way, calls the OCIO
//    function with the alpha lane 1.0 and discards the returned alpha, keeps the original alpha,
//    applies the caller's declared display clamp and packing to straight RGBA8
//    (floor(clamp(v,0,1)*255+0.5)), and never reads back a full frame on the interactive path.
//  * Viewer exposure/gamma (runtime::ViewAdjust) acts after the OCIO display function on the
//    linear display light and encoded value respectively; it is session state, never program
//    identity, so changing it must update uniforms, not recompile.
// OCIO descriptor sets: setDescriptorIndex() with the default textureBindingStart reserves
// binding 0 for the OCIO uniform buffer. Bloom's own I/O buffers live in a disjoint set recorded
// by the native consumer; this value never rewrites OCIO shader text to move them.

#include <bloom/core/sha256.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bloom::render {

enum class OcioGpuProgramError : std::uint8_t {
    None,
    InvalidRequest,
    UnsupportedColorSpace,
    IdentityTransform,
    TransformBuildFailed,
    GpuProcessorUnavailable,
    ShaderExtractionFailed,
    IdentityShaderText,
    ResourceLimitExceeded,
    UnsupportedResourceForm,
    // A file transform's LUT bytes are never parsed in-process. On a target that cannot provide the
    // isolated bloom-color-worker (missing confinement/process supervision) this typed boundary is
    // the honest failure instead of an in-process parse.
    ExternalLutBoundaryRequired,
    // The caller's cancellation was observed while the isolated helper owned the request; the
    // helper was terminated and no partial program was published.
    Cancelled,
};

[[nodiscard]] std::string_view ocioGpuProgramErrorName(OcioGpuProgramError error) noexcept;

enum class OcioGpuInterpolation : std::uint8_t {
    Unknown = 0,
    Nearest,
    Linear,
    Tetrahedral,
    Cubic,
};

[[nodiscard]] std::string_view ocioGpuInterpolationName(OcioGpuInterpolation value) noexcept;

enum class OcioGpuTextureDimensions : std::uint8_t { OneD = 1, TwoD = 2, ThreeD = 3 };

enum class OcioGpuTextureChannel : std::uint8_t { Red, Rgb };

// One OCIO LUT texture. `samples` is the exact OCIO-ordered float data, never resampled or
// reordered. For a 1D LUT width is the sample count and height is 1; for a 2D LUT the pair is
// width x height; for a 3D LUT `edgeLength` is the cube edge and width/height stay 0.
struct OcioGpuTextureDesc final {
    std::uint32_t binding = 0;
    std::string name;
    std::string samplerName;
    OcioGpuTextureDimensions dimensions = OcioGpuTextureDimensions::OneD;
    OcioGpuTextureChannel channel = OcioGpuTextureChannel::Rgb;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t edgeLength = 0;
    OcioGpuInterpolation interpolation = OcioGpuInterpolation::Unknown;
    std::vector<float> samples;

    friend bool operator==(const OcioGpuTextureDesc&, const OcioGpuTextureDesc&) = default;
};

// The OCIO uniform data types. A uniform buffer entry's byte offset is `bufferOffset`; the whole
// buffer is `uniformBufferSize` bytes and is uploaded without recompiling the program.
enum class OcioGpuUniformType : std::uint8_t {
    Double = 0,
    Bool,
    Float3,
    VectorFloat,
    VectorInt,
    Unknown,
};

struct OcioGpuUniformDesc final {
    std::string name;
    OcioGpuUniformType type = OcioGpuUniformType::Unknown;
    std::uint32_t bufferOffset = 0;
    std::uint32_t elementCount = 1;

    friend bool operator==(const OcioGpuUniformDesc&, const OcioGpuUniformDesc&) = default;
};

enum class OcioGpuProgramStage : std::uint8_t {
    // CST / FileTransform: straight-RGB in, straight-RGB out, caller keeps alpha association.
    ProcessEffect,
    // Display: straight-RGB display result for the caller's clamp/packing step.
    DisplayPacking,
};

// Checked ceilings applied before a program is accepted. The producer enforces these against the
// real OCIO reflection; an out-of-limit extraction is a typed refusal, never a truncated program.
struct OcioGpuProgramLimits final {
    std::uint64_t maxShaderBytes = 4U * 1024U * 1024U;
    std::uint32_t maxTextures = 32;
    std::uint32_t max3dEdge = 129;
    std::uint64_t maxAggregateLutBytes = 256U * 1024U * 1024U;
    std::uint32_t maxUniforms = 256;
    std::uint32_t maxDynamicPropertyCount = 32;
    std::uint64_t maxUniformBufferBytes = 64U * 1024U;
};

// The immutable extracted program. `shaderText` is the exact OCIO GLSL_VK_4_6 program, including
// its own `layout(set = ..., binding = ...)` declarations; it is never string-rewritten.
struct OcioGpuProgramDesc final {
    std::string shaderText;
    std::string functionName;
    std::string resourcePrefix;
    std::uint32_t descriptorSetIndex = 0;
    std::uint32_t textureBindingStart = 1;
    OcioGpuProgramStage stage = OcioGpuProgramStage::ProcessEffect;
    std::vector<OcioGpuTextureDesc> textures;
    std::vector<OcioGpuUniformDesc> uniforms;
    std::uint32_t dynamicPropertyCount = 0;
    std::uint64_t uniformBufferSize = 0;
    // Immutable snapshot of the OCIO uniform values at extraction time, exactly uniformBufferSize
    // bytes. The native executor uploads these bytes without recompiling; a caller may override
    // them per request with the same size (e.g. a viewer exposure change).
    std::vector<std::byte> uniformBufferData;
    // Device-independent content digests, folded into `contentIdentity` by the producer.
    core::Sha256Digest shaderTextDigest;
    core::Sha256Digest resourceDigest;
    // Stable identity of config + transform settings + OCIO GPU program/resources. No node,
    // layer, plan index, frame time, request generation, or revision enters this value.
    core::Sha256Digest contentIdentity;
    // OCIO provenance only; never a substitute for the Bloom content identity or revision.
    std::string processorCacheId;
    std::string gpuProcessorCacheId;
    std::string ocioVersion;
    // Producer-owned closed semantics id, e.g. "bloom.color.ocio-process-cst.v1".
    std::string semanticsId;
};

struct OcioGpuProgramResult final {
    [[nodiscard]] static OcioGpuProgramResult success(OcioGpuProgramDesc program);
    [[nodiscard]] static OcioGpuProgramResult failure(OcioGpuProgramError error) noexcept;

    OcioGpuProgramResult(OcioGpuProgramResult&&) noexcept = default;
    OcioGpuProgramResult& operator=(OcioGpuProgramResult&&) noexcept = default;
    OcioGpuProgramResult(const OcioGpuProgramResult&) = delete;
    OcioGpuProgramResult& operator=(const OcioGpuProgramResult&) = delete;
    ~OcioGpuProgramResult() = default;

    [[nodiscard]] bool succeeded() const noexcept { return program_.has_value(); }
    [[nodiscard]] explicit operator bool() const noexcept { return succeeded(); }
    [[nodiscard]] OcioGpuProgramError error() const noexcept { return error_; }
    [[nodiscard]] const OcioGpuProgramDesc* program() const& noexcept {
        return program_ ? &*program_ : nullptr;
    }
    [[nodiscard]] const OcioGpuProgramDesc* program() const&& = delete;
    [[nodiscard]] std::optional<OcioGpuProgramDesc> takeProgram() && noexcept {
        return std::move(program_);
    }

  private:
    OcioGpuProgramResult(std::optional<OcioGpuProgramDesc> program,
                         OcioGpuProgramError error) noexcept;

    std::optional<OcioGpuProgramDesc> program_;
    OcioGpuProgramError error_ = OcioGpuProgramError::None;
};

// Canonical device-independent digest of the exact shader text.
[[nodiscard]] core::Sha256Digest
computeOcioGpuShaderTextDigest(std::string_view shaderText) noexcept;

// Canonical device-independent digest of the reflected texture/uniform/UBO resources.
[[nodiscard]] core::Sha256Digest computeOcioGpuResourceDigest(const OcioGpuProgramDesc&) noexcept;

// Validates every limit and structural invariant (unique names, contiguous texture bindings,
// uniform offsets inside the declared buffer, LUT sample counts matching declared dimensions,
// finite samples). Returns the first violation.
[[nodiscard]] OcioGpuProgramError validateOcioGpuProgram(const OcioGpuProgramDesc&,
                                                         const OcioGpuProgramLimits&) noexcept;

struct OcioGpuContentIdentityParts final {
    std::string_view semanticsId;
    std::span<const std::byte> semanticIdentityBytes;
    core::Sha256Digest ocioConfigRevision;
    core::Sha256Digest shaderTextDigest;
    core::Sha256Digest resourceDigest;
};

// Folds the producer-owned semantic identity and the extracted program digests into the stable
// content identity. The domain prefix and version keep this identity distinct from any other
// Bloom digest domain.
[[nodiscard]] core::Sha256Digest
computeOcioGpuContentIdentity(const OcioGpuContentIdentityParts&) noexcept;

} // namespace bloom::render
