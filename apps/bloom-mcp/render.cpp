#include "server.hpp"
#include <bloom/core/sha256.hpp>
#include <bloom/host/gpu_export_provider.hpp>
#include <bloom/host/sequence_export_runner.hpp>

#include <array>
#include <chrono>
#include <fstream>
#include <thread>

namespace bloom::mcp {

std::string digestFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    core::Sha256Hasher hasher;
    std::array<char, 65536> bytes{};
    while (input) {
        input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!hasher.update(
                std::as_bytes(std::span(bytes.data(), static_cast<std::size_t>(input.gcount())))))
            throw std::runtime_error("File exceeds SHA-256 limits");
    }
    if (!input.eof())
        throw std::runtime_error("Cannot read published file");
    const auto hex = hasher.finalize().toLowercaseHex();
    return "sha256:" + std::string(hex.data(), hex.size());
}

yyjson_mut_val* Server::render(Json& out, yyjson_val* arguments, const bool compositionExport) {
    members(arguments,
            {"composition", "frame", "first", "last", "preset", "destination", "profile", "audio",
             "sampleRate"},
            {"composition", "preset", "destination"});
    const auto id = integer(member(arguments, "composition"));
    const auto presetName = string(member(arguments, "preset"), 128);
    const std::filesystem::path destination = string(member(arguments, "destination"));
    const bool range = member(arguments, "frame") == nullptr;
    if (destination.empty() ||
        (!range && (member(arguments, "first") || member(arguments, "last"))))
        throw InvalidInput(
            "Specify exactly a frame or inclusive first/last range and a destination");
    const auto first = integer(member(arguments, range ? "first" : "frame"));
    const auto last = range ? integer(member(arguments, "last")) : first;
    if (first > last || last - first >= 10000)
        throw InvalidInput("Range must contain 1..10000 frames");
    std::optional<output::OutputPresetV1> preset;
    constexpr std::array presets{output::OutputPresetV1::PngRgba8SrgbV1,
                                 output::OutputPresetV1::FlatExrRgba32fLinRec709SceneV1,
                                 output::OutputPresetV1::TiffRgba16SrgbV1,
                                 output::OutputPresetV1::ProResMovV1,
                                 output::OutputPresetV1::DnxhrMxfV1,
                                 output::OutputPresetV1::PcmWavV1};
    for (const auto candidate : presets) {
        const auto identity = output::outputPresetIdentityV1(candidate);
        if (identity && identity->serializedId == presetName)
            preset = candidate;
    }
    if (!preset)
        throw InvalidInput("Unknown preset");
    const bool still = *preset == presets[0] || *preset == presets[1] || *preset == presets[2];
    if (!compositionExport && !still)
        throw InvalidInput("Use export for media presets and encoding settings");
    if (still && (member(arguments, "profile") || member(arguments, "audio") ||
                  member(arguments, "sampleRate")))
        throw InvalidInput("Still presets do not accept media encoding settings");
    const auto snapshot = facade_.query.snapshot();
    const auto* composition =
        snapshot.project().findComposition(document::CompositionId::fromRaw(id));
    if (!composition)
        throw InvalidInput("Composition does not exist");
    runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    std::uint64_t frames = 0;
    std::string report;
    if (still) {
        const auto result = scripting::Render::run(
            *session_, scheduler_, compiler,
            {.composition = composition->id(),
             .frame = range ? std::nullopt : std::optional(first),
             .range = range ? std::optional(std::pair(first, last)) : std::nullopt,
             .preset = *preset,
             .destination = destination},
            {}, gpuExportProvider_);
        if (!result.succeeded)
            throw std::runtime_error(result.diagnostic);
        frames = result.publishedFrames;
        report = result.preservationReport;
    } else {
        const auto profile = member(arguments, "profile") ? string(member(arguments, "profile"), 32)
                             : *preset == output::OutputPresetV1::PcmWavV1 ? "pcm_s16le"
                                                                           : "hq";
        const auto rate =
            member(arguments, "sampleRate") ? integer(member(arguments, "sampleRate")) : 48000;
        if (rate < 8000 || rate > 192000)
            throw InvalidInput("Audio sample rate is outside 8000..192000");
        output::ExportResourceLedgerV1 ledger;
        // The server-lifetime GPU final-render provider is reused here; the sequence runner copies
        // it into every per-frame attempt and defers the first evaluation until its bootstrap is
        // terminal, so the first frame is genuinely GPU when a device exists. The unchanged CPU
        // reference path is the fallback otherwise.
        host::SequenceExportRunnerV1 runner(
            scheduler_, compiler, *session_->publicationCoordinator(),
            *session_->artifactCoordinator(), ledger,
            {.composition = {snapshot, composition->id()},
             .range = {.destination = destination,
                       .firstFrame = first,
                       .lastFrame = last,
                       .frameRate = composition->format().frameRate(),
                       .duration = composition->duration()},
             .preset = *preset,
             .profile = profile,
             .audio = member(arguments, "audio") ? boolean(member(arguments, "audio")) : true,
             .sampleRate = static_cast<std::uint32_t>(rate),
             .pcmCodec = *preset == output::OutputPresetV1::PcmWavV1 ? profile : "pcm_s16le",
             .bwfDescription = {},
             .assetBaseDirectory = session_->displayPath().parent_path(),
             .worker = {},
             .gpuProvider = gpuExportProvider_});
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(10);
        while (!runner.result()) {
            runner.poll();
            if (cancellationRequested_.load() || std::chrono::steady_clock::now() > deadline)
                runner.cancel();
            if (runner.stage() == host::SequenceExportStageV1::AwaitingApproval) {
                const auto* analysis = runner.analysis();
                const auto digest = runner.frameApprovalDigest();
                if (!analysis || !digest || !runner.approve(analysis->digest, *digest))
                    runner.cancel();
                else
                    report = analysis->implementationNote;
            }
            if (!runner.result())
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!runner.result()->published())
            throw std::runtime_error(runner.result()->failure ? runner.result()->failure->detail
                                                              : "Export was not published");
        frames = runner.result()->encodedFrames;
    }
    auto* result = out.object();
    out.set(result, "publishedFrames", out.number(frames));
    out.set(result, "preservationReport", out.text(report));
    auto* files = out.array();
    const auto add = [&](const std::filesystem::path& path) {
        auto* file = out.object();
        out.set(file, "path", out.text(path.string()));
        out.set(file, "digest", out.text(digestFile(path)));
        out.append(files, file);
    };
    if (still && range) {
        for (auto index = first;; ++index) {
            add(host::FrameRangeRunnerV1::sequenceFramePath(destination, index, last));
            if (index == last)
                break;
        }
    } else
        add(destination);
    out.set(result, "files", files);
    return result;
}
} // namespace bloom::mcp
