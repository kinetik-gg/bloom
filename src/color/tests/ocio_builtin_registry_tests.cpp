#include <bloom/color/ocio_builtin_registry.hpp>

#include <bloom/color/display_processor_identity.hpp>
#include <bloom/color/ocio_content_revision.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string_view>

namespace {

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << "FAILED: " << message << '\n';
    }

    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

using bloom::color::OcioBuiltInInvalidReason;
using bloom::color::OcioBuiltInRegistryOutcome;
using bloom::color::OcioConfigLocatorKind;
using bloom::color::resolveBloomNeutralV1BuiltIn;

void testAcesCgBuiltIn(Expectations& expectations) {
    const auto revision = bloom::color::ocioBuiltInContentRevision(
        OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kAcesCgV1ConfigUri);
    expectations.expect(revision.has_value(), "the OCIO ACES CG built-in has a content revision");
    if (!revision.has_value()) {
        return;
    }
    auto result = bloom::color::resolveOcioBuiltIn(OcioConfigLocatorKind::BloomBuiltIn,
                                                   bloom::color::kAcesCgV1ConfigUri, *revision,
                                                   bloom::color::kAcesCgV1SceneLinearColorSpaceId);
    expectations.expect(result.outcome() == OcioBuiltInRegistryOutcome::Ready,
                        "the pinned ACES CG built-in resolves Ready");
    const auto* resolved = result.resolved();
    expectations.expect(resolved != nullptr && resolved->processColorSpaceId() == "ACEScg" &&
                            resolved->configName() == bloom::color::kAcesCgV1BuiltinConfigName,
                        "ACES CG resolves the requested ACEScg working space and stable name");

    auto aces2065 =
        bloom::color::resolveOcioBuiltIn(OcioConfigLocatorKind::BloomBuiltIn,
                                         bloom::color::kAcesCgV1ConfigUri, *revision, "ACES2065-1");
    expectations.expect(aces2065.outcome() == OcioBuiltInRegistryOutcome::Ready,
                        "ACES2065-1 is accepted because the ACES CG config exposes it");
    auto neutralAces2065 = bloom::color::resolveOcioBuiltIn(
        OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kBloomNeutralV1ConfigUri,
        bloom::color::kBloomNeutralV1ConfigDigest, "ACES2065-1");
    expectations.expect(neutralAces2065.outcome() == OcioBuiltInRegistryOutcome::Invalid &&
                            neutralAces2065.invalidReason() ==
                                bloom::color::OcioBuiltInInvalidReason::WorkingColorSpaceMissing,
                        "a composition override fails closed when the selected config lacks it");
}

void testReadyExactDigest(Expectations& expectations) {
    auto result = resolveBloomNeutralV1BuiltIn(OcioConfigLocatorKind::BloomBuiltIn,
                                               bloom::color::kBloomNeutralV1ConfigUri,
                                               bloom::color::kBloomNeutralV1ConfigDigest);
    expectations.expect(result.outcome() == OcioBuiltInRegistryOutcome::Ready,
                        "exact digest against the exact URI resolves Ready");
    expectations.expect(static_cast<bool>(result), "Ready result is contextually true");
    const auto* resolved = result.resolved();
    expectations.expect(resolved != nullptr, "Ready result exposes a resolved config");
    if (resolved == nullptr) {
        return;
    }
    expectations.expect(resolved->processColorSpaceId() == "lin_rec709_scene",
                        "the discovered process color-space name is lin_rec709_scene");
    expectations.expect(resolved->outputColorSpaceId() == "srgb_rec709_display",
                        "the discovered output color-space name is srgb_rec709_display");
    expectations.expect(resolved->displayName() == "srgb_rec709_display",
                        "the discovered display name is srgb_rec709_display");
    expectations.expect(resolved->viewName() == "srgb_rec709_display",
                        "the discovered view name is srgb_rec709_display");
    expectations.expect(resolved->expectedRevision() == bloom::color::kBloomNeutralV1ConfigDigest,
                        "the resolved config retains the caller's expected revision");
}

