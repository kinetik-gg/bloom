#pragma once

#include <bloom/render/gpu_processing_phase_budget.hpp>

#include <cstdint>
#include <memory>
#include <optional>

namespace bloom::render {

struct NativeBufferStorageIdentity final {
    std::uint64_t allocatorGeneration = 0;
    std::uint32_t memoryType = 0;
    std::uint32_t heap = 0;
    bool deviceLocal = true;
    friend bool operator==(const NativeBufferStorageIdentity&,
                           const NativeBufferStorageIdentity&) = default;
};

struct NativeBufferStorageGrant;
struct NativeBufferStorageRefusal final {
    GpuProcessingPhaseAdmission status = GpuProcessingPhaseAdmission::InvalidRequest;
    GpuProcessingPhaseDomain domain = GpuProcessingPhaseDomain::None;
    std::uint64_t phaseId = 0;
    std::uint64_t revisionAfterOwnRelease = 0;
};

// One transient buffer suballocation, embedded in its backing-owning RAII object. Actual native
// destruction must precede token replacement/destruction. The existing native owner/quarantine
// retains that entire object; this token never performs native work or publishes cache storage.
class NativeBufferStorageToken final {
  public:
    NativeBufferStorageToken() = default;
    NativeBufferStorageToken(const NativeBufferStorageToken&) = delete;
    NativeBufferStorageToken& operator=(const NativeBufferStorageToken&) = delete;
    NativeBufferStorageToken(NativeBufferStorageToken&&) noexcept;
    NativeBufferStorageToken& operator=(NativeBufferStorageToken&&) noexcept;
    ~NativeBufferStorageToken();

    // Convert an existing native grant and, for a nonlocal heap, its already-planned host debit.
    // Both total claims remain unchanged. The caller independently bounds staging plus output
    // by its declared transfer allowance; no additive host admission or source/output grant lift.
    [[nodiscard]] static NativeBufferStorageGrant
    reserve(const std::shared_ptr<GpuProcessingPhaseLease>&, std::uint64_t upperBytes,
            NativeBufferStorageIdentity) noexcept;
    // Occupy the already-admitted U partition on every heap, plus its existing native debit.
    // Nonlocal staging converts the same protected U host estimate, never output/source credit.
    [[nodiscard]] static NativeBufferStorageGrant
    reserveUpload(const std::shared_ptr<GpuProcessingPhaseLease>&, std::uint64_t upperBytes,
                  NativeBufferStorageIdentity) noexcept;
    [[nodiscard]] std::uint64_t bytes() const noexcept { return bytes_; }
    [[nodiscard]] NativeBufferStorageIdentity identity() const noexcept { return identity_; }
    // Actual allocator size/type/heap/generation must match before bind/map; only shrink is valid.
    [[nodiscard]] bool reconcile(std::uint64_t actualBytes,
                                 NativeBufferStorageIdentity actualIdentity) noexcept;

  private:
    [[nodiscard]] static NativeBufferStorageGrant
    reserveInPartition(const std::shared_ptr<GpuProcessingPhaseLease>&, std::uint64_t,
                       NativeBufferStorageIdentity, bool upload) noexcept;
    NativeBufferStorageToken(std::shared_ptr<GpuProcessingPhaseLease>, std::uint64_t,
                             NativeBufferStorageIdentity, bool upload) noexcept;
    void reset() noexcept;
    std::shared_ptr<detail::GpuProcessingPhaseBudgetState> state_;
    std::shared_ptr<GpuProcessingPhaseLease> origin_;
    std::uint64_t bytes_ = 0;
    NativeBufferStorageIdentity identity_;
    bool upload_ = false;
};

struct NativeBufferStorageGrant final {
    GpuProcessingPhaseAdmission status = GpuProcessingPhaseAdmission::InvalidRequest;
    GpuProcessingPhaseDomain domain = GpuProcessingPhaseDomain::None;
    std::optional<NativeBufferStorageToken> token;
    std::uint64_t revisionAfterOwnRelease = 0;
};
} // namespace bloom::render
