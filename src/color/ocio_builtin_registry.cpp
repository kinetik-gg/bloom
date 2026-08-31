#include <bloom/color/ocio_builtin_registry.hpp>

#include "ocio_internal.hpp"
#include <bloom/color/display_processor_identity.hpp>
#include <bloom/color/ocio_content_revision.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include "ocio_builtin_payload.inc" // NOLINT(bugprone-suspicious-include) -- generated array data;
                                    // opens/closes namespace bloom::color::detail itself

namespace {

[[nodiscard]] std::span<const std::byte> embeddedPayloadBytes() noexcept {
    return std::as_bytes(
        std::span(bloom::color::detail::kBloomNeutralV1ConfigPayloadBytes,
                  sizeof(bloom::color::detail::kBloomNeutralV1ConfigPayloadBytes)));
}

// Discovers the exact color-space name whose declared OCIO interop ID (ColorSpace::getInteropID)
// equals `interopId`, scanning every non-data color space in the config. Per the contract's "exact
// unambiguous mapping" rule, zero or more than one match is reported as absent -- a name, role, or
// alias match is never substituted.
[[nodiscard]] std::optional<std::string>
findUniqueColorSpaceByInteropId(const OCIO::ConstConfigRcPtr& config,
                                const std::string_view interopId) {
    std::optional<std::string> found;
    const int count = config->getNumColorSpaces();
    for (int index = 0; index < count; ++index) {
        const char* const name = config->getColorSpaceNameByIndex(index);
        if (name == nullptr) {
            continue;
        }
        const OCIO::ConstColorSpaceRcPtr colorSpace = config->getColorSpace(name);
        if (!colorSpace || colorSpace->isData()) {
            continue;
        }
        const char* const interop = colorSpace->getInteropID();
        if (interop == nullptr || std::string_view(interop) != interopId) {
            continue;
        }
        if (found.has_value()) {
            return std::nullopt; // ambiguous: more than one color space declares this interop ID
        }
        found = std::string(name);
    }
    return found;
}

struct DisplayViewPair final {
    std::string display;
    std::string view;
};

// Discovers the exact (display, view) pair whose resolved color space
// (Config::getDisplayViewColorSpaceName) equals `outputColorSpaceName`, scanning every display and
// view in the config. Requires exactly one match across the whole config.
[[nodiscard]] std::optional<DisplayViewPair>
findUniqueDisplayViewForColorSpace(const OCIO::ConstConfigRcPtr& config,
                                   const std::string_view outputColorSpaceName) {
    std::optional<DisplayViewPair> found;
    const int displayCount = config->getNumDisplays();
    for (int displayIndex = 0; displayIndex < displayCount; ++displayIndex) {
        const char* const display = config->getDisplay(displayIndex);
        if (display == nullptr) {
            continue;
        }
        const int viewCount = config->getNumViews(display);
        for (int viewIndex = 0; viewIndex < viewCount; ++viewIndex) {
            const char* const view = config->getView(display, viewIndex);
            if (view == nullptr) {
                continue;
            }
            const char* const colorSpaceName = config->getDisplayViewColorSpaceName(display, view);
            if (colorSpaceName == nullptr ||
                std::string_view(colorSpaceName) != outputColorSpaceName) {
                continue;
            }
            if (found.has_value()) {
                return std::nullopt; // ambiguous: more than one (display, view) resolves here
            }
            found = DisplayViewPair{std::string(display), std::string(view)};
        }
    }
    return found;
}

} // namespace

