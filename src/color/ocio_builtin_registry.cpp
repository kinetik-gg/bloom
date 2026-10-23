#include <bloom/color/ocio_builtin_registry.hpp>

#include "ocio_internal.hpp"
#include <bloom/color/display_processor_identity.hpp>
#include <bloom/color/ocio_content_revision.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ocio_builtin_payload.inc" // NOLINT(bugprone-suspicious-include) -- generated array data;
                                    // opens/closes namespace bloom::color::detail itself

namespace {

constexpr std::size_t kMaximumDisplays = 64;
constexpr std::size_t kMaximumViewsPerDisplay = 32;

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

struct InputColorSpaceMappings final {
    std::vector<bloom::color::OcioColorSpaceInfo> spaces;
    std::string sRgbTexture;
    std::string rec709Video;
    std::string rec2100Pq;
    std::string rec2100Hlg;
};

[[nodiscard]] bool containsInsensitive(const std::string_view value,
                                       const std::string_view needle) {
    if (needle.size() > value.size())
        return false;
    for (std::size_t start = 0; start + needle.size() <= value.size(); ++start) {
        bool equal = true;
        for (std::size_t index = 0; index < needle.size(); ++index) {
            const auto left = static_cast<unsigned char>(value[start + index]);
            const auto right = static_cast<unsigned char>(needle[index]);
            if (std::tolower(left) != std::tolower(right)) {
                equal = false;
                break;
            }
        }
        if (equal)
            return true;
    }
    return true && needle.empty();
}

[[nodiscard]] bool isNonDataColorSpace(const OCIO::ConstConfigRcPtr& config, const char* name) {
    if (name == nullptr)
        return false;
    const auto colorSpace = config->getColorSpace(name);
    return colorSpace != nullptr && !colorSpace->isData();
}

// RGB->XYZ for a set of xy primaries and white point, in double precision (SMPTE RP 177).
[[nodiscard]] std::array<double, 9> rgbToXyz(const std::array<double, 8>& xy) {
    const auto column = [](double x, double y) {
        return std::array<double, 3>{x / y, 1.0, (1.0 - x - y) / y};
    };
    const auto r = column(xy[0], xy[1]), g = column(xy[2], xy[3]), b = column(xy[4], xy[5]),
               w = column(xy[6], xy[7]);
    const std::array<double, 9> p{r[0], g[0], b[0], r[1], g[1], b[1], r[2], g[2], b[2]};
    const auto det = p[0] * (p[4] * p[8] - p[5] * p[7]) - p[1] * (p[3] * p[8] - p[5] * p[6]) +
                     p[2] * (p[3] * p[7] - p[4] * p[6]);
    const std::array<double, 9> inv{
        (p[4] * p[8] - p[5] * p[7]) / det, (p[2] * p[7] - p[1] * p[8]) / det,
        (p[1] * p[5] - p[2] * p[4]) / det, (p[5] * p[6] - p[3] * p[8]) / det,
        (p[0] * p[8] - p[2] * p[6]) / det, (p[2] * p[3] - p[0] * p[5]) / det,
        (p[3] * p[7] - p[4] * p[6]) / det, (p[1] * p[6] - p[0] * p[7]) / det,
        (p[0] * p[4] - p[1] * p[3]) / det};
    std::array<double, 3> scale{};
    for (std::size_t i = 0; i < 3; ++i)
        scale[i] = inv[i * 3] * w[0] + inv[i * 3 + 1] * w[1] + inv[i * 3 + 2] * w[2];
    std::array<double, 9> m{};
    for (std::size_t row = 0; row < 3; ++row)
        for (std::size_t col = 0; col < 3; ++col)
            m[row * 3 + col] = p[row * 3 + col] * scale[col];
    return m;
}

[[nodiscard]] std::array<double, 16> matrix44(const std::array<double, 9>& m) {
    return {m[0], m[1], m[2], 0, m[3], m[4], m[5], 0, m[6], m[7], m[8], 0, 0, 0, 0, 1};
}

// Synthesizes kBloomRec2100HlgColorSpaceId in an in-memory copy of a pinned config that has no
// HLG input space. The pinned locator, payload and content revision are unchanged; only the
// resolved config used for processors gains the space. HLG is scene-referred (BT.2100): the
// space applies OCIO's built-in "CURVE - HLG-OETF-INVERSE" (signal to scene light, 0.42 -> 18%
// grey) and the BT.2020 (D65) to reference-primaries matrix. No HLG OOTF / system gamma is
// applied: that is a display adaptation, and compositing works in scene light.
[[nodiscard]] OCIO::ConstConfigRcPtr withBloomHlgInput(const OCIO::ConstConfigRcPtr& config,
                                                       const bool isNeutral) {
    if (config->getColorSpace(std::string(bloom::color::kBloomRec2100HlgColorSpaceId).c_str()) !=
        nullptr)
        return config;
    constexpr std::array<double, 8> bt2020{0.708, 0.292, 0.170,  0.797,
                                           0.131, 0.046, 0.3127, 0.3290};
    constexpr std::array<double, 8> bt709{0.64, 0.33, 0.30, 0.60, 0.15, 0.06, 0.3127, 0.3290};
    auto toReference = OCIO::GroupTransform::Create();
    // The HLG built-in style is only valid in configs of version 2.4 or later, and the pinned
    // configs are 2.0 and 2.1. OCIO's own implementation of the style is therefore taken from a
    // processor in a minimal raw 2.4 config (it is one 65,536-entry half-domain 1D LUT) and
    // embedded here, so the pinned configs keep their version and validate unchanged. It is the
    // exact op the built-in runs: identical CPU output and GPU processor cache id.
    auto raw = OCIO::Config::CreateRaw()->createEditableCopy();
    raw->setMajorVersion(2);
    raw->setMinorVersion(4);
    auto curve = OCIO::BuiltinTransform::Create();
    curve->setStyle("CURVE - HLG-OETF-INVERSE");
    toReference->appendTransform(raw->getProcessor(curve)->createGroupTransform());
    auto toXyz = OCIO::MatrixTransform::Create();
    const auto bt2020ToXyz = matrix44(rgbToXyz(bt2020));
    toXyz->setMatrix(bt2020ToXyz.data());
    toReference->appendTransform(toXyz);
    if (isNeutral) {
        // Bloom Neutral's reference is linear Rec.709 (D65): XYZ -> Rec.709.
        auto toRec709 = OCIO::MatrixTransform::Create();
        const auto rec709ToXyz = matrix44(rgbToXyz(bt709));
        toRec709->setMatrix(rec709ToXyz.data());
        toRec709->setDirection(OCIO::TRANSFORM_DIR_INVERSE);
        toReference->appendTransform(toRec709);
    } else {
        // The ACES configs' reference is ACES2065-1 (AP0, D60): OCIO's Bradford-adapted built-in.
        auto toAp0 = OCIO::BuiltinTransform::Create();
        toAp0->setStyle("UTILITY - ACES-AP0_to_CIE-XYZ-D65_BFD");
        toAp0->setDirection(OCIO::TRANSFORM_DIR_INVERSE);
        toReference->appendTransform(toAp0);
    }
    auto space = OCIO::ColorSpace::Create(OCIO::REFERENCE_SPACE_SCENE);
    space->setName(std::string(bloom::color::kBloomRec2100HlgColorSpaceId).c_str());
    space->setFamily("Input");
    space->setDescription("Bloom-synthesized Rec.2100 HLG scene-light input (no OOTF)");
    space->setTransform(toReference, OCIO::COLORSPACE_DIR_TO_REFERENCE);
    // The pinned config keeps its version; the copy with the added space is validated.
    auto editable = config->createEditableCopy();
    editable->addColorSpace(space);
    editable->validate();
    return editable;
}

[[nodiscard]] InputColorSpaceMappings inputColorSpaceMappings(const OCIO::ConstConfigRcPtr& config,
                                                              const bool isNeutral) {
    InputColorSpaceMappings result;
    for (int index = 0; index < config->getNumColorSpaces(); ++index) {
        const char* const name = config->getColorSpaceNameByIndex(index);
        if (!isNonDataColorSpace(config, name))
            continue;
        const auto colorSpace = config->getColorSpace(name);
        const auto* const family = colorSpace->getFamily();
        const auto reference = colorSpace->getReferenceSpaceType();
        result.spaces.push_back(
            {std::string(name), family == nullptr ? std::string{} : std::string(family),
             reference == OCIO::REFERENCE_SPACE_SCENE &&
                 config->isColorSpaceLinear(name, OCIO::REFERENCE_SPACE_SCENE)});
    }

    // Rec.2100 video interpretation targets. Configs commonly declare these display-referred
    // spaces inactive (hidden from pickers); OCIO still builds processors from them by name.
    for (int index = 0;
         index < config->getNumColorSpaces(OCIO::SEARCH_REFERENCE_SPACE_ALL, OCIO::COLORSPACE_ALL);
         ++index) {
        const char* const name = config->getColorSpaceNameByIndex(OCIO::SEARCH_REFERENCE_SPACE_ALL,
                                                                  OCIO::COLORSPACE_ALL, index);
        if (!isNonDataColorSpace(config, name) || !containsInsensitive(name, "2100"))
            continue;
        if (result.rec2100Pq.empty() && containsInsensitive(name, "pq"))
            result.rec2100Pq = name;
        if (result.rec2100Hlg.empty() && containsInsensitive(name, "hlg"))
            result.rec2100Hlg = name;
    }
    if (isNeutral) {
        result.sRgbTexture = "srgb_rec709_display";
        result.rec709Video = "lin_rec709_scene";
        return result;
    }

    constexpr std::array<std::string_view, 2> roles{"color_picking", "texture_paint"};
    for (const auto role : roles) {
        const char* const name = config->getRoleColorSpace(std::string(role).c_str());
        if (isNonDataColorSpace(config, name)) {
            result.sRgbTexture = name;
            break;
        }
    }
    if (result.sRgbTexture.empty()) {
        for (const auto& space : result.spaces) {
            if (containsInsensitive(space.id, "srgb") && containsInsensitive(space.id, "texture")) {
                result.sRgbTexture = space.id;
                break;
            }
        }
    }
    for (const auto& space : result.spaces) {
        if (containsInsensitive(space.id, "rec.709") && containsInsensitive(space.id, "camera")) {
            result.rec709Video = space.id;
            break;
        }
    }
    return result;
}

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

[[nodiscard]] std::optional<std::vector<bloom::color::DisplayViewEntry>>
enumerateDisplayViews(const OCIO::ConstConfigRcPtr& config, const DisplayViewPair& defaultPair,
                      bloom::color::OcioBuiltInInvalidReason& invalidReason) {
    const int displayCount = config->getNumDisplays();
    if (displayCount < 0 || static_cast<std::size_t>(displayCount) > kMaximumDisplays) {
        invalidReason = bloom::color::OcioBuiltInInvalidReason::DisplayViewEnumerationLimitExceeded;
        return std::nullopt;
    }

    std::vector<bloom::color::DisplayViewEntry> result;
    for (int displayIndex = 0; displayIndex < displayCount; ++displayIndex) {
        const char* const display = config->getDisplay(displayIndex);
        if (display == nullptr || *display == '\0') {
            invalidReason = bloom::color::OcioBuiltInInvalidReason::DisplayViewNotUniquelyMapped;
            return std::nullopt;
        }
        const int viewCount = config->getNumViews(display);
        if (viewCount < 0 || static_cast<std::size_t>(viewCount) > kMaximumViewsPerDisplay) {
            invalidReason =
                bloom::color::OcioBuiltInInvalidReason::DisplayViewEnumerationLimitExceeded;
            return std::nullopt;
        }
        for (int viewIndex = 0; viewIndex < viewCount; ++viewIndex) {
            const char* const view = config->getView(display, viewIndex);
            const char* const colourSpace =
                view == nullptr ? nullptr : config->getDisplayViewColorSpaceName(display, view);
            if (view == nullptr || *view == '\0' || colourSpace == nullptr ||
                *colourSpace == '\0') {
                invalidReason =
                    bloom::color::OcioBuiltInInvalidReason::DisplayViewNotUniquelyMapped;
                return std::nullopt;
            }
            result.push_back({std::string(display), std::string(view), std::string(colourSpace),
                              std::string_view(display) == defaultPair.display &&
                                  std::string_view(view) == defaultPair.view});
        }
    }
    const auto defaultFound = std::find_if(result.begin(), result.end(),
                                           [](const auto& entry) { return entry.isDefault; });
    if (defaultFound == result.end()) {
        invalidReason = bloom::color::OcioBuiltInInvalidReason::DisplayViewNotUniquelyMapped;
        return std::nullopt;
    }
    return result;
}

} // namespace

