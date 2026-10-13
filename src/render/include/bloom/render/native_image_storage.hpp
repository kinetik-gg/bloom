#pragma once

#include <bloom/render/gpu_processing_phase_budget.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>

namespace bloom::render {

// Actual allocator generation and selected physical-memory type/heap, resolved before backing
// allocation. No Vulkan handles, device ownership or API types enter this neutral authority.
struct NativeImageStorageIdentity final {
    std::uint64_t allocatorGeneration = 0;
    std::uint32_t memoryType = 0;
    std::uint32_t heap = 0;
    bool deviceLocal = true;
    friend bool operator==(const NativeImageStorageIdentity&,
                           const NativeImageStorageIdentity&) = default;
};

struct NativeImageStorageGrant;
struct NativeImageStorageRefusal final {
    GpuProcessingPhaseAdmission status = GpuProcessingPhaseAdmission::InvalidRequest;
    GpuProcessingPhaseDomain domain = GpuProcessingPhaseDomain::None;
    std::uint64_t phaseId = 0;
    std::uint64_t revisionAfterOwnRelease = 0;
};

// One covered native IMAGE suballocation. Embed this token in the backing-owning opaque impl.
// Destroy backing BEFORE token destruction/replacement; orphan publication or owner close is not
// destruction proof. Aliases of the impl share one token. It never owns a device/native reclaimer.
class NativeImageStorageToken final {
  public:
    NativeImageStorageToken() = default;
    NativeImageStorageToken(const NativeImageStorageToken&) = delete;
    NativeImageStorageToken& operator=(const NativeImageStorageToken&) = delete;
    NativeImageStorageToken(NativeImageStorageToken&&) noexcept;
    NativeImageStorageToken& operator=(NativeImageStorageToken&&) noexcept;
    ~NativeImageStorageToken();

    [[nodiscard]] static NativeImageStorageGrant
    reserve(const std::shared_ptr<GpuProcessingPhaseLease>&, std::uint64_t upperBytes,
            NativeImageStorageIdentity,
            std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) noexcept;
    [[nodiscard]] static NativeImageStorageGrant reservePresentation(
        const GpuProcessingRootIdentity&, std::uint64_t upperBytes, NativeImageStorageIdentity,
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) noexcept;
    [[nodiscard]] std::uint64_t bytes() const noexcept { return bytes_; }
    [[nodiscard]] NativeImageStorageIdentity identity() const noexcept { return identity_; }
    [[nodiscard]] bool published() const noexcept { return published_; }

    // Verify actual allocation BEFORE bind/view; never top up after allocation. Only provisional,
    // downward reconciliation with the exact admitted generation/type/heap/domain is permitted.
    [[nodiscard]] bool reconcile(std::uint64_t actualBytes,
                                 NativeImageStorageIdentity actualIdentity) noexcept;
    // Successful retention promotes this image only. Handoff itself changes no total/revision;
    // dropping the final phase alias outside the lock can release remaining phase/count normally.
    [[nodiscard]] bool promote() noexcept;
    // Explicit owning-failure proof, NOT inferred from a live origin. Caller must have destroyed
    // backing and proved the attempt owned its last INTERNAL alias, with no cache/external holder.
    // Exact-root/id failure returns false unchanged. Independent Published disposal always revises.
    [[nodiscard]] bool releaseFailedAttemptAfterBackingDestroyed(
        const std::shared_ptr<GpuProcessingPhaseLease>& exactOrigin) noexcept;

  private:
    NativeImageStorageToken(std::shared_ptr<GpuProcessingPhaseLease>, std::uint64_t,
                            NativeImageStorageIdentity) noexcept;
    NativeImageStorageToken(GpuProcessingRootIdentity, std::uint64_t,
                            NativeImageStorageIdentity) noexcept;
    void reset() noexcept;
    std::shared_ptr<detail::GpuProcessingPhaseBudgetState> state_;
    std::shared_ptr<GpuProcessingPhaseLease> origin_;
    // Published backing does not pin a request. Actual free can replenish its still-live original
    // preadmitted peak envelope; an expired origin releases total credit normally.
    std::weak_ptr<GpuProcessingPhaseLease> publishedOrigin_;
    std::uint64_t originId_ = 0;
    std::uint64_t bytes_ = 0;
    NativeImageStorageIdentity identity_;
    bool published_ = false;
    bool presentation_ = false;
};

struct NativeImageStorageGrant final {
    GpuProcessingPhaseAdmission status = GpuProcessingPhaseAdmission::InvalidRequest;
    GpuProcessingPhaseDomain domain = GpuProcessingPhaseDomain::None;
    std::optional<NativeImageStorageToken> token;
    // Capacity refusal watermark excludes only this originating phase's final release. It never
    // hides independent releases, including those before failed-result delivery.
    std::uint64_t revisionAfterOwnRelease = 0;
};
} // namespace bloom::render
