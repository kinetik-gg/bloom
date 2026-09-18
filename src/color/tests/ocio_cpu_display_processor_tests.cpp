#include <bloom/color/ocio_cpu_display_processor.hpp>

#include <bloom/color/display_processor_identity.hpp>
#include <bloom/color/ocio_builtin_registry.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

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

void appendBytes(std::vector<std::byte>& out, const std::string_view text) {
    for (const char character : text) {
        out.push_back(static_cast<std::byte>(character));
    }
}
void appendU8(std::vector<std::byte>& out, const std::uint8_t value) {
    out.push_back(static_cast<std::byte>(value));
}
void appendU16(std::vector<std::byte>& out, const std::uint16_t value) {
    out.push_back(static_cast<std::byte>((value >> 8U) & 0xFFU));
    out.push_back(static_cast<std::byte>(value & 0xFFU));
}
void appendU32(std::vector<std::byte>& out, const std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::byte>((value >> static_cast<unsigned>(shift)) & 0xFFU));
    }
}
void appendText(std::vector<std::byte>& out, const std::string_view text) {
    appendU32(out, static_cast<std::uint32_t>(text.size()));
    appendBytes(out, text);
}

// Independently derives the expected canonical DisplayProcessorIdentity bytes from
// docs/architecture/color-management.md's "Qualified Display Intent And Identity" layout, without
// calling bloom::color::writeDisplayProcessorIdentityV1 -- this is the golden the production
// serializer is checked against, not a re-exercise of it.
[[nodiscard]] std::vector<std::byte>
buildExpectedNeutralIdentityBytes(const bloom::core::Sha256Digest& expectedOcioRevision) {
    std::vector<std::byte> bytes;
    appendBytes(bytes, "BloomDisplayProcessorIdentity");
    bytes.push_back(std::byte{0x00});
    appendU16(bytes, 1); // version
    for (const auto byte : expectedOcioRevision.bytes()) {
        bytes.push_back(static_cast<std::byte>(byte));
    }
    appendU16(bytes, 0);                      // contextVariableCount
    appendText(bytes, "lin_rec709_scene");    // source Color Interop ID
    appendText(bytes, "srgb_rec709_display"); // OCIO display name (this asset's discovered name)
    appendText(bytes, "srgb_rec709_display"); // OCIO view name (this asset's discovered name)
    appendU8(bytes, 0);                       // lookMode: bypass
    appendU16(bytes, 0);                      // lookCount
    appendText(bytes, "srgb_rec709_display"); // output Color Interop ID
    appendText(bytes, "reference");           // display quality ID
    appendText(bytes, "bloom.color.ocio-cpu-display.v1"); // display pixel-semantics profile ID
    appendText(bytes, "straight-rgba8");                  // packing ID
    return bytes;
}

