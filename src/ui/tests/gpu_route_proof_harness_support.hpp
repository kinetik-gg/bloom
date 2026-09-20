#pragma once

// Test-only support for the genuine Viewer and RAM-preview GPU route-proof harnesses.
//
// Everything here is derived from real, executed production products: the ordered per-frame
// ProcessFrameIdentity of the frames the real service published, the CPU oracle's own display
// pixels, and the service counters. Nothing is synthesized to look like provenance: the digests
// are SHA-256 over the actual captured records, and a proof is published only when the typed
// contract and the codec both accept it.
//
// It is compiled only into the two route-proof harness binaries. No production TU includes it.

#include "gpu_route_proof_io.hpp"
#include "gpu_service_chain_fixture.hpp"

#include <bloom/core/sha256.hpp>
#include <bloom/render/image_types.hpp>
#include <bloom/runtime/prepared_preview_frame.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace bloom::ui::routeproof {

using bloom::gpu_route_proof_io::RouteProofIoStatus;
using bloom::runtime::GpuRouteExecutionProof;
using bloom::runtime::GpuRouteHarnessKind;
using bloom::runtime::PreparedPreviewFrame;
using PreparedPreviewFrameHandle = std::shared_ptr<const PreparedPreviewFrame>;

// A bounded big-endian canonical byte writer. Every integer field is fixed width, every text field
// is length-prefixed, so two semantically different records cannot encode to the same bytes.
class CanonicalWriter final {
  public:
    void u8(const std::uint8_t value) { bytes_.push_back(static_cast<std::byte>(value)); }
    void u32(const std::uint32_t value) { appendBigEndian(value); }
    void u64(const std::uint64_t value) { appendBigEndian(value); }
    void i64(const std::int64_t value) { appendBigEndian(static_cast<std::uint64_t>(value)); }
    void boolean(const bool value) { u8(value ? 1U : 0U); }
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

// The actual per-frame production process identity: the plan's own key identity and semantics, the
// request time/output/resolution/quality/colour intent, the provider, and the evaluator semantics
// that decided the pixels. It deliberately omits request generation (pixels do not depend on it).
inline void encodeFrameIdentity(const PreparedPreviewFrame& frame, CanonicalWriter& writer) {
    const auto& identity = frame.processIdentity();
    const auto& desired = frame.desiredIdentity();
    writer.text("bloom.gpu.route.frame-identity.v1");
    writer.u64(desired.projectId.value());
    writer.u64(desired.compositionId.value());
    writer.u64(desired.sourceRevision.value());
    const auto* plan = identity.plan.get();
    writer.u64(plan == nullptr ? 0U : plan->projectId().value());
    writer.u64(plan == nullptr ? 0U : plan->compositionId().value());
    writer.u64(plan == nullptr ? 0U : plan->sourceRevision().value());
    writer.u64(plan == nullptr ? 0U : plan->output().value());
    writer.u64(plan == nullptr ? 0U : plan->operations().size());
    writer.u32(plan == nullptr ? 0U : plan->planSemanticsVersion());
    writer.u32(plan == nullptr ? 0U : plan->animationSamplingSemanticsVersion());
    writer.i64(identity.time.numerator());
    writer.i64(identity.time.denominator());
    writer.u64(identity.output.value());
    if (const auto* proxy = std::get_if<runtime::ProxyResolution>(&identity.resolution)) {
        writer.u8(1U);
        writer.u64(proxy->extent.width());
        writer.u64(proxy->extent.height());
    } else {
        writer.u8(0U);
    }
    writer.u8(static_cast<std::uint8_t>(identity.quality));
    writer.text(identity.colorIntent.workingColorSpaceId);
    writer.text(hexOf(identity.colorIntent.ocioConfigRevision));
    writer.text(identity.colorIntent.ocioConfigUri);
    writer.u8(static_cast<std::uint8_t>(identity.provider));
    writer.u32(identity.evaluatorSemanticsVersion);
    writer.u32(identity.animationSamplingSemanticsVersion);
    writer.u32(identity.imagePrimitiveSemanticsVersion);
    writer.boolean(identity.roi.has_value());
    if (identity.roi.has_value()) {
        writer.i64(identity.roi->originX());
        writer.i64(identity.roi->originY());
        writer.u64(identity.roi->extent().width());
        writer.u64(identity.roi->extent().height());
    }
    writer.boolean(identity.bypassLookNodes);
}

// The ordered per-frame identity digest: each frame is first reduced to its own canonical identity
// digest, then the ordered list is hashed. A different order or a different frame cannot collide.
[[nodiscard]] inline std::string
frameIdentityDigest(const std::span<const PreparedPreviewFrameHandle> frames) {
    CanonicalWriter writer;
    writer.text("bloom.gpu.route.process-identity.v1");
    writer.u64(frames.size());
    for (const auto& frame : frames) {
        CanonicalWriter frameWriter;
        encodeFrameIdentity(*frame, frameWriter);
        writer.text(sha256Hex(frameWriter.bytes()));
    }
    return sha256Hex(writer.bytes());
}

// One paired GPU/CPU sample at an image-space pixel: the actual value copied out of the resident
// GPU display image and the CPU oracle's value at the same coordinate.
struct SparseSample final {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    render::Rgba8 cpu{};
    render::Rgba8 gpu{};
};

// A bounded set of image-space coordinates: known interior points plus, when the frame has them, an
// alpha-edge pair, an opaque content pixel, and a fully transparent pixel. Deterministic order, so
// the evidence digest is reproducible.
[[nodiscard]] inline std::vector<render::ImagePixelCoordinate>
selectSampleCoordinates(const PreparedPreviewFrame& cpuFrame, const std::size_t maximum = 8) {
    std::vector<render::ImagePixelCoordinate> coordinates;
    const auto view = cpuFrame.displayBufferView();
    if (!view.has_value()) {
        return coordinates;
    }
    const auto width = view->displayWindow.extent().width();
    const auto height = view->displayWindow.extent().height();
    if (width == 0 || height == 0) {
        return coordinates;
    }
    const auto& pixels = view->pixels;
    const auto add = [&](const std::uint32_t x, const std::uint32_t y) {
        if (x >= width || y >= height || coordinates.size() >= maximum) {
            return;
        }
        for (const auto& existing : coordinates) {
            if (existing.x == x && existing.y == y) {
                return;
            }
        }
        coordinates.push_back(render::ImagePixelCoordinate{x, y});
    };
    const std::array<std::array<double, 2>, 4> fractions{
        std::array<double, 2>{0.13, 0.17}, std::array<double, 2>{0.37, 0.19},
        std::array<double, 2>{0.81, 0.77}, std::array<double, 2>{0.47, 0.33}};
    for (const auto& fraction : fractions) {
        add(static_cast<std::uint32_t>(fraction[0] * static_cast<double>(width)),
            static_cast<std::uint32_t>(fraction[1] * static_cast<double>(height)));
    }
    // The first adjacent pair whose alpha crosses zero, so an alpha-edge defect is sampled.
    bool edgeFound = false;
    for (std::uint32_t y = 0; y < height && !edgeFound; ++y) {
        for (std::uint32_t x = 1; x < width; ++x) {
            const auto left = pixels[static_cast<std::size_t>(y) * width + (x - 1)].alpha;
            const auto right = pixels[static_cast<std::size_t>(y) * width + x].alpha;
            if ((left == 0) != (right == 0)) {
                add(x - 1, y);
                add(x, y);
                edgeFound = true;
                break;
            }
        }
    }
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            if (pixels[static_cast<std::size_t>(y) * width + x].alpha == 255U) {
                add(x, y);
                y = height;
                break;
            }
        }
    }
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            if (pixels[static_cast<std::size_t>(y) * width + x].alpha == 0U) {
                add(x, y);
                y = height;
                break;
            }
        }
    }
    return coordinates;
}

