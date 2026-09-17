#include "wire.hpp"

namespace bloom::media::provider::wire {
namespace {
void required(Writer& w, const std::string& s) {
    require(!s.empty());
    w.text(s);
}
void authority(Writer& w, const AuthorityRecordV1& v) {
    const bool absent = v.issuer.empty();
    require(!absent || (v.reference.empty() && v.product.empty() && v.scope.empty() &&
                        v.verifiedDate.empty() && v.reviewDate.empty()));
    for (const auto* s :
         {&v.issuer, &v.reference, &v.product, &v.scope, &v.verifiedDate, &v.reviewDate}) {
        require(absent || !s->empty());
        w.text(*s);
    }
}
AuthorityRecordV1 authority(Reader& r) {
    AuthorityRecordV1 v;
    v.issuer = r.text();
    v.reference = r.text();
    v.product = r.text();
    v.scope = r.text();
    v.verifiedDate = r.text();
    v.reviewDate = r.text();
    return v;
}
} // namespace
void write(Writer& w, const MediaCapabilityKeyV1& v) {
    w.enumeration(v.role, 1, 10);
    w.enumeration(v.purpose, 1, 5);
    for (const auto* s :
         {&v.container, &v.mapping, &v.codec, &v.profile, &v.level, &v.tier, &v.sampleEntry})
        required(w, *s);
    require(v.bitDepth > 0 && v.bitDepth <= 32);
    w.number(v.bitDepth);
    for (const auto* s : {&v.range, &v.chroma, &v.chromaLocation, &v.alpha, &v.fieldMode})
        required(w, *s);
    require(v.maxWidth > 0 && v.maxWidth <= Limits::dimension && v.maxHeight > 0 &&
            v.maxHeight <= Limits::dimension);
    w.number(v.maxWidth);
    w.number(v.maxHeight);
    require(v.maxRate.numerator > 0);
    w.rational(v.maxRate);
    for (const auto* s : {&v.audioFormat, &v.channelLayout, &v.colorFeatures, &v.hdrFeatures,
                          &v.timecodeFeatures, &v.metadataFeatures, &v.timing, &v.surfaceSemantics})
        required(w, *s);
}
MediaCapabilityKeyV1 capability(Reader& r) {
    MediaCapabilityKeyV1 v;
    v.role = r.enumeration<Role>(1, 10);
    v.purpose = r.enumeration<Purpose>(1, 5);
    for (auto* s :
         {&v.container, &v.mapping, &v.codec, &v.profile, &v.level, &v.tier, &v.sampleEntry})
        *s = r.text();
    v.bitDepth = r.number<std::uint32_t>();
    for (auto* s : {&v.range, &v.chroma, &v.chromaLocation, &v.alpha, &v.fieldMode})
        *s = r.text();
    v.maxWidth = r.number<std::uint32_t>();
    v.maxHeight = r.number<std::uint32_t>();
    v.maxRate = r.rational();
    for (auto* s : {&v.audioFormat, &v.channelLayout, &v.colorFeatures, &v.hdrFeatures,
                    &v.timecodeFeatures, &v.metadataFeatures, &v.timing, &v.surfaceSemantics})
        *s = r.text();
    Writer validation;
    write(validation, v);
    return v;
}
void write(Writer& w, const ProviderExecutionKeyV1& v) {
    required(w, v.provider);
    required(w, v.build);
    w.hash(v.dependencyLock);
    require(v.protocol == 1, Error::VersionMismatch);
    w.number(v.protocol);
    required(w, v.os);
    required(w, v.architecture);
    w.enumeration(v.implementation, 1, 2);
    for (const auto* s : {&v.sdk, &v.driver, &v.device})
        required(w, *s);
    require(v.generation != 0);
    w.number(v.generation);
    w.enumeration(v.transport, 0, 1);
    for (const auto* s : {&v.synchronization, &v.resourceProfile, &v.entitlement, &v.trustDomain})
        required(w, *s);
    w.enumeration(v.availability, 1, 2);
}
ProviderExecutionKeyV1 execution(Reader& r) {
    ProviderExecutionKeyV1 v;
    v.provider = r.text();
    v.build = r.text();
    v.dependencyLock = r.hash();
    v.protocol = r.number<std::uint16_t>();
    v.os = r.text();
    v.architecture = r.text();
    v.implementation = r.enumeration<Implementation>(1, 2);
    v.sdk = r.text();
    v.driver = r.text();
    v.device = r.text();
    v.generation = r.number<std::uint64_t>();
    v.transport = r.enumeration<Transport>(0, 1);
    v.synchronization = r.text();
    v.resourceProfile = r.text();
    v.entitlement = r.text();
    v.trustDomain = r.text();
    v.availability = r.enumeration<Availability>(1, 2);
    Writer validation;
    write(validation, v);
    return v;
}
void write(Writer& w, const QualificationEvidenceV1& v) {
    w.hash(v.fixtures);
    w.enumeration(v.qualification, 1, 5);
    w.enumeration(v.result, 1, 3);
    required(w, v.reviewDate);
    w.text(v.reviewDeadline);
    authority(w, v.authority);
}
QualificationEvidenceV1 evidence(Reader& r) {
    QualificationEvidenceV1 v;
    v.fixtures = r.hash();
    v.qualification = r.enumeration<Qualification>(1, 5);
    v.result = r.enumeration<QcResult>(1, 3);
    v.reviewDate = r.text();
    v.reviewDeadline = r.text();
    v.authority = authority(r);
    Writer validation;
    write(validation, v);
    return v;
}
void write(Writer& w, const PipelineQualificationV1& v) {
    require(!v.steps.empty());
    w.count(v.steps.size());
    for (const auto& s : v.steps) {
        w.hash(s.capability);
        w.hash(s.execution);
        w.hash(s.evidence);
    }
    w.enumeration(v.purpose, 1, 5);
    required(w, v.profile);
    w.hash(v.fixtures);
    w.count(v.conversionVersions.size());
    for (const auto& c : v.conversionVersions)
        required(w, c);
    w.enumeration(v.determinism, 1, 4);
    require(v.determinism != MediaDeterminismV1::NoDeterminismClaim ||
            v.purpose == Purpose::Preview);
    require((v.determinism == MediaDeterminismV1::DecodedSemanticTolerance) ==
            (v.toleranceProfile != Digest{}));
    w.hash(v.toleranceProfile);
    required(w, v.reopenPolicy);
    required(w, v.qcProfile);
    w.enumeration(v.result, 1, 3);
}
void write(Writer& w, const MediaQcEvidenceV1& v) {
    w.hash(v.artifact);
    w.hash(v.snapshot);
    w.hash(v.preset);
    w.hash(v.pipeline);
    w.hash(v.execution);
    for (const auto* s : {&v.tool, &v.version, &v.profile, &v.coverage})
        required(w, *s);
    w.number<std::uint8_t>(v.independentReader ? 1 : 0);
    authority(w, v.externalQc);
    w.enumeration(v.result, 1, 3);
}
} // namespace bloom::media::provider::wire
