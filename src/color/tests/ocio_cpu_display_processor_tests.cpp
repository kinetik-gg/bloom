#include <bloom/color/ocio_cpu_display_processor.hpp>

#include <bloom/color/display_processor_identity.hpp>
#include <bloom/color/ocio_builtin_registry.hpp>

#include <cstddef>
#include <cstdint>
#include <iostream>
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

    const auto expected =
        buildExpectedNeutralIdentityBytes(bloom::color::kBloomNeutralV1ConfigDigest);
    const auto actual = handle->identity().canonicalBytes();
    expectations.expect(std::vector<std::byte>(actual.begin(), actual.end()) == expected,
                        "the built processor's canonical DisplayProcessorIdentity bytes equal the "
                        "independently derived golden");

    expectations.expect(handle->lease().kind() ==
                            bloom::color::DisplayProcessorLeaseKind::InProcess,
                        "the Bloom Neutral built-in lease is InProcess");
    expectations.expect(!handle->provenance().ocioVersion.empty(),
                        "execution provenance records a nonempty OCIO version");
    expectations.expect(!handle->provenance().processorCacheId.empty(),
                        "execution provenance records a nonempty OCIO processor cache ID");
}

} // namespace

int main() {
    Expectations expectations;
    testNeutralIdentityGolden(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
