#pragma once

// Immutable prepared OCIO GPU command: the exact extracted render::OcioGpuProgramDesc plus the
// exact validated render::CompiledGpuShader artifact, with the geometry and output encoding the
// dispatch will use. Nothing here borrows a mutable value: the descriptor, artifact, uniform
// snapshot, and derived SPIR-V words are owned by value and never change after preparation.
//
// The canonical identity is a closed, domain-separated serialization that covers the extracted
// program (its own config/semantics/shader/resource content identity and resource digest), the
// immutable uniform snapshot, the input geometry, the output encoding, and the compiled artifact
// digest. FinalRgba32f and DisplayRgba8 therefore never share an identity even for the same OCIO
// program, and a changed config, uniform, geometry, or artifact is a different command.

#include <bloom/core/sha256.hpp>
#include <bloom/render/gpu_shader_artifact.hpp>
#include <bloom/render/ocio_gpu_program.hpp>
#include <bloom/runtime/view_adjust.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bloom::runtime {

// The two pixel contracts this lane keeps distinct. FinalRgba32f is the process-effect arm
// (premultiplied resident RGBA32F, no clamp); DisplayRgba8 is the packed display arm (resident
// RGBA8). A FinalRgba32f result is never treated as a display result or vice versa.
enum class GpuOcioOutputEncoding : std::uint8_t {
    FinalRgba32f = 1,
    DisplayRgba8 = 2,
};

[[nodiscard]] std::string_view gpuOcioOutputEncodingName(GpuOcioOutputEncoding encoding) noexcept;

struct GpuOcioCommandGeometry final {
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    friend bool operator==(const GpuOcioCommandGeometry&, const GpuOcioCommandGeometry&) = default;
};

enum class GpuOcioCommandError : std::uint8_t {
    None,
    InvalidProgram,
    InvalidGeometry,
    InvalidArtifact,
    ArtifactDigestMismatch,
    StageEncodingMismatch,
    UniformSnapshotMismatch,
    InvalidViewAdjust,
    // A non-neutral adjustment was supplied for a non-display (ProcessEffect) command.
    UnsupportedViewAdjust,
    // The binding's wrapper version does not match the canonical production wrapper for
    // (program, viewAdjust).
    WrapperVersionMismatch,
    // The binding's wrapper source digest does not match the canonical production wrapper for
    // (program, viewAdjust), i.e. the artifact is stale for this adjustment.
    WrapperSourceDigestMismatch,
};

[[nodiscard]] std::string_view gpuOcioCommandErrorName(GpuOcioCommandError error) noexcept;

struct GpuOcioCommandIdentityParts final {
    GpuOcioOutputEncoding encoding = GpuOcioOutputEncoding::FinalRgba32f;
    GpuOcioCommandGeometry geometry;
    core::Sha256Digest programContentIdentity{};
    core::Sha256Digest programResourceDigest{};
    core::Sha256Digest programShaderTextDigest{};
    std::span<const std::byte> uniformSnapshot;
    core::Sha256Digest artifactDigest{};
    // The versioned per-sampler precise-sampling adapter used to build the wrapper (e.g.
    // color::kOcioGpuPreciseSamplingVersion). A sampling-semantics change must never reuse an
    // older artifact, so it is part of the canonical identity.
    std::string_view wrapperVersion;
    // The exact generated wrapper source digest. Binding it means a change to the wrapper structure
    // or its baked constants (including the view adjustment) can never reuse an older artifact.
    core::Sha256Digest wrapperSourceDigest{};
    // The display post-process adjustment. Its exact exposure/gamma are folded in, so a changed
    // adjustment is a different display command. A ProcessEffect command carries a neutral value
    // and its identity is unaffected by any display adjustment.
    ViewAdjust viewAdjust{};
};

// Canonical bytes are exactly:
//   ASCII "BloomGpuOcioCommandIdentity\0"
//   u16(3) || u8(encoding) || u32(width) || u32(height)
//   bytes32(programContentIdentity) || bytes32(programResourceDigest)
//   bytes32(programShaderTextDigest)
//   u64(uniformSnapshot.size) || exact uniformSnapshot bytes
//   bytes32(artifactDigest)
//   text(wrapperVersion) || bytes32(wrapperSourceDigest)
//   f64(viewAdjust.exposure) || f64(viewAdjust.gamma)
// u16/u32/u64 are unsigned big-endian; text is u32(byteCount) followed by UTF-8 bytes; f64 is the
// IEEE-754 big-endian bit pattern. The digest is SHA-256 of those bytes.
[[nodiscard]] core::Sha256Digest
computeGpuOcioCommandIdentity(const GpuOcioCommandIdentityParts& parts) noexcept;

