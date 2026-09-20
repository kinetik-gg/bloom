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
};

// Canonical bytes are exactly:
//   ASCII "BloomGpuOcioCommandIdentity\0"
//   u16(1) || u8(encoding) || u32(width) || u32(height)
//   bytes32(programContentIdentity) || bytes32(programResourceDigest)
//   bytes32(programShaderTextDigest)
//   u64(uniformSnapshot.size) || exact uniformSnapshot bytes
//   bytes32(artifactDigest)
// u16/u32/u64 are unsigned big-endian. The digest is SHA-256 of those bytes.
[[nodiscard]] core::Sha256Digest
computeGpuOcioCommandIdentity(const GpuOcioCommandIdentityParts& parts) noexcept;

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
                                                      GpuOcioCommandGeometry geometry);

    [[nodiscard]] const render::OcioGpuProgramDesc& program() const& noexcept { return program_; }
    [[nodiscard]] const render::OcioGpuProgramDesc& program() const&& = delete;
    [[nodiscard]] const render::CompiledGpuShader& artifact() const& noexcept { return artifact_; }
    [[nodiscard]] const render::CompiledGpuShader& artifact() const&& = delete;
    [[nodiscard]] GpuOcioOutputEncoding encoding() const noexcept { return encoding_; }
    [[nodiscard]] const GpuOcioCommandGeometry& geometry() const& noexcept { return geometry_; }
    [[nodiscard]] const GpuOcioCommandGeometry& geometry() const&& = delete;
    [[nodiscard]] const core::Sha256Digest& identity() const& noexcept { return identity_; }
    [[nodiscard]] const core::Sha256Digest& identity() const&& = delete;
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
                           std::uint64_t retainedBytes) noexcept;

    render::OcioGpuProgramDesc program_;
    render::CompiledGpuShader artifact_;
    GpuOcioOutputEncoding encoding_ = GpuOcioOutputEncoding::FinalRgba32f;
    GpuOcioCommandGeometry geometry_;
    std::vector<std::uint32_t> spirvWords_;
    core::Sha256Digest identity_{};
    std::uint64_t retainedBytes_ = 0;
};

struct GpuOcioCommandResult final {
    std::shared_ptr<const PreparedGpuOcioCommand> command;
    GpuOcioCommandError error = GpuOcioCommandError::None;

    [[nodiscard]] bool hasValue() const noexcept { return command != nullptr; }
    [[nodiscard]] explicit operator bool() const noexcept { return hasValue(); }
};

} // namespace bloom::runtime
