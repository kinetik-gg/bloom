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
        const auto hex = report.digest.toLowercaseHex();
        // Independent Python byte oracle: domain + 04 + frozen 146-byte settings hash + eleven
        // explicit (facet byte,state byte,NUL-terminated UTF-8 description) tuples: 579 bytes.
        check(std::string(hex.data(), hex.size()) ==
                  "9a5f56e4290356b06d838af31b272da8d83339b2bd9bd8fd8eb4ed6bf9771350",
              "media analysis frozen oracle");
        check(report.implementationNote == p::kProResExportNote && !report.appleAuthorized &&
                  !report.deliveryQualified,
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
        check(std::holds_alternative<p::Unavailable>(
                  o::analyzeMediaOutputV1(o::OutputPresetV1::ProResMovV1, s)),
              "codec/preset mismatch refused");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