// Immutable wrapper/source facts bound into the command at preparation time.
struct GpuOcioCommandSourceBinding final {
    std::string wrapperVersion;
    core::Sha256Digest wrapperSourceDigest{};
    ViewAdjust viewAdjust{};
};

class PreparedGpuOcioCommand;
struct GpuOcioCommandResult;

class PreparedGpuOcioCommand final {
  public:
    PreparedGpuOcioCommand(const PreparedGpuOcioCommand&) = delete;
    PreparedGpuOcioCommand& operator=(const PreparedGpuOcioCommand&) = delete;
    PreparedGpuOcioCommand(PreparedGpuOcioCommand&&) noexcept = default;
    PreparedGpuOcioCommand& operator=(PreparedGpuOcioCommand&&) noexcept = default;
    ~PreparedGpuOcioCommand() = default;

    // Validates the descriptor, the artifact (whole words, non-empty SPIR-V, exact digest, compute
    // entry point), the geometry, and the stage/encoding correspondence, then freezes the command.
    [[nodiscard]] static GpuOcioCommandResult prepare(render::OcioGpuProgramDesc program,
                                                      render::CompiledGpuShader artifact,
                                                      GpuOcioCommandGeometry geometry,
                                                      GpuOcioCommandSourceBinding binding);

    [[nodiscard]] const render::OcioGpuProgramDesc& program() const& noexcept { return program_; }
    [[nodiscard]] const render::OcioGpuProgramDesc& program() const&& = delete;
    [[nodiscard]] const render::CompiledGpuShader& artifact() const& noexcept { return artifact_; }
    [[nodiscard]] const render::CompiledGpuShader& artifact() const&& = delete;
    [[nodiscard]] GpuOcioOutputEncoding encoding() const noexcept { return encoding_; }
    [[nodiscard]] const GpuOcioCommandGeometry& geometry() const& noexcept { return geometry_; }
    [[nodiscard]] const GpuOcioCommandGeometry& geometry() const&& = delete;
    [[nodiscard]] const core::Sha256Digest& identity() const& noexcept { return identity_; }
    [[nodiscard]] const core::Sha256Digest& identity() const&& = delete;
    [[nodiscard]] std::string_view wrapperVersion() const& noexcept { return wrapperVersion_; }
    [[nodiscard]] std::string_view wrapperVersion() const&& = delete;
    [[nodiscard]] const core::Sha256Digest& wrapperSourceDigest() const& noexcept {
        return wrapperSourceDigest_;
    }
    [[nodiscard]] const core::Sha256Digest& wrapperSourceDigest() const&& = delete;
    // The exact display post-process adjustment bound into this command; neutral for ProcessEffect.
    [[nodiscard]] const ViewAdjust& viewAdjust() const& noexcept { return viewAdjust_; }
    [[nodiscard]] const ViewAdjust& viewAdjust() const&& = delete;
    [[nodiscard]] std::span<const std::uint32_t> spirvWords() const& noexcept {
        return spirvWords_;
    }
    [[nodiscard]] std::span<const std::uint32_t> spirvWords() const&& = delete;

    // Descriptor-declared native resource bytes this command will pin once its program exists:
    // aggregate LUT sample bytes + uniform buffer + 4-byte status + the SPIR-V module. It is a real
    // byte figure for cache accounting, not a requested-extent estimate.
    [[nodiscard]] std::uint64_t retainedBytes() const noexcept { return retainedBytes_; }

  private:
    PreparedGpuOcioCommand(render::OcioGpuProgramDesc program, render::CompiledGpuShader artifact,
                           GpuOcioOutputEncoding encoding, GpuOcioCommandGeometry geometry,
                           std::vector<std::uint32_t> spirvWords, core::Sha256Digest identity,
                           std::string wrapperVersion, core::Sha256Digest wrapperSourceDigest,
                           ViewAdjust viewAdjust, std::uint64_t retainedBytes) noexcept;

    render::OcioGpuProgramDesc program_;
    render::CompiledGpuShader artifact_;
    GpuOcioOutputEncoding encoding_ = GpuOcioOutputEncoding::FinalRgba32f;
    GpuOcioCommandGeometry geometry_;
    std::vector<std::uint32_t> spirvWords_;
    core::Sha256Digest identity_{};
    std::string wrapperVersion_;
    core::Sha256Digest wrapperSourceDigest_{};
    ViewAdjust viewAdjust_{};
    std::uint64_t retainedBytes_ = 0;
};

struct GpuOcioCommandResult final {
    std::shared_ptr<const PreparedGpuOcioCommand> command;
    GpuOcioCommandError error = GpuOcioCommandError::None;

    [[nodiscard]] bool hasValue() const noexcept { return command != nullptr; }
    [[nodiscard]] explicit operator bool() const noexcept { return hasValue(); }
};

} // namespace bloom::runtime