namespace bloom::color {

ResolvedBloomNeutralConfig::ResolvedBloomNeutralConfig(std::unique_ptr<Impl> impl,
                                                       core::Sha256Digest expectedRevision,
                                                       std::string processColorSpaceId,
                                                       std::string outputColorSpaceId,
                                                       std::string displayName,
                                                       std::string viewName) noexcept
    : impl_(std::move(impl)), expectedRevision_(expectedRevision),
      processColorSpaceId_(std::move(processColorSpaceId)),
      outputColorSpaceId_(std::move(outputColorSpaceId)), displayName_(std::move(displayName)),
      viewName_(std::move(viewName)) {}

ResolvedBloomNeutralConfig::ResolvedBloomNeutralConfig(ResolvedBloomNeutralConfig&&) noexcept =
    default;
ResolvedBloomNeutralConfig&
ResolvedBloomNeutralConfig::operator=(ResolvedBloomNeutralConfig&&) noexcept = default;
ResolvedBloomNeutralConfig::~ResolvedBloomNeutralConfig() = default;

const core::Sha256Digest& ResolvedBloomNeutralConfig::expectedRevision() const& noexcept {
    return expectedRevision_;
}
std::string_view ResolvedBloomNeutralConfig::processColorSpaceId() const& noexcept {
    return processColorSpaceId_;
}
std::string_view ResolvedBloomNeutralConfig::outputColorSpaceId() const& noexcept {
    return outputColorSpaceId_;
}
std::string_view ResolvedBloomNeutralConfig::displayName() const& noexcept { return displayName_; }
std::string_view ResolvedBloomNeutralConfig::viewName() const& noexcept { return viewName_; }
const ResolvedBloomNeutralConfig::Impl& ResolvedBloomNeutralConfig::impl() const& noexcept {
    return *impl_;
}

OcioBuiltInResolutionResult::OcioBuiltInResolutionResult(OcioBuiltInResolutionResult&&) noexcept =
    default;
OcioBuiltInResolutionResult&
OcioBuiltInResolutionResult::operator=(OcioBuiltInResolutionResult&&) noexcept = default;
OcioBuiltInResolutionResult::~OcioBuiltInResolutionResult() = default;

std::span<const std::byte> bloomNeutralV1EmbeddedPayload() noexcept {
    return embeddedPayloadBytes();
}

OcioBuiltInResolutionResult
resolveBloomNeutralV1BuiltIn(const OcioConfigLocatorKind locatorKind,
                             const std::string_view locatorValue,
                             const core::Sha256Digest& expectedRevision) noexcept {
    if (locatorKind != OcioConfigLocatorKind::BloomBuiltIn) {
        // A real, planned locator kind this in-process registry never resolves -- see
        // OcioBuiltInRegistryOutcome::LocatorKindRequiresHelper.
        return OcioBuiltInResolutionResult(OcioBuiltInRegistryOutcome::LocatorKindRequiresHelper);
    }
    if (locatorValue != kBloomNeutralV1ConfigUri) {
        return OcioBuiltInResolutionResult(OcioBuiltInRegistryOutcome::Missing);
    }

    const auto payload = embeddedPayloadBytes();
    const auto revisionResult =
        computeOcioContentRevisionV1(OcioContentLocatorKind::BuiltIn, payload);
    if (!revisionResult) {
        // The embedded payload is a fixed, small, checked-in build-time constant; a
        // revision-computation failure here is an internal invariant violation, never a
        // hostile-input case, so it is reported Invalid rather than crashing or fabricating Ready.
        auto result = OcioBuiltInResolutionResult(OcioBuiltInRegistryOutcome::Invalid);
        result.invalidReason_ = OcioBuiltInInvalidReason::ParseFailed;
        return result;
    }
    const auto recomputed = *revisionResult.revision();

    if (recomputed != expectedRevision) {
        auto result = OcioBuiltInResolutionResult(OcioBuiltInRegistryOutcome::Changed);
        result.recomputedRevision_ = recomputed;
        return result;
    }

    // Match -- proceed to OCIO parse/validation from the embedded bytes via the memory/stream
    // API only; never a file path, and no environment, working-directory, or search-path
    // influence.
    OCIO::ConstConfigRcPtr config;
    try {
        std::istringstream stream(
            std::string(reinterpret_cast<const char*>(payload.data()), payload.size()));
        config = OCIO::Config::CreateFromStream(stream);
        config->validate();
    } catch (const OCIO::Exception&) {
        auto result = OcioBuiltInResolutionResult(OcioBuiltInRegistryOutcome::Invalid);
        result.recomputedRevision_ = recomputed;
        result.invalidReason_ = OcioBuiltInInvalidReason::ParseFailed;
        return result;
    } catch (const std::exception&) {
        auto result = OcioBuiltInResolutionResult(OcioBuiltInRegistryOutcome::Invalid);
        result.recomputedRevision_ = recomputed;
        result.invalidReason_ = OcioBuiltInInvalidReason::ValidateFailed;
        return result;
    }

    // Assert the config declares exactly the empty recorded environment-variable set: this is
    // Config::getNumEnvironmentVars(), the config's OWN "environment:" YAML section (empty for
    // this asset -- see assets/ocio/neutral-v1/config.ocio), not Config::getCurrentContext(),
    // which OCIO auto-populates from every process environment variable by default
    // (EnvironmentMode::ENV_ENVIRONMENT_LOAD_ALL, observed ~90 entries on a real developer
    // shell) regardless of what the config file declares or references. That auto-population is
    // therefore not "no environment influence entering resolution" by itself; this registry
    // relies instead on never invoking loadEnvironment() and on
    // buildBloomNeutralCpuDisplayProcessor (ocio_cpu_display_processor.cpp) explicitly
    // constructing and passing a freshly created, never-loaded OCIO::Context to every
    // Config::getProcessor call, so the actual transform build and application never consult
    // ambient environment variables regardless of the config's own default context population.
    if (config->getNumEnvironmentVars() != 0) {
        auto result = OcioBuiltInResolutionResult(OcioBuiltInRegistryOutcome::Invalid);
        result.recomputedRevision_ = recomputed;
        result.invalidReason_ = OcioBuiltInInvalidReason::ValidateFailed;
        return result;
    }

    const auto processColorSpace =
        findUniqueColorSpaceByInteropId(config, kDisplayProcessorIdentitySourceColorSpaceId);
    if (!processColorSpace.has_value()) {
        auto result = OcioBuiltInResolutionResult(OcioBuiltInRegistryOutcome::Invalid);
        result.recomputedRevision_ = recomputed;
        result.invalidReason_ = OcioBuiltInInvalidReason::ProcessColorSpaceNotUniquelyMapped;
        return result;
    }
    const auto outputColorSpace =
        findUniqueColorSpaceByInteropId(config, kDisplayProcessorIdentityOutputColorSpaceId);
    if (!outputColorSpace.has_value()) {
        auto result = OcioBuiltInResolutionResult(OcioBuiltInRegistryOutcome::Invalid);
        result.recomputedRevision_ = recomputed;
        result.invalidReason_ = OcioBuiltInInvalidReason::OutputColorSpaceNotUniquelyMapped;
        return result;
    }
    const auto displayView = findUniqueDisplayViewForColorSpace(config, *outputColorSpace);
    if (!displayView.has_value()) {
        auto result = OcioBuiltInResolutionResult(OcioBuiltInRegistryOutcome::Invalid);
        result.recomputedRevision_ = recomputed;
        result.invalidReason_ = OcioBuiltInInvalidReason::DisplayViewNotUniquelyMapped;
        return result;
    }

    auto impl = std::make_unique<ResolvedBloomNeutralConfig::Impl>(config);
    ResolvedBloomNeutralConfig resolved(std::move(impl), expectedRevision, *processColorSpace,
                                        *outputColorSpace, displayView->display, displayView->view);

    auto result = OcioBuiltInResolutionResult(OcioBuiltInRegistryOutcome::Ready);
    result.recomputedRevision_ = recomputed;
    result.resolved_ = std::move(resolved);
    return result;
}

} // namespace bloom::color
