#include "fake_provider.hpp"
#include <algorithm>
#include <numeric>

namespace bloom::media::fake {
namespace {
provider::MediaCapabilityKeyV1 capability(provider::Role role) {
    provider::MediaCapabilityKeyV1 c;
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
} // namespace
provider::Handshake handshake() {
    provider::Handshake h;
    auto& e = h.execution;
    e.provider = "bloom.fake";
    e.build = BLOOM_MEDIA_BUILD_ID;
    const auto lock = provider::Digest::fromLowercaseHex(BLOOM_MEDIA_DEPENDENCY_LOCK);
    if (lock)
        e.dependencyLock = *lock;
    e.os = BLOOM_MEDIA_OS;
    e.architecture = BLOOM_MEDIA_ARCH;
    e.sdk = "none";
    e.driver = "none";
    e.device = "cpu";
    e.synchronization = "pipe-order";
    e.resourceProfile = "media-v1";
    e.entitlement = "none";
    e.trustDomain = "fake-only";
    provider::QualificationEvidenceV1 q;
    q.reviewDate = "2026-09-17";
    q.qualification = provider::Qualification::PreviewQualified;
    q.result = provider::QcResult::Pass;
    for (const auto role : {provider::Role::Probe, provider::Role::VideoDecode})
        h.declarations.push_back({capability(role), e, q});
    h.transports = {provider::Transport::PipeCopiesV0};
    return h;
}
provider::Payload call(const provider::CallRequest& request) {
    const auto h = handshake();
    if (request.source != "synthetic:rgba8" ||
        std::ranges::none_of(h.declarations,
                             [&](const auto& d) { return d.capability == request.capability; }) ||
        request.width == 0 || request.height == 0 || request.width > 64 || request.height > 64 ||
        request.frame > 100000)
        return provider::Unavailable{provider::Error::Unavailable,
                                     "Fake provider supports only bounded synthetic RGBA8"};
    if (request.capability.role == provider::Role::Probe) {
        provider::ProbeResult p;
        p.container = "bloom.synthetic";
        p.version = "1";
        provider::StreamDescriptor s;
        s.codec = "fake.rgba8";
        s.profile = "none";
        s.timebase = {1, 24};
        s.rate = {24, 1};
        s.width = request.width;
        s.height = request.height;
        p.streams.push_back(s);
        return p;
    }
    provider::FrameProduct f;
    const auto divisor = std::gcd(request.frame, std::uint64_t{24});
    f.pts = {static_cast<std::int64_t>(request.frame / divisor),
             static_cast<std::int64_t>(24 / divisor)};
    provider::CpuPlane p;
    p.width = request.width;
    p.height = request.height;
    p.stride = p.width * 4;
    p.bytes.resize(static_cast<std::size_t>(p.stride) * p.height);
    for (std::uint32_t y = 0; y < p.height; ++y)
        for (std::uint32_t x = 0; x < p.width; ++x) {
            const auto offset =
                static_cast<std::size_t>(y) * p.stride + static_cast<std::size_t>(x) * 4U;
            p.bytes[offset] = static_cast<std::byte>(x);
            p.bytes[offset + 1] = static_cast<std::byte>(y);
            p.bytes[offset + 2] = static_cast<std::byte>(request.frame % 256);
            p.bytes[offset + 3] = std::byte{255};
        }
    p.digest = provider::digestBytes(p.bytes);
    f.planes.push_back(std::move(p));
    return f;
}
} // namespace bloom::media::fake
