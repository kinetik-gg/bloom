#include <bloom/runtime/gpu_ocio_command.hpp>

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace bloom::runtime {
namespace {

void appendBigEndian(std::vector<std::byte>& bytes, const std::uint64_t value,
                     const int byteCount) {
    for (int shift = (byteCount - 1) * 8; shift >= 0; shift -= 8) {
        bytes.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
    }
}

void appendDigest(std::vector<std::byte>& bytes, const core::Sha256Digest& digest) {
    for (const auto value : digest.bytes()) {
        bytes.push_back(static_cast<std::byte>(value));
    }
}

[[nodiscard]] std::uint64_t retainedResourceBytes(const render::OcioGpuProgramDesc& program,
                                                  const std::size_t spirvBytes) noexcept {
    std::uint64_t total = 0;
    const auto add = [&total](const std::uint64_t value) {
        if (total > std::numeric_limits<std::uint64_t>::max() - value) {
            total = std::numeric_limits<std::uint64_t>::max();
            return;
        }
        total += value;
    };
    for (const auto& texture : program.textures) {
        add(static_cast<std::uint64_t>(texture.samples.size()) * sizeof(float));
    }
    add(program.uniformBufferSize);
    add(4);
    add(spirvBytes);
    return total;
}

[[nodiscard]] GpuOcioCommandResult failure(const GpuOcioCommandError error) noexcept {
    GpuOcioCommandResult result;
    result.error = error;
    return result;
}

} // namespace

std::string_view gpuOcioOutputEncodingName(const GpuOcioOutputEncoding encoding) noexcept {
    switch (encoding) {
    case GpuOcioOutputEncoding::FinalRgba32f:
        return "final-rgba32f";
    case GpuOcioOutputEncoding::DisplayRgba8:
        return "display-rgba8";
    }
    return "unknown";
}

std::string_view gpuOcioCommandErrorName(const GpuOcioCommandError error) noexcept {
    switch (error) {
    case GpuOcioCommandError::None:
        return "none";
    case GpuOcioCommandError::InvalidProgram:
        return "invalid-program";
    case GpuOcioCommandError::InvalidGeometry:
        return "invalid-geometry";
    case GpuOcioCommandError::InvalidArtifact:
        return "invalid-artifact";
    case GpuOcioCommandError::ArtifactDigestMismatch:
        return "artifact-digest-mismatch";
    case GpuOcioCommandError::StageEncodingMismatch:
        return "stage-encoding-mismatch";
    case GpuOcioCommandError::UniformSnapshotMismatch:
        return "uniform-snapshot-mismatch";
    }
    return "unknown";
}

core::Sha256Digest
computeGpuOcioCommandIdentity(const GpuOcioCommandIdentityParts& parts) noexcept {
    static constexpr std::string_view kDomain = "BloomGpuOcioCommandIdentity";
    std::vector<std::byte> bytes;
    bytes.reserve(kDomain.size() + 1 + 2 + 1 + 4 + 4 + 32 + 32 + 32 + 8 +
                  parts.uniformSnapshot.size() + 32);
    for (const char character : kDomain) {
        bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
    }
    bytes.push_back(std::byte{0});
    appendBigEndian(bytes, 1, 2);
    appendBigEndian(bytes, static_cast<std::uint64_t>(parts.encoding), 1);
    appendBigEndian(bytes, parts.geometry.width, 4);
    appendBigEndian(bytes, parts.geometry.height, 4);
    appendDigest(bytes, parts.programContentIdentity);
    appendDigest(bytes, parts.programResourceDigest);
    appendDigest(bytes, parts.programShaderTextDigest);
    appendBigEndian(bytes, parts.uniformSnapshot.size(), 8);
    bytes.insert(bytes.end(), parts.uniformSnapshot.begin(), parts.uniformSnapshot.end());
    appendDigest(bytes, parts.artifactDigest);
    const auto digest = core::Sha256Hasher::hash(bytes);
    return digest.has_value() ? *digest : core::Sha256Digest{};
}

PreparedGpuOcioCommand::PreparedGpuOcioCommand(render::OcioGpuProgramDesc program,
                                               render::CompiledGpuShader artifact,
                                               const GpuOcioOutputEncoding encoding,
                                               const GpuOcioCommandGeometry geometry,
                                               std::vector<std::uint32_t> spirvWords,
                                               core::Sha256Digest identity,
                                               const std::uint64_t retainedBytes) noexcept
    : program_(std::move(program)), artifact_(std::move(artifact)), encoding_(encoding),
      geometry_(geometry), spirvWords_(std::move(spirvWords)), identity_(identity),
      retainedBytes_(retainedBytes) {}

GpuOcioCommandResult PreparedGpuOcioCommand::prepare(render::OcioGpuProgramDesc program,
                                                     render::CompiledGpuShader artifact,
                                                     const GpuOcioCommandGeometry geometry) {
    if (geometry.width == 0 || geometry.height == 0) {
        return failure(GpuOcioCommandError::InvalidGeometry);
    }
    const std::uint64_t pixelCount = static_cast<std::uint64_t>(geometry.width) * geometry.height;
    if (pixelCount == 0 || pixelCount > std::numeric_limits<std::uint32_t>::max()) {
        return failure(GpuOcioCommandError::InvalidGeometry);
    }
    if (render::validateOcioGpuProgram(program, {}) != render::OcioGpuProgramError::None) {
        return failure(GpuOcioCommandError::InvalidProgram);
    }
    if (!program.uniformBufferData.empty() &&
        program.uniformBufferData.size() != program.uniformBufferSize) {
        return failure(GpuOcioCommandError::UniformSnapshotMismatch);
    }

    GpuOcioOutputEncoding encoding = GpuOcioOutputEncoding::FinalRgba32f;
    switch (program.stage) {
    case render::OcioGpuProgramStage::ProcessEffect:
        encoding = GpuOcioOutputEncoding::FinalRgba32f;
        break;
    case render::OcioGpuProgramStage::DisplayPacking:
        encoding = GpuOcioOutputEncoding::DisplayRgba8;
        break;
    default:
        return failure(GpuOcioCommandError::StageEncodingMismatch);
    }

    if (artifact.spirv.empty() || artifact.spirv.size() % sizeof(std::uint32_t) != 0 ||
        artifact.entryPoint.empty() || artifact.stage != render::GpuShaderStage::Compute) {
        return failure(GpuOcioCommandError::InvalidArtifact);
    }
    const auto artifactDigest = core::Sha256Hasher::hash(std::as_bytes(std::span(artifact.spirv)));
    if (!artifactDigest.has_value() || *artifactDigest != artifact.spirvDigest) {
        return failure(GpuOcioCommandError::ArtifactDigestMismatch);
    }

    std::vector<std::uint32_t> spirvWords(artifact.spirv.size() / sizeof(std::uint32_t));
    std::memcpy(spirvWords.data(), artifact.spirv.data(), artifact.spirv.size());

    const GpuOcioCommandIdentityParts parts{
        .encoding = encoding,
        .geometry = geometry,
        .programContentIdentity = program.contentIdentity,
        .programResourceDigest = program.resourceDigest,
        .programShaderTextDigest = program.shaderTextDigest,
        .uniformSnapshot = std::span<const std::byte>(program.uniformBufferData.data(),
                                                      program.uniformBufferData.size()),
        .artifactDigest = artifact.spirvDigest,
    };
    const auto identity = computeGpuOcioCommandIdentity(parts);
    if (identity == core::Sha256Digest{}) {
        return failure(GpuOcioCommandError::InvalidArtifact);
    }
    const std::uint64_t retainedBytes = retainedResourceBytes(program, artifact.spirv.size());
    auto command = std::shared_ptr<const PreparedGpuOcioCommand>(
        new PreparedGpuOcioCommand(std::move(program), std::move(artifact), encoding, geometry,
                                   std::move(spirvWords), identity, retainedBytes));
    GpuOcioCommandResult result;
    result.command = std::move(command);
    return result;
}

} // namespace bloom::runtime