void testNeutralIdentityGolden(Expectations& expectations) {
    auto resolution = bloom::color::resolveBloomNeutralV1BuiltIn(
        bloom::color::OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kBloomNeutralV1ConfigUri,
        bloom::color::kBloomNeutralV1ConfigDigest);
    expectations.expect(resolution.outcome() == bloom::color::OcioBuiltInRegistryOutcome::Ready,
                        "the Bloom Neutral built-in resolves Ready for the processor test");
    auto resolved = std::move(resolution).takeResolved();
    expectations.expect(resolved.has_value(), "a Ready resolution yields a resolved config");
    if (!resolved.has_value()) {
        return;
    }

    auto buildResult = bloom::color::buildBloomNeutralCpuDisplayProcessor(*resolved);
    expectations.expect(static_cast<bool>(buildResult),
                        "the CPU display processor builds successfully");
    if (!buildResult) {
        return;
    }
    const auto* handle = buildResult.handle();
    expectations.expect(handle != nullptr, "a successful build exposes a handle");
    if (handle == nullptr) {
        return;
    }

    const std::array grid{0.0, 0.003, 0.04, 0.18, 0.5, 0.9, 1.0, 2.0, 16.0};
    for (const double r : grid) {
        for (const double g : grid) {
            for (const double b : grid) {
                const bloom::core::Color4d display{r, g, b, 0.123456789012345};
                const auto reference = handle->displayToReference(display);
                expectations.expect(reference.has_value(), "grid inverse succeeds including HDR");
                if (!reference)
                    continue;
                const auto roundTrip = handle->referenceToDisplay(*reference);
                expectations.expect(roundTrip &&
                                        std::abs(roundTrip->red - std::min(r, 1.0)) < 3e-5 &&
                                        std::abs(roundTrip->green - std::min(g, 1.0)) < 3e-5 &&
                                        std::abs(roundTrip->blue - std::min(b, 1.0)) < 3e-5,
                                    "OCIO pair round trips the grid up to display clipping");
                expectations.expect(reference->alpha == display.alpha && roundTrip &&
                                        roundTrip->alpha == display.alpha,
                                    "alpha is preserved at binary64 precision");
                if (r > 1.0)
                    expectations.expect(reference->red > 1.0, "inverse retains HDR range");
                const bloom::core::Color4d authored{r, g, b, 0.0};
                const auto shown = handle->referenceToDisplay(authored);
                expectations.expect(shown && shown->alpha == 0.0,
                                    "straight RGB conversion also preserves zero alpha");
                if (shown && r <= 1.0 && g <= 1.0 && b <= 1.0) {
                    const auto restored = handle->displayToReference(*shown);
                    expectations.expect(restored && std::abs(restored->red - r) < 3e-5 &&
                                            std::abs(restored->green - g) < 3e-5 &&
                                            std::abs(restored->blue - b) < 3e-5,
                                        "reference grid is invertible before clipping");
                }
            }
        }
    }
    expectations.expect(
        !handle->referenceToDisplay({std::numeric_limits<double>::infinity(), 0, 0, 1}) &&
            !handle->displayToReference({std::numeric_limits<double>::max(), 0, 0, 1}),
        "non-finite and float overflow fail closed");

    const auto expected =
        buildExpectedNeutralIdentityBytes(bloom::color::kBloomNeutralV1ConfigDigest);
    const auto actual = handle->identity().canonicalBytes();
    expectations.expect(std::vector<std::byte>(actual.begin(), actual.end()) == expected,
                        "the built processor's canonical DisplayProcessorIdentity bytes equal the "
                        "independently derived golden");

    auto explicitDefault = bloom::color::buildBloomNeutralCpuDisplayProcessor(
        *resolved, resolved->displayName(), resolved->viewName());
    expectations.expect(explicitDefault.succeeded(),
                        "selecting the resolved sRGB pair explicitly still builds");
    if (explicitDefault.succeeded()) {
        expectations.expect(
            std::ranges::equal(explicitDefault.handle()->identity().canonicalBytes(), actual),
            "the explicit default sRGB pair preserves the identity golden");
        const auto authored = bloom::core::Color4d{0.18, 0.5, 0.8, 1.0};
        expectations.expect(explicitDefault.handle()->referenceToDisplay(authored) ==
                                handle->referenceToDisplay(authored),
                            "the explicit default sRGB pair preserves display values");
    }

    expectations.expect(handle->lease().kind() ==
                            bloom::color::DisplayProcessorLeaseKind::InProcess,
                        "the Bloom Neutral built-in lease is InProcess");
    expectations.expect(!handle->provenance().ocioVersion.empty(),
                        "execution provenance records a nonempty OCIO version");
    expectations.expect(!handle->provenance().processorCacheId.empty(),
                        "execution provenance records a nonempty OCIO processor cache ID");
}

