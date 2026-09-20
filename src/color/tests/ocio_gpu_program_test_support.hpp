#pragma once

// Private shared support for the OCIO GPU program unit tests: the expectation sink and the
// built-in config resolvers used by more than one focused test translation unit.

#include <bloom/color/bloom_neutral_builtin.hpp>
#include <bloom/color/ocio_builtin_registry.hpp>

#include <iostream>
#include <string_view>

namespace bloom::color::gpu_program_test {

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

[[nodiscard]] inline OcioBuiltInResolutionResult neutralConfig() {
    return resolveBloomNeutralV1BuiltIn(OcioConfigLocatorKind::BloomBuiltIn,
                                        kBloomNeutralV1ConfigUri, kBloomNeutralV1ConfigDigest);
}

[[nodiscard]] inline OcioBuiltInResolutionResult acesConfig() {
    const auto revision =
        ocioBuiltInContentRevision(OcioConfigLocatorKind::BloomBuiltIn, kAcesCgV1ConfigUri);
    if (!revision.has_value()) {
        return resolveOcioBuiltIn(OcioConfigLocatorKind::BloomBuiltIn, kAcesCgV1ConfigUri,
                                  kBloomNeutralV1ConfigDigest, "ACEScg");
    }
    return resolveOcioBuiltIn(OcioConfigLocatorKind::BloomBuiltIn, kAcesCgV1ConfigUri, *revision,
                              "ACEScg");
}

// Defined in ocio_gpu_program_transport_tests.cpp.
void testSerializationRoundTrip(Expectations& expectations);
void testPerSamplerSamplingAdapter(Expectations& expectations);

} // namespace bloom::color::gpu_program_test
