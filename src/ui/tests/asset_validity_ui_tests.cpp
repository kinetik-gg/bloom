// Assets validity on real media in the application's AssetController (the path the owner's ⚠
// comes from): an ordinary H.264 High + AAC video and a data-only Cryptomatte EXR sequence are
// valid, not "Missing — Relink", and each gets a thumbnail. Extra file paths on the command line
// are imported and reported the same way.

#include "cryptomatte_fixture_support.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QProcess>
#include <QStandardPaths>
#include <QStringList>
#include <QTest>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <bloom/commands/asset_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/asset.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/input_color_context.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/asset_controller.hpp>
#include <bloom/ui/assets_editor.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/project_host.hpp>
#include <bloom/ui/task_ui_bridge.hpp>
#include <bloom/ui/window_status_bar.hpp>

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
    std::string colourNote;
    std::string inputDisplay;
    bool assumed = false;
    // The Assets selection header's facts line and colour line for the asset (#380).
    std::string headerKind;
    std::string headerColour;
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
    outcome.colourNote = harness.controller->inputColorSpaceWarning(asset.id).toStdString();
    outcome.inputDisplay = harness.controller->inputColorSpaceDisplay(asset.id).toStdString();
    outcome.assumed = harness.controller->inputColorSpaceAssumed(asset.id);
    {
        // The selection header reads the same controller state the tree does.
        ui::AssetsEditor editor(harness.session);
        auto* tree = editor.findChild<QTreeWidget*>("assetsTree");
        for (QTreeWidgetItemIterator it(tree); tree && *it; ++it)
            if ((*it)->text(0) == QString::fromStdString(asset.name))
                tree->setCurrentItem(*it);
        QCoreApplication::processEvents();
        const auto line = [&](const char* name) {
            auto* label = editor.findChild<ui::kit::KLabel*>(name);
            return label ? label->toolTip().toStdString() : std::string{};
        };
        outcome.headerKind = line("assetsSelectionKind");
        outcome.headerColour = line("assetsSelectionColour");
    }
    if (chooseRec709) {
        const auto config = runtime::detail::resolveInputColorConfig(harness.session.colorIntent());
        if (config && !config->rec709VideoColorSpaceId().empty()) {
            commands::Transaction choose("Input Color", harness.session.snapshot().revision());
            choose.emplace<commands::SetAssetInputColor>(
                asset.id, asset.interpretation.colorSpace,
                std::string(config->rec709VideoColorSpaceId()));
            if (harness.session.executeTransaction(std::move(choose)).succeeded())
                // The explicit choice replaces the assumption (and its note) with a new thumbnail.
                outcome.thumbnailAfterChoice = waitUntil(
                    [&] {
                        return !harness.controller->inputColorSpaceAssumed(asset.id) &&
                               harness.controller->inputColorSpaceWarning(asset.id).isEmpty() &&
                               !harness.controller->thumbnail(asset.id).isNull();
                    },
                    30000);
        }
    }
    outcome.note = (harness.controller->audioDiagnostic(asset.id) + " | " +
                    harness.controller->inputColorSpaceWarning(asset.id))
                       .toStdString();
    return outcome;
}

// The status bar's video cell (owner decision 2026-10-24, issue #352): after the evaluator decodes
// an untagged video, the bar names the decode route and the assumed-colour note, without the note
// becoming an evaluation diagnostic.
void statusBarVideoNote(const std::filesystem::path& clip, const std::string& family) {
    Harness harness;
    harness.controller->importFiles({QString::fromStdString(clip.string())});
    waitUntil([&] { return !harness.controller->busy(); });
    const auto& assets = harness.session.snapshot().project().assets();
    expect(!assets.empty(), "status bar: the clip imports");
    if (assets.empty())
        return;
    const auto composition = harness.session.compositionId();
    commands::Transaction layer("Video layer", harness.session.snapshot().revision());
    layer.emplace<commands::AddImageLayer>(composition, assets.front().id);
    expect(harness.session.executeTransaction(std::move(layer)).succeeded(),
           "status bar: a Video layer is added");
    runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    const auto compiled = compiler.compile({harness.session.snapshot(), composition}, {});
    expect(compiled.plan != nullptr, "status bar: the plan compiles");
    if (!compiled.plan)
        return;
    runtime::CpuCompositionEvaluator evaluator;
    ui::WindowStatusBar bar(harness.session, nullptr, nullptr, evaluator.operationCache().get());
    bar.refreshVideoCellForTest();
    expect(bar.videoTextForTest().isEmpty(), "status bar: no video cell before a decode");
    const auto result = evaluator.evaluate(compiled.plan,
                                           {.time = core::RationalTime{},
                                            .output = compiled.plan->output(),
                                            .resolution = {},
                                            .pixelStorageByteLimit = std::size_t{64} << 20U},
                                           {});
    expect(result.frame() && result.diagnostics().empty(),
           "status bar: the untagged video renders with no evaluation diagnostic");
    bar.refreshVideoCellForTest();
    const auto text = bar.videoTextForTest().toStdString();
    std::cout << "status bar video cell: " << text << '\n';
    expect(text.starts_with("Video: software") &&
               text.ends_with("Color tags missing — assumed " + family +
                              " with the sRGB transfer. Change in Assets → Input Color"),
           "status bar: the decode route and the assumed-colour note are shown");
    expect(bar.videoToolTipForTest().contains(QString::fromStdString(assets.front().name)),
           "status bar: the tooltip names the source");
}

