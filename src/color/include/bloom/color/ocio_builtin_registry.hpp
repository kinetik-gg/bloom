#pragma once

#include <bloom/color/bloom_neutral_builtin.hpp>
#include <bloom/core/sha256.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

// The Bloom Neutral v1 in-process built-in OCIO registry (issue #95, design decision 2 of the
// task package): resolves EXACTLY the URI bloom://ocio/neutral-v1/config.ocio to the payload
// embedded at build time from assets/ocio/neutral-v1/config.ocio, per
// docs/architecture/color-management.md's "Durable OCIO Configuration Identity" and "Supervised
// OCIO Execution" (Bloom Neutral is the sole in-process trust class; every other locator kind
// remains routed to the not-yet-implemented bloom-color-worker helper). This header exposes only
// Bloom value types; OpenColorIO's Config/ColorSpace/Processor types never appear here (contract
// "CPU Display Processor Boundary": "OCIO classes, exceptions, pointers, and enums remain
// private").

namespace bloom::color {

// The four locator kinds named in color-management.md's "Durable OCIO Configuration Identity"
// table, in that table's order. Only BloomBuiltIn is implemented by this in-process registry;
// the other three name real, planned locator kinds that this version routes honestly to the
// helper rather than silently reporting Missing or Invalid for them.
enum class OcioConfigLocatorKind : std::uint8_t {
    BloomBuiltIn = 1,
    ProjectRelativeArchive = 2,
    ExternalArchive = 3,
    ExternalLooseConfig = 4,
};

enum class OcioBuiltInRegistryOutcome : std::uint8_t {
    // Locator resolves and matches the expected revision; OCIO parsed and validated the payload,
    // and it exposes exactly one unambiguous lin_rec709_scene process mapping and exactly one
    // unambiguous srgb_rec709_display output mapping reachable through exactly one display/view.
    Ready,
    // The locator is the exact recognized built-in URI, but the recomputed kind-1 content
    // revision differs from the caller's expected revision.
    Changed,
    // The locator kind is BloomBuiltIn but the URI text is not the exact recognized built-in URI.
    Missing,
    // The URI and revision matched, but OCIO could not parse or validate the payload, or it does
    // not expose the exact unambiguous interop-ID mappings this registry requires. See
    // invalidReason() for the specific cause.
    Invalid,
    // locatorKind names a real, planned locator kind (project-relative/external archive, or
    // external loose config) that this in-process registry never resolves. Those inputs are
    // supervised-helper-only per docs/architecture/color-management.md's "Supervised OCIO
    // Execution"; the not-yet-implemented bloom-color-worker helper is the pending route. This is
    // a distinct, honestly-named state -- never collapsed into Missing or Invalid.
    LocatorKindRequiresHelper,
};

enum class OcioBuiltInInvalidReason : std::uint8_t {
    // Meaningful only when outcome() == Invalid; None otherwise.
    None,
    ParseFailed,
    ValidateFailed,
    ProcessColorSpaceNotUniquelyMapped,
    OutputColorSpaceNotUniquelyMapped,
    DisplayViewNotUniquelyMapped,
};

class ResolvedBloomNeutralConfig;
class OcioBuiltInResolutionResult;

// Attempts to resolve one OcioConfigReference-shaped request against the in-process Bloom
// Neutral v1 built-in registry. `locatorValue` is the caller's exact locator text; it is compared
// byte-for-byte against kBloomNeutralV1ConfigUri, never normalized, aliased, or matched
// approximately. `expectedRevision` is the caller's persisted OcioConfigReference.expectedRevision
// value. This function never touches the filesystem, environment, or a search path; the embedded
// payload is a build-time constant.
[[nodiscard]] OcioBuiltInResolutionResult
resolveBloomNeutralV1BuiltIn(OcioConfigLocatorKind locatorKind, std::string_view locatorValue,
                             const core::Sha256Digest& expectedRevision) noexcept;

// The exact byte span of the payload embedded at build time from
// assets/ocio/neutral-v1/config.ocio. Exposed so tests and the registry itself can verify the
// embedded payload's digest equals bloom::color::kBloomNeutralV1ConfigDigest without a runtime
// filesystem read.
[[nodiscard]] std::span<const std::byte> bloomNeutralV1EmbeddedPayload() noexcept;

// Immutable, move-only successful-resolution product. Retains the parsed and validated OCIO
// config privately (never exposed) plus the exact discovered interop-ID mapping this registry
// used, per the contract's "exact unambiguous mapping" rule: process/output color-space names are
// discovered by scanning every non-data color space's declared interop ID
// (OCIO::ColorSpace::getInteropID()), not by matching a name, role, or alias; the display/view
// pair is discovered by scanning every (display, view) pair's resolved color space
// (OCIO::Config::getDisplayViewColorSpaceName()) against the discovered output color-space name.
class ResolvedBloomNeutralConfig final {
  public:
    ResolvedBloomNeutralConfig(ResolvedBloomNeutralConfig&&) noexcept;
    ResolvedBloomNeutralConfig& operator=(ResolvedBloomNeutralConfig&&) noexcept;
    ResolvedBloomNeutralConfig(const ResolvedBloomNeutralConfig&) = delete;
    ResolvedBloomNeutralConfig& operator=(const ResolvedBloomNeutralConfig&) = delete;
    ~ResolvedBloomNeutralConfig();