namespace bloom::color {

ResolvedBloomNeutralConfig::ResolvedBloomNeutralConfig(
    std::unique_ptr<Impl> impl, core::Sha256Digest expectedRevision,
    std::string processColorSpaceId, std::string outputColorSpaceId, std::string displayName,
    std::string viewName, std::string configName, std::vector<OcioColorSpaceInfo> colorSpaces,
    std::vector<DisplayViewEntry> displays, std::string sRgbTextureColorSpaceId,
    std::string rec709VideoColorSpaceId, std::string rec2100PqColorSpaceId,
    std::string rec2100HlgColorSpaceId) noexcept
    : impl_(std::move(impl)), expectedRevision_(expectedRevision),
      processColorSpaceId_(std::move(processColorSpaceId)),
      outputColorSpaceId_(std::move(outputColorSpaceId)), displayName_(std::move(displayName)),
      viewName_(std::move(viewName)), configName_(std::move(configName)),
      colorSpaces_(std::move(colorSpaces)), displays_(std::move(displays)),
      sRgbTextureColorSpaceId_(std::move(sRgbTextureColorSpaceId)),
      rec709VideoColorSpaceId_(std::move(rec709VideoColorSpaceId)),
      rec2100PqColorSpaceId_(std::move(rec2100PqColorSpaceId)),
      rec2100HlgColorSpaceId_(std::move(rec2100HlgColorSpaceId)) {}

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
std::string_view ResolvedBloomNeutralConfig::configName() const& noexcept { return configName_; }
const std::vector<OcioColorSpaceInfo>& ResolvedBloomNeutralConfig::colorSpaces() const& noexcept {
    return colorSpaces_;
}
std::span<const DisplayViewEntry> ResolvedBloomNeutralConfig::displays() const& noexcept {
    return displays_;
}
std::string_view ResolvedBloomNeutralConfig::sRgbTextureColorSpaceId() const& noexcept {
    return sRgbTextureColorSpaceId_;
}
std::string_view ResolvedBloomNeutralConfig::rec709VideoColorSpaceId() const& noexcept {
    return rec709VideoColorSpaceId_;
}
std::string synthesizedColorSpaceIdentity(const std::string_view colorSpaceId) {
    if (colorSpaceId != kBloomRec2100HlgColorSpaceId)
        return {};
    return std::string("bloom-hlg-scene-v1;ocio=") + OCIO::GetVersion();
}
std::string_view ResolvedBloomNeutralConfig::rec2100PqColorSpaceId() const& noexcept {
    return rec2100PqColorSpaceId_;
}
std::string_view ResolvedBloomNeutralConfig::rec2100HlgColorSpaceId() const& noexcept {
    return rec2100HlgColorSpaceId_;
}
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
resolveOcioBuiltIn(const OcioConfigLocatorKind locatorKind, const std::string_view locatorValue,
                   const core::Sha256Digest& expectedRevision,
                   const std::string_view requestedWorkingColorSpaceId) noexcept {
    if (locatorKind != OcioConfigLocatorKind::BloomBuiltIn) {
        // A real, planned locator kind this in-process registry never resolves -- see
        // OcioBuiltInRegistryOutcome::LocatorKindRequiresHelper.
        return OcioBuiltInResolutionResult(OcioBuiltInRegistryOutcome::LocatorKindRequiresHelper);
    }
    const bool isNeutral = locatorValue == kBloomNeutralV1ConfigUri;
    const bool isAces = locatorValue == kAcesCgV1ConfigUri;
    if (!isNeutral && !isAces) {
        return OcioBuiltInResolutionResult(OcioBuiltInRegistryOutcome::Missing);
    }

    std::string serializedAcesConfig;
    std::span<const std::byte> payload;
    OCIO::ConstConfigRcPtr config;
    try {
        if (isNeutral) {
            payload = embeddedPayloadBytes();
            std::istringstream stream(
                std::string(reinterpret_cast<const char*>(payload.data()), payload.size()));
            config = OCIO::Config::CreateFromStream(stream);
        } else {
            config = OCIO::Config::CreateFromBuiltinConfig(
                std::string(kAcesCgV1BuiltinConfigName).c_str());
            std::ostringstream stream;
            config->serialize(stream);
            serializedAcesConfig = stream.str();
            payload =
                std::as_bytes(std::span(serializedAcesConfig.data(), serializedAcesConfig.size()));
        }
        config->validate();
    } catch (const OCIO::Exception&) {
        auto result = OcioBuiltInResolutionResult(OcioBuiltInRegistryOutcome::Invalid);
        result.invalidReason_ = OcioBuiltInInvalidReason::ParseFailed;
        return result;
    } catch (const std::exception&) {
        auto result = OcioBuiltInResolutionResult(OcioBuiltInRegistryOutcome::Invalid);
        result.invalidReason_ = OcioBuiltInInvalidReason::ValidateFailed;
        return result;
    }

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

    std::optional<std::string> processColorSpace;
    if (isNeutral) {
        processColorSpace =
            findUniqueColorSpaceByInteropId(config, kDisplayProcessorIdentitySourceColorSpaceId);
    } else {
        const char* const sceneLinearRole = config->getRoleColorSpace("scene_linear");
        const std::string_view selected =
            requestedWorkingColorSpaceId.empty()
                ? (sceneLinearRole == nullptr ? std::string_view{}
                                              : std::string_view(sceneLinearRole))
                : requestedWorkingColorSpaceId;
        const OCIO::ConstColorSpaceRcPtr colorSpace =
            selected.empty() ? OCIO::ConstColorSpaceRcPtr{}
                             : config->getColorSpace(std::string(selected).c_str());
        if (!colorSpace) {
            auto result = OcioBuiltInResolutionResult(OcioBuiltInRegistryOutcome::Invalid);
            result.recomputedRevision_ = recomputed;
            result.invalidReason_ = OcioBuiltInInvalidReason::WorkingColorSpaceMissing;
            return result;
        }
        if (colorSpace->isData() ||
            colorSpace->getReferenceSpaceType() != OCIO::REFERENCE_SPACE_SCENE ||
            !config->isColorSpaceLinear(std::string(selected).c_str(),
                                        OCIO::REFERENCE_SPACE_SCENE)) {
            auto result = OcioBuiltInResolutionResult(OcioBuiltInRegistryOutcome::Invalid);
            result.recomputedRevision_ = recomputed;
            result.invalidReason_ = OcioBuiltInInvalidReason::WorkingColorSpaceNotSceneLinear;
            return result;
        }
        processColorSpace = std::string(selected);
    }
    if (!processColorSpace.has_value()) {
        auto result = OcioBuiltInResolutionResult(OcioBuiltInRegistryOutcome::Invalid);
        result.recomputedRevision_ = recomputed;
        result.invalidReason_ = OcioBuiltInInvalidReason::ProcessColorSpaceNotUniquelyMapped;
        return result;
    }
    if (isNeutral && !requestedWorkingColorSpaceId.empty() &&
        requestedWorkingColorSpaceId != *processColorSpace) {
        const auto selected = std::string(requestedWorkingColorSpaceId);
        const OCIO::ConstColorSpaceRcPtr colorSpace = config->getColorSpace(selected.c_str());
        if (!colorSpace) {
            auto result = OcioBuiltInResolutionResult(OcioBuiltInRegistryOutcome::Invalid);
            result.recomputedRevision_ = recomputed;
            result.invalidReason_ = OcioBuiltInInvalidReason::WorkingColorSpaceMissing;
            return result;
        }
        if (colorSpace->isData() ||
            colorSpace->getReferenceSpaceType() != OCIO::REFERENCE_SPACE_SCENE ||
            !config->isColorSpaceLinear(selected.c_str(), OCIO::REFERENCE_SPACE_SCENE)) {
            auto result = OcioBuiltInResolutionResult(OcioBuiltInRegistryOutcome::Invalid);
            result.recomputedRevision_ = recomputed;
            result.invalidReason_ = OcioBuiltInInvalidReason::WorkingColorSpaceNotSceneLinear;
            return result;
        }
        *processColorSpace = selected;
    }
    std::optional<std::string> outputColorSpace;
    if (isNeutral) {
        outputColorSpace =
            findUniqueColorSpaceByInteropId(config, kDisplayProcessorIdentityOutputColorSpaceId);
        if (!outputColorSpace.has_value()) {
            auto result = OcioBuiltInResolutionResult(OcioBuiltInRegistryOutcome::Invalid);
            result.recomputedRevision_ = recomputed;
            result.invalidReason_ = OcioBuiltInInvalidReason::OutputColorSpaceNotUniquelyMapped;
            return result;
        }
    } else {
        outputColorSpace = std::string("display");
    }
    std::optional<DisplayViewPair> displayView;
    if (isNeutral) {
        displayView = findUniqueDisplayViewForColorSpace(config, *outputColorSpace);
    } else {
        const char* const display = config->getDefaultDisplay();
        const char* const view = display == nullptr
                                     ? nullptr
                                     : config->getDefaultView(display, processColorSpace->c_str());
        if (display != nullptr && view != nullptr && *display != '\0' && *view != '\0') {
            displayView = DisplayViewPair{std::string(display), std::string(view)};
        }
    }
    if (!displayView.has_value()) {
        auto result = OcioBuiltInResolutionResult(OcioBuiltInRegistryOutcome::Invalid);
        result.recomputedRevision_ = recomputed;
        result.invalidReason_ = OcioBuiltInInvalidReason::DisplayViewNotUniquelyMapped;
        return result;
    }

    OcioBuiltInInvalidReason displayEnumerationReason = OcioBuiltInInvalidReason::None;
    const auto displays = enumerateDisplayViews(config, *displayView, displayEnumerationReason);
    if (!displays.has_value()) {
        auto result = OcioBuiltInResolutionResult(OcioBuiltInRegistryOutcome::Invalid);
        result.recomputedRevision_ = recomputed;
        result.invalidReason_ = displayEnumerationReason;
        return result;
    }

    try {
        config = withBloomHlgInput(config, isNeutral);
    } catch (const OCIO::Exception&) {
        auto result = OcioBuiltInResolutionResult(OcioBuiltInRegistryOutcome::Invalid);
        result.recomputedRevision_ = recomputed;
        result.invalidReason_ = OcioBuiltInInvalidReason::ParseFailed;
        return result;
    }
    const auto inputMappings = inputColorSpaceMappings(config, isNeutral);
    auto impl = std::make_unique<ResolvedBloomNeutralConfig::Impl>(config);
    ResolvedBloomNeutralConfig resolved(
        std::move(impl), expectedRevision, *processColorSpace,
        isNeutral
            ? *outputColorSpace
            : std::string(config->getDisplayViewColorSpaceName(displayView->display.c_str(),
                                                               displayView->view.c_str()) != nullptr
                              ? config->getDisplayViewColorSpaceName(displayView->display.c_str(),
                                                                     displayView->view.c_str())
                              : displayView->display),
        displayView->display, displayView->view,
        config->getName() == nullptr ? std::string(locatorValue) : std::string(config->getName()),
        inputMappings.spaces, *displays, inputMappings.sRgbTexture, inputMappings.rec709Video,
        inputMappings.rec2100Pq, inputMappings.rec2100Hlg);

    auto result = OcioBuiltInResolutionResult(OcioBuiltInRegistryOutcome::Ready);
    result.recomputedRevision_ = recomputed;
    result.resolved_ = std::move(resolved);
    return result;
}

OcioBuiltInResolutionResult
resolveBloomNeutralV1BuiltIn(const OcioConfigLocatorKind locatorKind,
                             const std::string_view locatorValue,
                             const core::Sha256Digest& expectedRevision) noexcept {
    return resolveOcioBuiltIn(locatorKind, locatorValue, expectedRevision, {});
}

std::optional<core::Sha256Digest>
ocioBuiltInContentRevision(const OcioConfigLocatorKind locatorKind,
                           const std::string_view locatorValue) noexcept {
    if (locatorKind != OcioConfigLocatorKind::BloomBuiltIn) {
        return std::nullopt;
    }
    if (locatorValue == kBloomNeutralV1ConfigUri) {
        const auto result =
            computeOcioContentRevisionV1(OcioContentLocatorKind::BuiltIn, embeddedPayloadBytes());
        return result ? std::optional<core::Sha256Digest>(*result.revision()) : std::nullopt;
    }
    if (locatorValue != kAcesCgV1ConfigUri) {
        return std::nullopt;
    }
    try {
        const auto config =
            OCIO::Config::CreateFromBuiltinConfig(std::string(kAcesCgV1BuiltinConfigName).c_str());
        std::ostringstream stream;
        config->serialize(stream);
        const auto serialized = stream.str();
        const auto payload = std::as_bytes(std::span(serialized.data(), serialized.size()));
        const auto result = computeOcioContentRevisionV1(OcioContentLocatorKind::BuiltIn, payload);
        return result ? std::optional<core::Sha256Digest>(*result.revision()) : std::nullopt;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

} // namespace bloom::color
