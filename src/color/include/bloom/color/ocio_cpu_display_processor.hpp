#pragma once

#include <bloom/color/display_processor_identity.hpp>
#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/core/sha256.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

// Builds the qualified in-process CPU display processor for a resolved Bloom Neutral v1 config
// (issue #95 design decision 4). Public API is Bloom value types and the existing
// DisplayProcessorIdentityV1 serializer only; OpenColorIO's Processor/CPUProcessor types remain
// private (see src/color/ocio_internal.hpp).

namespace bloom::color {

// Target-specific facts retained for diagnostics and local cache partitioning, per
// docs/architecture/color-management.md's "Qualified Display Intent And Identity"
// (DisplayProcessorExecutionProvenance is explicitly NOT part of the portable
// DisplayProcessorIdentity). dependencyLockDigest and qualifiedPrefixDigest are left empty rather
// than fabricated: docs/architecture/dependency-intake.md states the production prefix manifest
// and its validator remain pending, so this build has no qualified prefix identity to report yet.
struct DisplayProcessorExecutionProvenance final {
    std::string ocioVersion;
    std::string compilerId;
    std::string compilerVersion;
    std::string targetTriple;
    std::string processorCacheId;
    std::optional<core::Sha256Digest> dependencyLockDigest;
    std::optional<core::Sha256Digest> qualifiedPrefixDigest;

    friend bool operator==(const DisplayProcessorExecutionProvenance&,
                           const DisplayProcessorExecutionProvenance&) = default;
};

// The lease-vs-helper-token distinction as a closed variant (design decision 4: "Model the
// lease-vs-helper-token distinction as a closed variant NOW (helper arm unimplemented, typed as
// such) so the helper chapter slots in without an API break"). Only the InProcess arm has a
// public factory in this version; HelperToken names the real, planned arm for supervised external
// configs and is intentionally unreachable until the helper chapter adds its factory and opaque
// token payload.
enum class DisplayProcessorLeaseKind : std::uint8_t {
    InProcess = 1,
    HelperToken = 2,
};

class DisplayProcessorLease final {
  public:
    [[nodiscard]] static constexpr DisplayProcessorLease inProcess() noexcept {
        return DisplayProcessorLease(DisplayProcessorLeaseKind::InProcess);
    }

    [[nodiscard]] constexpr DisplayProcessorLeaseKind kind() const noexcept { return kind_; }

    friend constexpr bool operator==(const DisplayProcessorLease&,
                                     const DisplayProcessorLease&) noexcept = default;

  private:
    explicit constexpr DisplayProcessorLease(const DisplayProcessorLeaseKind kind) noexcept
        : kind_(kind) {}

    DisplayProcessorLeaseKind kind_;
};

enum class OcioBuildProcessorError : std::uint8_t {
    None,
    GetProcessorFailed,
    GetCpuProcessorFailed,
    IdentityConstructionFailed,
};

class PreparedCpuDisplayProcessorHandle;
class OcioBuildProcessorResult;

[[nodiscard]] OcioBuildProcessorResult
buildBloomNeutralCpuDisplayProcessor(const ResolvedBloomNeutralConfig& resolved) noexcept;

// Boundary product 2 of the "CPU Display Processor Boundary" contract: "an immutable
// DisplayProcessorIdentity, execution provenance, and either a qualified in-process built-in
// lease or an opaque helper token". Never contains a raw OCIO object in its public surface.
class PreparedCpuDisplayProcessorHandle final {
  public:
    // Move-construct only, exactly like DisplayProcessorIdentityV1 (one of this class's own
    // members): its DisplayProcessorIdentityV1 member is not move-assignable, so move-assignment
    // is explicitly deleted rather than silently omitted.
    PreparedCpuDisplayProcessorHandle(PreparedCpuDisplayProcessorHandle&&) noexcept;
    PreparedCpuDisplayProcessorHandle& operator=(PreparedCpuDisplayProcessorHandle&&) = delete;
    PreparedCpuDisplayProcessorHandle(const PreparedCpuDisplayProcessorHandle&) = delete;
    PreparedCpuDisplayProcessorHandle& operator=(const PreparedCpuDisplayProcessorHandle&) = delete;
    ~PreparedCpuDisplayProcessorHandle();

    [[nodiscard]] const DisplayProcessorIdentityV1& identity() const& noexcept { return identity_; }
    [[nodiscard]] const DisplayProcessorIdentityV1& identity() const&& = delete;
    [[nodiscard]] const DisplayProcessorExecutionProvenance& provenance() const& noexcept {
        return provenance_;
    }
    [[nodiscard]] const DisplayProcessorExecutionProvenance& provenance() const&& = delete;
    [[nodiscard]] const DisplayProcessorLease& lease() const& noexcept { return lease_; }
    [[nodiscard]] const DisplayProcessorLease& lease() const&& = delete;

    // Opaque handle consumed only by ocio_cpu_display_frame.cpp within this same library.
    class Impl;
    [[nodiscard]] const Impl& impl() const& noexcept { return *impl_; }
    [[nodiscard]] const Impl& impl() const&& = delete;

  private:
    friend OcioBuildProcessorResult
    buildBloomNeutralCpuDisplayProcessor(const ResolvedBloomNeutralConfig&) noexcept;

    PreparedCpuDisplayProcessorHandle(std::unique_ptr<Impl> impl,
                                      DisplayProcessorIdentityV1 identity,
                                      DisplayProcessorExecutionProvenance provenance,
                                      DisplayProcessorLease lease) noexcept;

    std::unique_ptr<Impl> impl_;
    DisplayProcessorIdentityV1 identity_;
    DisplayProcessorExecutionProvenance provenance_;
    DisplayProcessorLease lease_;
};

class [[nodiscard]] OcioBuildProcessorResult final {
  public:
    // Move-construct only: it wraps optional<PreparedCpuDisplayProcessorHandle>, which is itself
    // move-construct only (see PreparedCpuDisplayProcessorHandle above).
    OcioBuildProcessorResult(OcioBuildProcessorResult&&) noexcept = default;
    OcioBuildProcessorResult& operator=(OcioBuildProcessorResult&&) = delete;
    OcioBuildProcessorResult(const OcioBuildProcessorResult&) = delete;
    OcioBuildProcessorResult& operator=(const OcioBuildProcessorResult&) = delete;
    ~OcioBuildProcessorResult() = default;

    [[nodiscard]] bool succeeded() const noexcept { return handle_.has_value(); }
    [[nodiscard]] explicit operator bool() const noexcept { return succeeded(); }
    [[nodiscard]] OcioBuildProcessorError error() const noexcept { return error_; }
    [[nodiscard]] const PreparedCpuDisplayProcessorHandle* handle() const& noexcept {
        return handle_ ? &*handle_ : nullptr;
    }
    [[nodiscard]] const PreparedCpuDisplayProcessorHandle* handle() const&& = delete;
    [[nodiscard]] std::optional<PreparedCpuDisplayProcessorHandle> takeHandle() && noexcept {
        return std::move(handle_);
    }

  private:
    friend OcioBuildProcessorResult
    buildBloomNeutralCpuDisplayProcessor(const ResolvedBloomNeutralConfig&) noexcept;

    explicit OcioBuildProcessorResult(PreparedCpuDisplayProcessorHandle handle) noexcept
        : handle_(std::move(handle)) {}
    explicit OcioBuildProcessorResult(const OcioBuildProcessorError error) noexcept
        : error_(error) {}

    std::optional<PreparedCpuDisplayProcessorHandle> handle_;
    OcioBuildProcessorError error_ = OcioBuildProcessorError::None;
};

} // namespace bloom::color
