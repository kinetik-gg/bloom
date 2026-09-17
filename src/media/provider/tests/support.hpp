#pragma once
#include <bloom/media/provider/contract.hpp>
#include <iostream>
#include <stdexcept>
namespace bloom::media::provider::test {
inline void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
inline MediaCapabilityKeyV1 capability(Role role = Role::Probe) {
    MediaCapabilityKeyV1 c;
    c.role = role;
    for (auto* s : {&c.container,
                    &c.mapping,
                    &c.codec,
                    &c.profile,
                    &c.level,
                    &c.tier,
                    &c.sampleEntry,
                    &c.range,
                    &c.chroma,
                    &c.chromaLocation,
                    &c.alpha,
                    &c.fieldMode,
                    &c.audioFormat,
                    &c.channelLayout,
                    &c.colorFeatures,
                    &c.hdrFeatures,
                    &c.timecodeFeatures,
                    &c.metadataFeatures,
                    &c.timing,
                    &c.surfaceSemantics})
        *s = "none";
    c.container = "bloom.synthetic";
    c.codec = "fake.rgba8";
    c.maxWidth = 64;
    c.maxHeight = 64;
    c.maxRate = {24, 1};
    return c;
}
inline ProviderExecutionKeyV1 execution() {
    ProviderExecutionKeyV1 e;
    e.provider = "bloom.fake";
    e.build = "v1";
    e.os = "Linux";
    e.architecture = "test";
    e.sdk = "none";
    e.driver = "none";
    e.device = "cpu";
    e.synchronization = "pipe-order";
    e.resourceProfile = "media-v1";
    e.entitlement = "none";
    e.trustDomain = "fake-only";
    return e;
}
inline QualificationEvidenceV1 evidence() {
    QualificationEvidenceV1 e;
    e.reviewDate = "2026-09-17";
    e.qualification = Qualification::PreviewQualified;
    e.result = QcResult::Pass;
    return e;
}
} // namespace bloom::media::provider::test