void testChangedPerturbedRevision(Expectations& expectations) {
    auto perturbed = bloom::color::kBloomNeutralV1ConfigDigest;
    auto bytes = perturbed.bytes();
    // Sha256Digest exposes only a const span; rebuild a flipped digest through fromBytes.
    std::array<std::uint8_t, 32> flipped{};
    for (std::size_t i = 0; i < flipped.size(); ++i) {
        flipped[i] = bytes[i];
    }
    flipped[0] ^= 0xFFU;
    const auto flippedDigest = bloom::core::Sha256Digest::fromBytes(flipped);

    auto result = resolveBloomNeutralV1BuiltIn(
        OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kBloomNeutralV1ConfigUri, flippedDigest);
    expectations.expect(result.outcome() == OcioBuiltInRegistryOutcome::Changed,
                        "a perturbed expected revision resolves Changed");
    expectations.expect(result.resolved() == nullptr, "Changed result has no resolved config");
    expectations.expect(result.recomputedRevision().has_value() &&
                            *result.recomputedRevision() ==
                                bloom::color::kBloomNeutralV1ConfigDigest,
                        "Changed still reports the actual recomputed revision");
}

void testMissingUnknownUri(Expectations& expectations) {
    auto result = resolveBloomNeutralV1BuiltIn(OcioConfigLocatorKind::BloomBuiltIn,
                                               "bloom://ocio/neutral-v2/config.ocio",
                                               bloom::color::kBloomNeutralV1ConfigDigest);
    expectations.expect(result.outcome() == OcioBuiltInRegistryOutcome::Missing,
                        "an unrecognized built-in URI resolves Missing");
    expectations.expect(result.resolved() == nullptr, "Missing result has no resolved config");

    // Aliasing is rejected, never normalized: a trailing-slash or differently-cased variant is
    // Missing too, not silently accepted.
    auto trailingSlash = resolveBloomNeutralV1BuiltIn(OcioConfigLocatorKind::BloomBuiltIn,
                                                      "bloom://ocio/neutral-v1/config.ocio/",
                                                      bloom::color::kBloomNeutralV1ConfigDigest);
    expectations.expect(trailingSlash.outcome() == OcioBuiltInRegistryOutcome::Missing,
                        "a trailing-slash alias of the built-in URI resolves Missing");
}

void testUnsupportedLocatorKindsTyped(Expectations& expectations) {
    for (const auto kind :
         {OcioConfigLocatorKind::ProjectRelativeArchive, OcioConfigLocatorKind::ExternalArchive,
          OcioConfigLocatorKind::ExternalLooseConfig}) {
        auto result = resolveBloomNeutralV1BuiltIn(kind, bloom::color::kBloomNeutralV1ConfigUri,
                                                   bloom::color::kBloomNeutralV1ConfigDigest);
        expectations.expect(
            result.outcome() == OcioBuiltInRegistryOutcome::LocatorKindRequiresHelper,
            "a non-built-in locator kind resolves the honestly-typed helper-required outcome, "
            "never Missing or Invalid");
        expectations.expect(result.resolved() == nullptr,
                            "a helper-required outcome has no resolved config");
    }
}

void testEmbeddedPayloadDigestEqualsProfileConstant(Expectations& expectations) {
    const auto payload = bloom::color::bloomNeutralV1EmbeddedPayload();
    expectations.expect(payload.size() == bloom::color::kBloomNeutralV1ConfigPayloadByteCount,
                        "the embedded payload's byte count equals the frozen profile constant");

    const auto revision = bloom::color::computeOcioContentRevisionV1(
        bloom::color::OcioContentLocatorKind::BuiltIn, payload);
    expectations.expect(static_cast<bool>(revision),
                        "the embedded payload's kind-1 revision computes successfully");
    if (revision) {
        expectations.expect(*revision.revision() == bloom::color::kBloomNeutralV1ConfigDigest,
                            "the embedded payload's recomputed digest equals "
                            "bloom::color::kBloomNeutralV1ConfigDigest");
    }
}

} // namespace

int main() {
    Expectations expectations;
    testReadyExactDigest(expectations);
    testChangedPerturbedRevision(expectations);
    testMissingUnknownUri(expectations);
    testUnsupportedLocatorKindsTyped(expectations);
    testEmbeddedPayloadDigestEqualsProfileConstant(expectations);
    testAcesCgBuiltIn(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