    [[nodiscard]] const core::Sha256Digest& expectedRevision() const& noexcept;
    [[nodiscard]] const core::Sha256Digest& expectedRevision() const&& = delete;

    // Exactly "lin_rec709_scene" and "srgb_rec709_display" for this frozen asset; discovered, not
    // hardcoded, by the mechanism documented above.
    [[nodiscard]] std::string_view processColorSpaceId() const& noexcept;
    [[nodiscard]] std::string_view processColorSpaceId() const&& = delete;
    [[nodiscard]] std::string_view outputColorSpaceId() const& noexcept;
    [[nodiscard]] std::string_view outputColorSpaceId() const&& = delete;
    [[nodiscard]] std::string_view displayName() const& noexcept;
    [[nodiscard]] std::string_view displayName() const&& = delete;
    [[nodiscard]] std::string_view viewName() const& noexcept;
    [[nodiscard]] std::string_view viewName() const&& = delete;

    // Opaque handle consumed only by ocio_cpu_display_processor.hpp's processor builder within
    // this same library; not part of the public color-value surface.
    class Impl;
    [[nodiscard]] const Impl& impl() const& noexcept;
    [[nodiscard]] const Impl& impl() const&& = delete;

  private:
    friend OcioBuiltInResolutionResult
    resolveBloomNeutralV1BuiltIn(OcioConfigLocatorKind, std::string_view,
                                 const core::Sha256Digest&) noexcept;

    ResolvedBloomNeutralConfig(std::unique_ptr<Impl> impl, core::Sha256Digest expectedRevision,
                               std::string processColorSpaceId, std::string outputColorSpaceId,
                               std::string displayName, std::string viewName) noexcept;

    std::unique_ptr<Impl> impl_;
    core::Sha256Digest expectedRevision_;
    std::string processColorSpaceId_;
    std::string outputColorSpaceId_;
    std::string displayName_;
    std::string viewName_;
};

class [[nodiscard]] OcioBuiltInResolutionResult final {
  public:
    OcioBuiltInResolutionResult(OcioBuiltInResolutionResult&&) noexcept;
    OcioBuiltInResolutionResult& operator=(OcioBuiltInResolutionResult&&) noexcept;
    OcioBuiltInResolutionResult(const OcioBuiltInResolutionResult&) = delete;
    OcioBuiltInResolutionResult& operator=(const OcioBuiltInResolutionResult&) = delete;
    ~OcioBuiltInResolutionResult();

    [[nodiscard]] OcioBuiltInRegistryOutcome outcome() const noexcept { return outcome_; }
    [[nodiscard]] bool ready() const noexcept {
        return outcome_ == OcioBuiltInRegistryOutcome::Ready;
    }
    [[nodiscard]] explicit operator bool() const noexcept { return ready(); }
    // Meaningful only when outcome() == Invalid.
    [[nodiscard]] OcioBuiltInInvalidReason invalidReason() const noexcept { return invalidReason_; }
    // The recomputed kind-1 content revision, when it could be computed (Ready and Changed).
    [[nodiscard]] const std::optional<core::Sha256Digest>& recomputedRevision() const& noexcept {
        return recomputedRevision_;
    }
    [[nodiscard]] const std::optional<core::Sha256Digest>& recomputedRevision() const&& = delete;
    // Non-null exactly when outcome() == Ready. Takeable exactly once.
    [[nodiscard]] const ResolvedBloomNeutralConfig* resolved() const& noexcept {
        return resolved_ ? &*resolved_ : nullptr;
    }
    [[nodiscard]] const ResolvedBloomNeutralConfig* resolved() const&& = delete;
    [[nodiscard]] std::optional<ResolvedBloomNeutralConfig> takeResolved() && noexcept {
        return std::move(resolved_);
    }

  private:
    friend OcioBuiltInResolutionResult
    resolveBloomNeutralV1BuiltIn(OcioConfigLocatorKind, std::string_view,
                                 const core::Sha256Digest&) noexcept;

    explicit OcioBuiltInResolutionResult(OcioBuiltInRegistryOutcome outcome) noexcept
        : outcome_(outcome) {}

    OcioBuiltInRegistryOutcome outcome_;
    OcioBuiltInInvalidReason invalidReason_ = OcioBuiltInInvalidReason::None;
    std::optional<core::Sha256Digest> recomputedRevision_;
    std::optional<ResolvedBloomNeutralConfig> resolved_;
};

} // namespace bloom::color
