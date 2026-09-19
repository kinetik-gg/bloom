#include <bloom/media/provider/ffmpeg_manifest.hpp>
#include <bloom/output/media_output.hpp>
#include <iostream>
#include <stdexcept>
namespace {
void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
bloom::output::MediaOutputAnalysisV1 analyze(bloom::output::OutputPresetV1 preset,
                                             bloom::media::provider::EncodeSettingsV1 settings) {
    auto result = bloom::output::analyzeMediaOutputV1(preset, std::move(settings));
    check(std::holds_alternative<bloom::output::MediaOutputAnalysisV1>(result),
          "media analysis succeeds");
    return std::get<bloom::output::MediaOutputAnalysisV1>(std::move(result));
}
} // namespace
int main() {
    try {
        namespace p = bloom::media::provider;
        namespace o = bloom::output;
        p::EncodeSettingsV1 s;
        s.width = 256;
        s.height = 128;
        s.frames = 48;
        s.audioCodec = "pcm_s16le";
        s.audioSamples = 96000;
        const auto report = analyze(o::OutputPresetV1::ProResMovV1, s);
        const auto revision = bloom::color::ocioBuiltInContentRevision(
            bloom::color::OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kAcesCgV1ConfigUri);
        if (revision) {
            const auto display = o::PreparedOutputDisplayV1::prepare(
                {.workingColorSpaceId = "ACEScg",
                 .ocioConfigRevision = *revision,
                 .ocioConfigUri = bloom::color::kAcesCgV1ConfigUri});
            check(display != nullptr, "ACES display processor available");
            check(display->description().find("view: ACES 1.0 - SDR Video") != std::string::npos,
                  "config default view recorded");
            const auto rec709 = o::PreparedOutputDisplayV1::prepare(
                {.workingColorSpaceId = "ACEScg",
                 .ocioConfigRevision = *revision,
                 .ocioConfigUri = bloom::color::kAcesCgV1ConfigUri},
                "Rec.1886 Rec.709 - Display", "ACES 1.0 - SDR Video");
            check(rec709 && rec709->digest() != display->digest(),
                  "Rec.709 review pair has its own identity");
        }
#if !defined(__APPLE__)
        const auto hex = report.digest.toLowercaseHex();
#endif
        std::size_t recordBytes = std::string_view("BloomMediaOutputAnalysisV1").size() + 1 + 32;
        for (const auto& facet : report.facets)
            recordBytes += 3 + facet.description.size();
#if defined(__APPLE__)
        // The exact oracle below is pinned to the FFmpeg provider's ProRes wording. macOS uses the
        // native Apple ProRes note, so its record is deliberately different: keep the structural
        // and authority checks and leave the frozen Linux/FFmpeg oracle to that platform.
        check(recordBytes > 0 &&
                  report.display->processor().identity().canonicalBytes().size() == 224,
              "identity record sizes pinned");
#else
        check(recordBytes == 759 &&
                  report.display->processor().identity().canonicalBytes().size() == 224,
              "identity record sizes pinned");
        // Independent Python byte oracle: domain + 04 + frozen 146-byte settings hash + eleven
        // explicit (facet byte,state byte,NUL-terminated UTF-8 description) tuples: 759 bytes. See
        // tests/color5_identity_oracle.py.
        check(std::string(hex.data(), hex.size()) ==
                  "881bec4a9feff91651f94191980833942eb3e0708fa814dcc2af9e20a36599d9",
              "media analysis frozen oracle");
#endif
        check(report.implementationNote.starts_with(p::kProResExportNote) &&
                  !report.appleAuthorized && !report.deliveryQualified,
              "ProRes wording and authority");
        check(report.facets[3].preservation == o::OutputPreservationStateV1::Omitted,
              "422 alpha omitted");
        auto silent = s;
        silent.audioCodec.clear();
        silent.audioSamples = 0;
        check(analyze(o::OutputPresetV1::ProResMovV1, silent).facets[4].preservation ==
                  o::OutputPreservationStateV1::Omitted,
              "audio-off analysis discloses omitted channels");
        s.profile = "4444";
        const auto alpha = analyze(o::OutputPresetV1::ProResMovV1, s);
        check(alpha.digest != report.digest &&
                  alpha.facets[3].preservation == o::OutputPreservationStateV1::Approximated,
              "profile and alpha bind analysis");
        s.videoCodec.clear();
        s.frames = 0;
        s.container = "wav";
        const auto pcm = analyze(o::OutputPresetV1::PcmWavV1, s);
        check(pcm.determinism == p::MediaDeterminismV1::ByteExact &&
                  pcm.toleranceProfile == p::Digest{},
              "PCM byte-exact identity");
        s.bwfDescription = "BWF metadata";
        check(analyze(o::OutputPresetV1::PcmWavV1, s).digest != pcm.digest,
              "BWF metadata binds approval");
        p::EncodeSettingsV1 h264;
        h264.width = 256;
        h264.height = 128;
        h264.frames = 48;
        h264.videoCodec = "h264";
        h264.profile = "high";
        h264.container = "mov";
        h264.audioCodec = "pcm_s16le";
        h264.audioSamples = 96000;
        const auto review = analyze(o::OutputPresetV1::H264MovV1, h264);
        check(review.determinism == p::MediaDeterminismV1::DecodedSemanticTolerance &&
                  review.toleranceProfile != p::Digest{} &&
                  review.implementationNote.starts_with("Review deliverable — not for archival"),
              "H.264 review analysis records lossy tolerance and archival warning");
        check(std::holds_alternative<p::Unavailable>(
                  o::analyzeMediaOutputV1(o::OutputPresetV1::ProResMovV1, s)),
              "codec/preset mismatch refused");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