// The per-frame verification evidence: the production identity digest plus paired actual-GPU and
// CPU-oracle sparse samples. It is produced only when the real owner-thread sparse readback ran and
// every sampled GPU value matched the CPU oracle within one code (RGB) and exactly (alpha). The
// sparse byte/submission totals are test-only and never folded into a production counter.
struct FrameEvidence final {
    std::string identityHex;
    std::size_t oraclePixelCount = 0;
    std::vector<SparseSample> samples;
    std::uint64_t sparseBytes = 0;
    std::uint64_t sparseSubmissions = 0;
};

[[nodiscard]] inline bool buildFrameEvidence(runtime::GpuPreviewDisplayService& service,
                                             const PreparedPreviewFrame& gpuFrame,
                                             const PreparedPreviewFrame& cpuFrame,
                                             FrameEvidence& out, std::string& detail) {
    out = FrameEvidence{};
    const auto coordinates = selectSampleCoordinates(cpuFrame);
    if (coordinates.empty()) {
        detail = "no sparse sample coordinates were available";
        return false;
    }
    const auto view = cpuFrame.displayBufferView();
    if (!view.has_value()) {
        detail = "the CPU oracle has no display buffer";
        return false;
    }
    if (gpuFrame.provenance().provider != runtime::PreviewDisplayProvider::GpuResident ||
        gpuFrame.residentFrame() == nullptr) {
        detail = "the GPU frame is not a resident frame";
        return false;
    }
    // The sparse copy runs as one bounded owner-thread task; retry once so a transient owner-thread
    // stall cannot lose an otherwise genuine proof. A real absence still fails after both attempts.
    runtime::detail::ResidentSparseSampleResult sampled;
    bool sampledOk = false;
    std::string firstAttemptDiagnostic;
    for (int attempt = 0; attempt < 2 && !sampledOk; ++attempt) {
        sampledOk = runtime::detail::GpuPreviewDisplayServiceTestAccess::sampleResidentFrameSparse(
                        service, gpuFrame.residentFrame()->lease(), coordinates, sampled,
                        std::chrono::seconds(30)) &&
                    sampled.pixels.size() == coordinates.size();
        if (!sampledOk) {
            if (attempt == 0) {
                firstAttemptDiagnostic = sampled.diagnostic;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }
    if (!sampledOk) {
        detail = "the GPU sparse readback was unavailable: " + sampled.diagnostic;
        return false;
    }
    if (!firstAttemptDiagnostic.empty()) {
        // Surface a transient stall rather than hiding it: the retry still had to produce real,
        // matching samples before any proof may be published.
        std::cout << "PHASE sparse-readback-retry firstAttempt=" << firstAttemptDiagnostic << '\n';
    }
    const auto width = view->displayWindow.extent().width();
    const auto channelDelta = [](const std::uint8_t a, const std::uint8_t b) {
        return std::abs(static_cast<int>(a) - static_cast<int>(b));
    };
    for (std::size_t index = 0; index < coordinates.size(); ++index) {
        const auto x = coordinates[index].x;
        const auto y = coordinates[index].y;
        const auto cpu = view->pixels[static_cast<std::size_t>(y) * width + x];
        const auto gpu = sampled.pixels[index];
        if (channelDelta(gpu.red, cpu.red) > 1 || channelDelta(gpu.green, cpu.green) > 1 ||
            channelDelta(gpu.blue, cpu.blue) > 1 || gpu.alpha != cpu.alpha) {
            detail = "the actual GPU pixel differs from the CPU oracle at (" + std::to_string(x) +
                     "," + std::to_string(y) + ")";
            return false;
        }
        out.samples.push_back(SparseSample{x, y, cpu, gpu});
    }
    out.oraclePixelCount = view->pixels.size();
    out.sparseBytes = coordinates.size() * sizeof(render::Rgba8);
    out.sparseSubmissions = 1;
    CanonicalWriter frameWriter;
    encodeFrameIdentity(gpuFrame, frameWriter);
    out.identityHex = sha256Hex(frameWriter.bytes());
    return true;
}

[[nodiscard]] inline std::string evidenceDigest(const std::span<const FrameEvidence> evidence) {
    CanonicalWriter writer;
    writer.text("bloom.gpu.route.evidence.v2");
    writer.u64(evidence.size());
    for (const auto& frame : evidence) {
        writer.text(frame.identityHex);
        writer.u64(frame.oraclePixelCount);
        writer.u64(frame.samples.size());
        for (const auto& sample : frame.samples) {
            writer.u32(sample.x);
            writer.u32(sample.y);
            writer.u8(sample.cpu.red);
            writer.u8(sample.cpu.green);
            writer.u8(sample.cpu.blue);
            writer.u8(sample.cpu.alpha);
            writer.u8(sample.gpu.red);
            writer.u8(sample.gpu.green);
            writer.u8(sample.gpu.blue);
            writer.u8(sample.gpu.alpha);
        }
        writer.u64(frame.sparseBytes);
        writer.u64(frame.sparseSubmissions);
    }
    return sha256Hex(writer.bytes());
}

// The production plan and per-frame request identity must agree with the CPU oracle's: same deep
// plan content, same time/output/quality/colour intent, and the same evaluated geometry. The actual
// GPU pixels are verified separately by buildFrameEvidence above.
[[nodiscard]] inline bool sameOracleIdentity(const PreparedPreviewFrame& gpu,
                                             const PreparedPreviewFrame& cpu, std::string& detail) {
    const auto& gpuIdentity = gpu.processIdentity();
    const auto& cpuIdentity = cpu.processIdentity();
    if (gpuIdentity.plan == nullptr || cpuIdentity.plan == nullptr) {
        detail = "a frame has no retained plan";
        return false;
    }
    if (!(*gpuIdentity.plan == *cpuIdentity.plan)) {
        detail = "the production plan differs from the CPU oracle's plan";
        return false;
    }
    if (gpuIdentity.time != cpuIdentity.time ||
        gpuIdentity.output.value() != cpuIdentity.output.value() ||
        gpuIdentity.quality != cpuIdentity.quality ||
        gpuIdentity.colorIntent != cpuIdentity.colorIntent || gpuIdentity.roi != cpuIdentity.roi ||
        gpuIdentity.bypassLookNodes != cpuIdentity.bypassLookNodes) {
        detail = "the production request identity differs from the CPU oracle's";
        return false;
    }
    const auto gpuBounds = gpu.evaluatedBounds();
    const auto cpuBounds = cpu.evaluatedBounds();
    if (gpuBounds.size() != cpuBounds.size() ||
        !std::equal(gpuBounds.begin(), gpuBounds.end(), cpuBounds.begin())) {
        detail = "the production evaluated geometry differs from the CPU oracle's";
        return false;
    }
    return true;
}

struct ProofWriteResult final {
    bool written = false;
    std::string detail;
};

// Builds the preview proof from the genuine captured values and publishes it through the codec.
// The typed validation runs first, so a zero-dispatch / no-frame / readback-bearing preview proof
// can never be written even by accident. Preview routes carry no readback fields.
[[nodiscard]] inline ProofWriteResult
publishPreviewProof(const std::filesystem::path& directory, const std::string_view nonce,
                    const std::string_view routeId, const GpuRouteHarnessKind harness,
                    const std::uint64_t deviceOwnershipEpoch, const std::uint64_t coldDispatches,
                    const std::uint64_t verifiedFrames, std::string processIdentityDigest,
                    std::string evidenceDigestValue) {
    GpuRouteExecutionProof proof;
    proof.routeId = std::string{routeId};
    proof.harness = harness;
    proof.processIdentityDigest = std::move(processIdentityDigest);
    proof.deviceOwnershipEpoch = std::to_string(deviceOwnershipEpoch);
    proof.nativeDispatches = coldDispatches;
    proof.verifiedFrames = verifiedFrames;
    proof.readbackSubmissions = 0;
    proof.payloads = 0;
    proof.transferredBytes = 0;
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

} // namespace bloom::ui::routeproof