void testAcesCgWhiteDisplay(Expectations& expectations) {
    const auto revision = bloom::color::ocioBuiltInContentRevision(
        bloom::color::OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kAcesCgV1ConfigUri);
    expectations.expect(revision.has_value(), "the ACES CG built-in has a content revision");
    if (!revision.has_value())
        return;
    auto resolution = bloom::color::resolveOcioBuiltIn(
        bloom::color::OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kAcesCgV1ConfigUri,
        *revision, bloom::color::kAcesCgV1SceneLinearColorSpaceId);
    expectations.expect(resolution.ready(), "the ACES CG built-in resolves its ACEScg space");
    auto resolved = std::move(resolution).takeResolved();
    if (!resolved.has_value())
        return;
    auto built = bloom::color::buildBloomNeutralCpuDisplayProcessor(*resolved);
    expectations.expect(built.succeeded(), "the ACEScg CPU display processor builds");
    if (!built.succeeded())
        return;
    const auto displayed = built.handle()->referenceToDisplay({1.0, 1.0, 1.0, 1.0});
    expectations.expect(displayed.has_value(), "an ACEScg authored white reaches the display");
    if (displayed.has_value()) {
        constexpr double kAcesSdrWhite = 0.811975;
        expectations.expect(std::abs(displayed->red - kAcesSdrWhite) < 1e-5 &&
                                std::abs(displayed->green - kAcesSdrWhite) < 1e-5 &&
                                std::abs(displayed->blue - kAcesSdrWhite) < 1e-5,
                            "ACEScg/AP1 white follows the ACES SDR display transform");
    }

    const auto rec709 = std::ranges::find_if(resolved->displays(), [](const auto& entry) {
        return entry.display == "Rec.1886 Rec.709 - Display" &&
               entry.view == "ACES 1.0 - SDR Video";
    });
    expectations.expect(rec709 != resolved->displays().end(),
                        "the ACES config exposes the Rec.709 SDR display/view pair");
    if (rec709 == resolved->displays().end()) {
        return;
    }
    auto selected = bloom::color::buildBloomNeutralCpuDisplayProcessor(*resolved, rec709->display,
                                                                       rec709->view);
    expectations.expect(selected.succeeded(), "an explicit non-default ACES display/view builds");
    if (!selected.succeeded()) {
        return;
    }
    const auto selectedValue = selected.handle()->referenceToDisplay({0.18, 0.18, 0.18, 1.0});
    const auto defaultValue = built.handle()->referenceToDisplay({0.18, 0.18, 0.18, 1.0});
    expectations.expect(selectedValue.has_value() && defaultValue.has_value(),
                        "both ACES display/view transforms evaluate the known solid");
    if (selectedValue.has_value() && defaultValue.has_value()) {
        const auto pack = [](const double value) {
            return static_cast<std::uint8_t>(std::floor(std::clamp(value, 0.0, 1.0) * 255.0 + 0.5));
        };
        expectations.expect(pack(selectedValue->red) != pack(defaultValue->red) ||
                                pack(selectedValue->green) != pack(defaultValue->green) ||
                                pack(selectedValue->blue) != pack(defaultValue->blue),
                            "a non-default ACES display/view changes the packed solid bytes");
        constexpr bloom::core::Color4d kRec709Oracle{0.389528, 0.389529, 0.389528, 1.0};
        expectations.expect(
            std::abs(selectedValue->red - kRec709Oracle.red) < 1.0 / 255.0 &&
                std::abs(selectedValue->green - kRec709Oracle.green) < 1.0 / 255.0 &&
                std::abs(selectedValue->blue - kRec709Oracle.blue) < 1.0 / 255.0,
            "the selected packed solid agrees with the OCIO oracle before encoding");
    }
    expectations.expect(selected.handle()->provenance().displayName == rec709->display &&
                            selected.handle()->provenance().viewName == rec709->view,
                        "processor provenance retains the explicit display/view pair");
}

} // namespace

int main() {
    Expectations expectations;
    testNeutralIdentityGolden(expectations);
    testAcesCgWhiteDisplay(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
