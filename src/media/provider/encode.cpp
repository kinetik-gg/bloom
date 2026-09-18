#include "wire.hpp"
#include <bloom/media/provider/encode.hpp>
#include <limits>

namespace bloom::media::provider {
bool valid(const EncodeSettingsV1& v) {
    return v.width > 0 && v.height > 0 && v.width <= Limits::dimension &&
           v.height <= Limits::dimension &&
           static_cast<std::uint64_t>(v.width) * v.height <= Limits::pixels && valid(v.rate) &&
           v.rate.numerator > 0 && v.rate.numerator <= 1000000 && v.rate.denominator <= 1000000 &&
           v.frames <= Limits::indexEntries && v.audioSamples <= 86400ULL * Limits::sampleRate &&
           v.sampleRate > 0 && v.sampleRate <= Limits::sampleRate && v.channels > 0 &&
           v.channels <= 8 && v.byteLimit > 0 && v.byteLimit <= Limits::sourceBytes &&
           v.bwfDescription.size() <= 256 && wire::validText(v.bwfDescription) &&
           !v.container.empty() && v.container.size() <= Limits::stringBytes &&
           wire::validText(v.container) && v.videoCodec.size() <= Limits::stringBytes &&
           wire::validText(v.videoCodec) && v.profile.size() <= Limits::stringBytes &&
           wire::validText(v.profile) && v.audioCodec.size() <= Limits::stringBytes &&
           wire::validText(v.audioCodec) && (!v.videoCodec.empty() || !v.audioCodec.empty()) &&
           (v.videoCodec.empty() ? v.frames == 0 : v.frames > 0) &&
           (v.audioCodec.empty() ? v.audioSamples == 0 : v.audioSamples > 0);
}
MediaDeterminismV1 encodeDeterminism(const EncodeSettingsV1& v) {
    if (v.videoCodec.empty() && v.audioCodec.starts_with("pcm_"))
        return MediaDeterminismV1::ByteExact;
    if (v.videoCodec == "tiff")
        return MediaDeterminismV1::DecodedSemanticExact;
    return MediaDeterminismV1::DecodedSemanticTolerance;
}
Digest encodeTolerance(const EncodeSettingsV1& v) {
    if (encodeDeterminism(v) != MediaDeterminismV1::DecodedSemanticTolerance)
        return {};
    if (!v.videoCodec.empty() && v.audioCodec == "aac") {
        core::Sha256Hasher hasher;
        (void)hasher.update(
            std::as_bytes(std::span(kVideoToleranceV1.data(), kVideoToleranceV1.size())));
        (void)hasher.update(
            std::as_bytes(std::span(kAacToleranceV1.data(), kAacToleranceV1.size())));
        return hasher.finalize();
    }
    const auto profile = v.videoCodec.empty() ? kAacToleranceV1 : kVideoToleranceV1;
    return digestBytes(std::as_bytes(std::span(profile.data(), profile.size())));
}
Result<Digest> digest(const EncodeSettingsV1& v) {
    if (!valid(v))
        return Unavailable{Error::InvalidValue, "Invalid encode settings"};
    wire::Writer w;
    w.text("EncodeSettingsV1");
    w.number<std::uint16_t>(1);
    for (const auto* s : {&v.container, &v.videoCodec, &v.profile, &v.audioCodec})
        w.text(*s);
    w.number(v.width);
    w.number(v.height);
    w.rational(v.rate);
    w.number(v.frames);
    w.number(v.audioSamples);
    w.number(v.sampleRate);
    w.number(v.channels);
    w.text(v.bwfDescription);
    w.enumeration(encodeDeterminism(v), 1, 4);
    w.hash(encodeTolerance(v));
    return digestBytes(w.bytes);
}
} // namespace bloom::media::provider