void report(const std::string& label, const Outcome& outcome) {
    std::cout << label << ": imported=" << outcome.imported << " kind=" << outcome.kind
              << " missing=" << outcome.missing << " thumbnail=" << outcome.thumbnail
              << " dataOnly=" << outcome.dataOnly << " problem=" << outcome.problem
              << " audio=" << outcome.audio << " color=" << outcome.colourNote
              << " input=" << outcome.inputDisplay << '\n';
}

} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    try {
        const auto arguments = QCoreApplication::arguments();
        std::vector<std::filesystem::path> extra;
        // Local files (never committed) that must render in Auto with the Rec.709 note, such as an
        // owner's untagged 720p camera clip.
        std::vector<std::filesystem::path> assumedHd;
        std::filesystem::path fixtures;
        for (int index = 1; index < arguments.size(); ++index) {
            if (arguments[index] == "--media-fixtures" && index + 1 < arguments.size())
                fixtures = arguments[++index].toStdString();
            else if (arguments[index] == "--expect-assumed-hd" && index + 1 < arguments.size())
                assumedHd.emplace_back(arguments[++index].toStdString());
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
        // #380: an sRGB PNG's header colour line reads "sRGB", never the config identifier.
        {
            QImage image(16, 16, QImage::Format_RGB32);
            image.fill(Qt::darkCyan);
            const auto png = folder / "plain.png";
            image.save(QString::fromStdString(png.string()));
            const auto still = importAndInspect(png);
            report("png still", still);
            expect(still.headerColour.starts_with("Color: sRGB (from the file)") &&
                       still.headerColour.find("srgb_rec709_display") == std::string::npos,
                   "a PNG header names its color space sRGB, got " + still.headerColour);
        }
        // Untagged video (owner decision 2026-10-24): Auto no longer refuses. It assumes the size
        // convention (Rec.709 for HD, Rec.601 for SD), shows a thumbnail and the visible note
        // naming the fix, and its audio still decodes; choosing Rec.709 explicitly still works.
        const auto checkUntagged = [](const std::string& label, const Outcome& video,
                                      const bool hasAudio, const std::string& family) {
            expect(video.imported && !video.missing, label + ": a present video is not missing");
            expect(video.thumbnail && video.problem.empty(),
                   label + ": Auto shows a thumbnail instead of a color refusal");
            expect(video.assumed && video.colourNote == "Color tags missing — assumed " + family +
                                                            " with the sRGB transfer. Change in "
                                                            "Assets → Input Color",
                   label + ": the assumption is a visible note naming its fix");
            expect(video.inputDisplay.rfind("Auto (assumed: ", 0) == 0,
                   label + ": the input color display says the space was assumed");
            // #380: the header names the space plainly and states the assumption once.
            const auto count = [&](const std::string& part) {
                std::size_t found = 0;
                for (auto at = video.headerColour.find(part); at != std::string::npos;
                     at = video.headerColour.find(part, at + part.size()))
                    ++found;
                return found;
            };
            expect(video.headerColour.starts_with("Color: ") && count("lin_rec709_scene") == 0 &&
                       count("srgb_rec709_display") == 0 && count("(Bloom)") == 0,
                   label + ": the header shows a plain color-space name, got " +
                       video.headerColour);
            expect(count("Color tags missing") == 1 && count("assumed") == 1,
                   label + ": the header shows the assumed-colour note once");
            expect(video.headerKind.find(" frames), ") != std::string::npos &&
                       video.headerKind.find("Δ") == std::string::npos,
                   label + ": the header shows the duration as timecode with frames, got " +
                       video.headerKind);
            if (hasAudio)
                expect(video.audio.empty(), label + ": its audio decodes");
            expect(video.thumbnailAfterChoice,
                   label + ": choosing Rec.709 as its input color space gives a thumbnail");
        };
        if (!fixtures.empty())
            statusBarVideoNote(fixtures / "untagged-hd-prores.mov", "Rec.709");
        if (!fixtures.empty()) {
            const auto video = importAndInspect(fixtures / "untagged-h264.mp4", true);
            report("untagged h264 video (SD)", video);
            checkUntagged("untagged SD H.264", video, false, "Rec.601");
            const auto hd = importAndInspect(fixtures / "untagged-hd-prores.mov", true);
            report("untagged ProRes video (HD)", hd);
            checkUntagged("untagged HD ProRes", hd, false, "Rec.709");
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
                checkUntagged("H.264 High + AAC", video, true, "Rec.601");
            } else {
                std::cout << "NOTE: FFmpeg could not encode the H.264 High + AAC clip\n";
            }
        } else {
            std::cout << "NOTE: no FFmpeg on PATH; the H.264 High + AAC clip is not exercised\n";
        }
        for (const auto& path : assumedHd) {
            const auto video = importAndInspect(path);
            report(path.filename().string(), video);
            expect(video.imported && !video.missing && video.thumbnail && video.problem.empty() &&
                       video.assumed &&
                       video.colourNote == "Color tags missing — assumed Rec.709 with the "
                                           "sRGB transfer. Change in Assets → Input Color",
                   path.filename().string() + ": renders in Auto with the Rec.709 note");
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
