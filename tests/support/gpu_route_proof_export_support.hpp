#pragma once

// Qt-free, header-only TEST support for the genuine export route-proof harnesses.
//
// It shares the accepted handoff codec (gpu_route_proof_io.hpp) and adds only what an export proof
// needs on top of a preview proof: a canonical per-frame identity digest, a per-frame paired
// GPU/CPU pixel-comparison evidence record, and the explicit readback submission/payload/byte
// counters a final-render route must report.
//
// Everything is derived from real, executed production products: the ordered per-frame production
// process identity (the attempt's canonical identity bytes where the harness retains the attempt,
// or the real compiled plan/request identity where the facade exposes only the plan), the actual
// GPU payload and the independently decoded CPU reference, and the counters the run reported.
// Nothing is synthesized to look like provenance, and a proof is published only when the typed
// contract and the codec both accept it.
//
// It is compiled only into the export route-proof harness binaries. No production TU includes it.

#include "gpu_route_proof_io.hpp"

#include <bloom/core/sha256.hpp>
#include <bloom/render/image_types.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace bloom::gpu_route_proof_export {

using bloom::gpu_route_proof_io::RouteProofIoStatus;
using bloom::runtime::GpuRouteExecutionProof;
using bloom::runtime::GpuRouteHarnessKind;

// A bounded big-endian canonical byte writer. Every integer field is fixed width and every text
// field is length-prefixed, so two semantically different records cannot encode to the same bytes.
class CanonicalWriter final {
  public:
    void u8(const std::uint8_t value) { bytes_.push_back(static_cast<std::byte>(value)); }
    void boolean(const bool value) { u8(value ? 1U : 0U); }
    void u32(const std::uint32_t value) { appendBigEndian(value); }
    void u64(const std::uint64_t value) { appendBigEndian(value); }
    void i64(const std::int64_t value) { appendBigEndian(static_cast<std::uint64_t>(value)); }
    void f64(const double value) {
        std::uint64_t bits = 0;
        static_assert(sizeof(bits) == sizeof(value));
        std::memcpy(&bits, &value, sizeof(bits));
        appendBigEndian(bits);
    }
    void text(const std::string_view value) {
        u64(value.size());
        const auto* data = reinterpret_cast<const std::byte*>(value.data());
        bytes_.insert(bytes_.end(), data, data + value.size());
    }
    [[nodiscard]] const std::vector<std::byte>& bytes() const noexcept { return bytes_; }

  private:
    template <typename Integer> void appendBigEndian(const Integer value) {
        for (std::size_t index = sizeof(Integer); index-- > 0;) {
            bytes_.push_back(static_cast<std::byte>((value >> (index * 8U)) & 0xFFU));
        }
    }
    std::vector<std::byte> bytes_;
};

[[nodiscard]] inline std::string hexOf(const core::Sha256Digest& digest) {
    const auto hex = digest.toLowercaseHex();
    return std::string{hex.data(), hex.size()};
}

[[nodiscard]] inline std::string sha256Hex(const std::span<const std::byte> bytes) {
    const auto digest = core::Sha256Hasher::hash(bytes);
    return digest.has_value() ? hexOf(*digest) : std::string{};
}

// The exact bytes of a real pixel buffer (Rgba32f process payload or Rgba8 decoded output), hashed
// without copying. Trivially-copyable and standard-layout are required by the image contract.
template <typename Pixel>
[[nodiscard]] inline std::string sha256Pixels(const std::span<const Pixel> pixels) {
    static_assert(std::is_trivially_copyable_v<Pixel> && std::is_standard_layout_v<Pixel>);
    const auto* data = reinterpret_cast<const std::byte*>(pixels.data());
    return sha256Hex({data, pixels.size() * sizeof(Pixel)});
}

// One verified frame's actual comparison record. All fields are observed values from the run.
struct FrameEvidence final {
    // The per-frame canonical production process identity digest (lowercase SHA-256 hex).
    std::string identityHex;
    std::uint64_t comparedPixels = 0;
    // Paired pixels whose decoded values exceeded the route's tolerance (0 for a verified frame).
    std::uint64_t mismatchedPixels = 0;
    // Strongest observed 8-bit decoded channel deviation and float process-component deviation.
    std::uint64_t maxIntegerDelta = 0;
    double maxFloatDelta = 0.0;
    bool alphaExact = true;
    // SHA-256 over the actual CPU reference and actual GPU pixel bytes that were compared.
    std::string cpuDecodedDigest;
    std::string gpuDecodedDigest;
};

