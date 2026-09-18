#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/color/ocio_cpu_color_space_processor.hpp>

#include <array>
#include <cmath>
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

using bloom::color::OcioBuiltInRegistryOutcome;
using bloom::color::OcioColorSpaceProcessorError;
using bloom::color::OcioConfigLocatorKind;

[[nodiscard]] bloom::color::OcioBuiltInResolutionResult acesConfig() {
    const auto revision = bloom::color::ocioBuiltInContentRevision(
        OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kAcesCgV1ConfigUri);
    if (!revision.has_value()) {
        return bloom::color::resolveOcioBuiltIn(
            OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kAcesCgV1ConfigUri,
            bloom::color::kBloomNeutralV1ConfigDigest, "ACEScg");
    }
    return bloom::color::resolveOcioBuiltIn(OcioConfigLocatorKind::BloomBuiltIn,
                                            bloom::color::kAcesCgV1ConfigUri, *revision, "ACEScg");
}

void testGeneralTransformAndAlpha(Expectations& expectations) {
    auto config = acesConfig();
    expectations.expect(config.outcome() == OcioBuiltInRegistryOutcome::Ready,
                        "the ACES config resolves for a general input transform");
    const auto* resolved = config.resolved();
    if (resolved == nullptr) {
        return;
    }
    const auto prepared =
        bloom::color::CpuColorSpaceProcessor::prepare(*resolved, "ACES2065-1", "ACEScg");
    expectations.expect(prepared.succeeded(), "ACES2065-1 to ACEScg prepares on the CPU");
    if (!prepared) {
        return;
    }
    std::array<std::array<float, 4>, 1> pixels{{{0.18F, 0.12F, 0.09F, 0.375F}}};
    const auto before = pixels[0];
    expectations.expect(prepared.processor()->apply(pixels),
                        "the prepared CPU transform applies to finite RGBA32F pixels");
    expectations.expect(pixels[0][3] == before[3], "the general input transform preserves alpha");
    expectations.expect(pixels[0][0] != before[0] || pixels[0][1] != before[1] ||
                            pixels[0][2] != before[2],
                        "a non-identity ACES2065-1 to ACEScg transform changes RGB");
    expectations.expect(prepared.processor()->fromId() == "ACES2065-1" &&
                            prepared.processor()->workingSpaceId() == "ACEScg" &&
                            prepared.processor()->configRevision() == resolved->expectedRevision(),
                        "the processor retains its exact source, working space, and revision");
}

void testIdentityAndFailures(Expectations& expectations) {
    auto config = acesConfig();
    const auto* resolved = config.resolved();
    if (resolved == nullptr) {
        expectations.expect(false, "the ACES config is available for identity/error coverage");
        return;
    }
    const auto identity =
        bloom::color::CpuColorSpaceProcessor::prepare(*resolved, "ACEScg", "ACEScg");
    expectations.expect(identity.succeeded(),
                        "a source equal to the working space prepares as identity");
    if (identity) {
        std::array<std::array<float, 4>, 1> pixels{{{0.2F, 0.3F, 0.4F, 0.5F}}};
        const auto before = pixels[0];
        expectations.expect(identity.processor()->apply(pixels) && pixels[0] == before,
                            "the identity processor is bit-preserving including alpha");
    }

    const auto missing =
        bloom::color::CpuColorSpaceProcessor::prepare(*resolved, "missing-colour-space", "ACEScg");
    expectations.expect(missing.error() == OcioColorSpaceProcessorError::MissingInputColorSpace,
                        "a missing input id fails closed with a typed error");
    const auto data =
        bloom::color::CpuColorSpaceProcessor::prepare(*resolved, "Utility - Raw", "ACEScg");
    expectations.expect(data.error() == OcioColorSpaceProcessorError::InputColorSpaceIsData,
                        "a data input space fails closed with a typed error");
    const auto missingWorking =
        bloom::color::CpuColorSpaceProcessor::prepare(*resolved, "ACEScg", "missing-working-space");
    expectations.expect(missingWorking.error() ==
                            OcioColorSpaceProcessorError::MissingWorkingColorSpace,
                        "a missing working id fails closed with a typed error");
}

} // namespace

int main() {
    Expectations expectations;
    testGeneralTransformAndAlpha(expectations);
    testIdentityAndFailures(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
