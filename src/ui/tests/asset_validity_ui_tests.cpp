// Assets validity on real media in the application's AssetController (the path the owner's ⚠
// comes from): an ordinary H.264 High + AAC video and a data-only Cryptomatte EXR sequence are
// valid, not "Missing — Relink", and each gets a thumbnail. Extra file paths on the command line
// are imported and reported the same way.

#include "cryptomatte_fixture_support.hpp"

#include <QApplication>
#include <QProcess>
#include <QStandardPaths>
#include <QElapsedTimer>
#include <QStringList>
#include <QTest>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/asset.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/commands/asset_operations.hpp>
#include <bloom/runtime/input_color_context.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/asset_controller.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/project_host.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

#include <filesystem>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace {
using namespace bloom;

int failures = 0;
void expect(const bool value, const std::string& message) {
    if (!value) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

template <typename Predicate> bool waitUntil(Predicate predicate, const int milliseconds = 60000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < milliseconds)
        QTest::qWait(10);
    return predicate();
}

struct Harness final {
    runtime::TaskScheduler scheduler;
    ui::ProjectHost host{scheduler};
    ui::CompositionSession session{*host.liveDocumentAndStack().first,
                                   *host.liveDocumentAndStack().second, host.lowestCompositionId()};
    ui::TaskUiBridge bridge{scheduler};
    std::unique_ptr<ui::AssetController> controller;
    Harness() {
        if (session.composition() == nullptr) {
            commands::Transaction seed("Seed", session.snapshot().revision());
            seed.emplace<commands::AddComposition>("Main", document::CompositionFormat{},
                                                   core::RationalTime::fromInteger(10));
            const auto seeded = session.executeTransaction(std::move(seed));
            const auto id = seeded.succeeded() ? seeded.outputId<document::CompositionId>(
                                                     commands::kAddCompositionOutput)
                                               : std::nullopt;
            if (!id || !session.setComposition(*id))
                throw std::runtime_error("composition seed");
        }
        // A standalone audio allowance, as an application root grants through its phase policy.
        controller = std::make_unique<ui::AssetController>(
            session, host, scheduler, bridge, nullptr, nullptr,
            ui::AudioProcessingAdmission{.policy = nullptr,
                                         .standaloneByteAllowance = 256U * 1024U * 1024U});
    }
    ~Harness() {
        controller->cancel();
        bridge.beginShutdown();
        scheduler.beginShutdown();
        QElapsedTimer timer;
        timer.start();
        while (!scheduler.isQuiescent() && timer.elapsed() < 10000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    Harness(const Harness&) = delete;
    Harness& operator=(const Harness&) = delete;
};

// Imports `path` and reports the asset the way the Assets panel reads it.
struct Outcome final {
    bool imported = false;
    bool missing = true;
    bool thumbnail = false;
    bool dataOnly = false;
    std::string kind;
    std::string problem;
    std::string audio;
    std::string note;
    // After choosing the config's Rec.709 video space (the fix the problem names).
    bool thumbnailAfterChoice = false;
};
Outcome importAndInspect(const std::filesystem::path& path, const bool chooseRec709 = false) {
    Harness harness;
    Outcome outcome;
    harness.controller->importFiles({QString::fromStdString(path.string())});
    waitUntil([&] { return !harness.controller->busy(); });
    const auto& assets = harness.session.snapshot().project().assets();
    if (assets.empty())
        return outcome;
    outcome.imported = true;
    const auto& asset = assets.front();
    outcome.kind = std::to_string(static_cast<int>(asset.kind));
    // Settled: a thumbnail, a missing source, or a stated problem.
    waitUntil([&] {
        return !harness.controller->thumbnail(asset.id).isNull() ||
               harness.controller->missing(asset.id) ||
               !harness.controller->problem(asset.id).isEmpty();
    });
    outcome.missing = harness.controller->missing(asset.id);
    outcome.thumbnail = !harness.controller->thumbnail(asset.id).isNull();
    outcome.dataOnly = harness.controller->dataOnly(asset.id);
    outcome.problem = harness.controller->problem(asset.id).toStdString();
    outcome.audio = harness.controller->audioDiagnostic(asset.id).toStdString();
    if (chooseRec709) {
        const auto config = runtime::detail::resolveInputColorConfig(harness.session.colorIntent());
        if (config && !config->rec709VideoColorSpaceId().empty()) {
            commands::Transaction choose("Input Colour", harness.session.snapshot().revision());
            choose.emplace<commands::SetAssetInputColor>(
                asset.id, asset.interpretation.colorSpace,
                std::string(config->rec709VideoColorSpaceId()));
            if (harness.session.executeTransaction(std::move(choose)).succeeded())
                outcome.thumbnailAfterChoice = waitUntil(
                    [&] { return !harness.controller->thumbnail(asset.id).isNull(); }, 30000);
        }
    }
    outcome.note = (harness.controller->audioDiagnostic(asset.id) + " | " +
                    harness.controller->inputColorSpaceWarning(asset.id))
                       .toStdString();
    return outcome;
}

void report(const std::string& label, const Outcome& outcome) {
    std::cout << label << ": imported=" << outcome.imported << " kind=" << outcome.kind
              << " missing=" << outcome.missing << " thumbnail=" << outcome.thumbnail
              << " dataOnly=" << outcome.dataOnly << " problem=" << outcome.problem
              << " audio=" << outcome.audio << '\n';
}

} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    try {
        const auto arguments = QCoreApplication::arguments();
        std::vector<std::filesystem::path> extra;
        std::filesystem::path fixtures;
        for (int index = 1; index < arguments.size(); ++index) {
            if (arguments[index] == "--media-fixtures" && index + 1 < arguments.size())
                fixtures = arguments[++index].toStdString();
            else
                extra.emplace_back(arguments[index].toStdString());
        }
        std::mt19937_64 random{std::random_device{}()};
        const auto folder = std::filesystem::temp_directory_path() /
                            ("bloom-asset-validity-" + std::to_string(random()));
        std::filesystem::create_directories(folder);
        // A sidecar-style, Cryptomatte-only EXR sequence: CryptoAsset ranks, no RGB.
        for (int frame = 1; frame <= 3; ++frame)
            bloom::cryptomatte_fixture::writeCryptomatteExr(
                folder / ("shot_cryptomatte_asset_000" + std::to_string(frame) + ".exr"), true,
                "CryptoAsset", {}, static_cast<std::size_t>(frame - 1), false);
        const auto crypto = importAndInspect(folder / "shot_cryptomatte_asset_0001.exr");
        report("cryptomatte-only sequence", crypto);
        expect(crypto.imported && !crypto.missing,
               "a Cryptomatte-only EXR sequence is a valid asset, not missing");
        expect(crypto.thumbnail && crypto.dataOnly && crypto.problem.empty(),
               "a Cryptomatte-only EXR sequence is data-only with an ID-colour thumbnail");
        expect(crypto.kind == std::to_string(static_cast<int>(document::AssetKind::Sequence)),
               "a Cryptomatte-only EXR sequence is classified as a sequence");
        // An untagged H.264 stream is present: never "missing", the colour refusal is a stated
        // problem naming its fix, and its audio still decodes.
        const auto checkUntagged = [](const std::string& label, const Outcome& video,
                                      const bool hasAudio) {
            expect(video.imported && !video.missing, label + ": a present video is not missing");
            expect(video.problem.find("input colour space") != std::string::npos,
                   label + ": the colour refusal names its fix");
            if (hasAudio)
                expect(video.audio.empty(), label + ": its audio decodes");
            expect(video.thumbnailAfterChoice,
                   label + ": choosing Rec.709 as its input colour space gives a thumbnail");
        };
        if (!fixtures.empty()) {
            const auto video = importAndInspect(fixtures / "untagged-h264.mp4", true);
            report("untagged h264 video", video);
            checkUntagged("untagged H.264", video, false);
        }
        // A real H.264 High + AAC LC clip, as cameras and editors write them (no colour tags).
        // Encoded at test time when FFmpeg with libx264 and AAC is on the PATH.
        const auto ffmpeg = QStandardPaths::findExecutable("ffmpeg");
        if (!ffmpeg.isEmpty()) {
            const auto clip = folder / "h264_high_aac.mp4";
            QProcess encode;
            encode.start(ffmpeg, {"-v", "error", "-f", "lavfi", "-i",
                                  "testsrc=size=320x180:rate=24:duration=1", "-f", "lavfi", "-i",
                                  "sine=frequency=440:sample_rate=48000:duration=1", "-c:v",
                                  "libx264", "-profile:v", "high", "-pix_fmt", "yuv420p", "-c:a",
                                  "aac", "-shortest", QString::fromStdString(clip.string())});
            if (encode.waitForFinished(60000) && encode.exitCode() == 0 &&
                std::filesystem::exists(clip)) {
                const auto video = importAndInspect(clip, true);
                report("h264 high + aac", video);
                checkUntagged("H.264 High + AAC", video, true);
            } else {
                std::cout << "NOTE: FFmpeg could not encode the H.264 High + AAC clip\n";
            }
        } else {
            std::cout << "NOTE: no FFmpeg on PATH; the H.264 High + AAC clip is not exercised\n";
        }
        for (const auto& path : extra)
            report(path.filename().string(), importAndInspect(path));
        std::error_code error;
        std::filesystem::remove_all(folder, error);
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    if (failures != 0)
        return 1;
    std::cout << "PASS: asset validity\n";
    return 0;
}