// The ordered per-frame production identity digest: the ordered list of real per-frame identity
// digests is itself hashed, so a different order or a different frame cannot collide.
[[nodiscard]] inline std::string
orderedIdentityDigest(const std::span<const std::string> perFrameIdentityHex) {
    CanonicalWriter writer;
    writer.text("bloom.gpu.route.process-identity.export.v1");
    writer.u64(perFrameIdentityHex.size());
    for (const auto& identity : perFrameIdentityHex) {
        writer.text(identity);
    }
    return sha256Hex(writer.bytes());
}

// The evidence digest binds the ordered verified frames to the actual paired GPU/CPU comparison:
// the per-frame identity, the compared pixel count, the observed deviation, and the digests of the
// exact CPU and GPU pixel bytes that produced it.
[[nodiscard]] inline std::string evidenceDigest(const std::span<const FrameEvidence> evidence) {
    CanonicalWriter writer;
    writer.text("bloom.gpu.route.export-evidence.v1");
    writer.u64(evidence.size());
    for (const auto& frame : evidence) {
        writer.text(frame.identityHex);
        writer.u64(frame.comparedPixels);
        writer.u64(frame.mismatchedPixels);
        writer.u64(frame.maxIntegerDelta);
        writer.f64(frame.maxFloatDelta);
        writer.boolean(frame.alphaExact);
        writer.text(frame.cpuDecodedDigest);
        writer.text(frame.gpuDecodedDigest);
    }
    return sha256Hex(writer.bytes());
}

// The explicit final-render transfer figures a proof must report. `readbackSubmissions` is the
// final device-to-host submission count (readbacks); `payloads` is the number of distinct payloads
// those submissions carried; `transferredBytes` is the exact process-payload byte total. The
// accepted readback transfers one process payload per submission today, so a caller with no
// separate production payload counter reports submissions == payloads. When the output-colour
// provenance integration lands and exposes a real two-payload counter, the harness supplies it
// here; it is never guessed upward.
struct ExportProofCounters final {
    std::uint64_t deviceOwnershipEpoch = 0;
    std::uint64_t nativeDispatches = 0;
    std::uint64_t verifiedFrames = 0;
    std::uint64_t readbackSubmissions = 0;
    std::uint64_t payloads = 0;
    std::uint64_t transferredBytes = 0;
};

struct ProofWriteResult final {
    bool written = false;
    std::string detail;
};

// Publishes a genuine export proof. The typed validation runs first, so a zero-native, no-frame, or
// over-policy proof can never be written even by accident; a failed validation or codec write
// reports the exact reason and writes nothing.
[[nodiscard]] inline ProofWriteResult
publishExportProof(const std::filesystem::path& directory, const std::string_view nonce,
                   const std::string_view routeId, const GpuRouteHarnessKind harness,
                   const ExportProofCounters& counters, std::string processIdentityDigestValue,
                   std::string evidenceDigestValue) {
    GpuRouteExecutionProof proof;
    proof.routeId = std::string{routeId};
    proof.harness = harness;
    proof.processIdentityDigest = std::move(processIdentityDigestValue);
    proof.deviceOwnershipEpoch = counters.deviceOwnershipEpoch == 0
                                     ? std::string{}
                                     : std::to_string(counters.deviceOwnershipEpoch);
    proof.nativeDispatches = counters.nativeDispatches;
    proof.verifiedFrames = counters.verifiedFrames;
    proof.readbackSubmissions = counters.readbackSubmissions;
    proof.payloads = counters.payloads;
    proof.transferredBytes = counters.transferredBytes;
    proof.evidenceDigest = std::move(evidenceDigestValue);
    const auto issues = runtime::validateGpuRouteExecutionProof(proof);
    if (!issues.empty()) {
        return {false, issues.front().detail};
    }
    const auto status = bloom::gpu_route_proof_io::writeRouteProof(directory, nonce, proof);
    if (status != RouteProofIoStatus::Ok) {
        return {false, std::string{bloom::gpu_route_proof_io::routeProofIoStatusName(status)}};
    }
    return {true, {}};
}

[[nodiscard]] inline bool readProofNonce(const std::filesystem::path& directory,
                                         std::string& nonce) {
    return bloom::gpu_route_proof_io::readRunNonce(directory, nonce) == RouteProofIoStatus::Ok;
}

// The exact process-payload byte count of `frames` final RGBA32F readbacks of an `width` x `height`
// frame. This is the real size of the payload the accepted readback transfers.
[[nodiscard]] inline std::uint64_t processPayloadBytes(const std::uint64_t frames,
                                                       const std::uint64_t width,
                                                       const std::uint64_t height) noexcept {
    return frames * width * height * sizeof(render::Rgba32f);
}

} // namespace bloom::gpu_route_proof_export
