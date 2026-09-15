#include "node_editor_items.hpp"
#include "window_fixture.hpp"
#include <QDir>
#include <QFile>
#include <QPainter>
#include <QTemporaryDir>
#include <bloom/commands/operations.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/node_editor.hpp>
#include <cstdint>
#include <iostream>

namespace {
using namespace bloom;
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
void writeFixtures(const QTemporaryDir& directory) {
    for (int frame = 0; frame <= 24; ++frame) {
        QImage image(640, 480, QImage::Format_RGBA8888);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        const auto color = frame == 0 ? QColor(245, 155, 78) : QColor(65, 184, 209);
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        if (frame == 0) {
            painter.drawRoundedRect(QRectF(40, 40, 560, 400), 80, 80);
            painter.setBrush(QColor(45, 54, 70));
            painter.drawEllipse(QRectF(140, 60, 360, 360));
            painter.setBrush(QColor(255, 214, 158));
            painter.drawEllipse(QRectF(240, 160, 160, 160));
        } else {
            const int radius = 95 + frame * 3;
            painter.drawEllipse(QPoint(320, 240), radius, radius);
            painter.setBrush(QColor(13, 73, 99));
            painter.drawEllipse(QPoint(320, 240), radius - 40, radius - 40);
            painter.setBrush(QColor(180, 243, 236));
            painter.drawEllipse(QPoint(260 + frame * 5, 195), 30, 30);
        }
        painter.end();
        const auto name = frame == 0 ? QStringLiteral("Orbit.png")
                                     : QStringLiteral("Pulse.%1.png").arg(frame, 4, 10, QChar('0'));
        require(image.save(directory.filePath(name)), "generated capture PNG");
    }
    QByteArray wave;
    const auto appendLe16 = [&wave](const std::uint16_t value) {
        wave.append(static_cast<char>(value & 0xffU));
        wave.append(static_cast<char>((value >> 8U) & 0xffU));
    };
    const auto appendLe32 = [&wave](const std::uint32_t value) {
        wave.append(static_cast<char>(value & 0xffU));
        wave.append(static_cast<char>((value >> 8U) & 0xffU));
        wave.append(static_cast<char>((value >> 16U) & 0xffU));
        wave.append(static_cast<char>((value >> 24U) & 0xffU));
    };
    const std::uint32_t sampleCount = 48'000;
    const std::uint32_t dataBytes = sampleCount * static_cast<std::uint32_t>(sizeof(std::int16_t));
    wave.append("RIFF", 4);
    appendLe32(36U + dataBytes);
    wave.append("WAVEfmt ", 8);
    appendLe32(16);
    appendLe16(1);
    appendLe16(1);
    appendLe32(48'000);
    appendLe32(48'000U * static_cast<std::uint32_t>(sizeof(std::int16_t)));
    appendLe16(static_cast<std::uint16_t>(sizeof(std::int16_t)));
    appendLe16(16);
    wave.append("data", 4);
    appendLe32(dataBytes);
    for (std::uint32_t frame = 0; frame < sampleCount; ++frame) {
        const auto phase = frame % 192U;
        const auto sample = static_cast<std::int16_t>(
            phase < 96U ? static_cast<std::int32_t>(phase) * 300
                        : (static_cast<std::int32_t>(192U - phase) * 300));
        appendLe16(static_cast<std::uint16_t>(sample));
    }
    QFile audio(directory.filePath("Tone.wav"));
    require(audio.open(QIODevice::WriteOnly) && audio.write(wave) == wave.size(),
            "generated capture WAV");
}
void author(ui::test::WindowFixture& fixture, const QTemporaryDir& directory) {
    commands::Transaction setup("Media composition", fixture.session.snapshot().revision());
    setup.emplace<commands::SetProjectName>("Image Sources");
    setup.emplace<commands::SetCompositionName>(fixture.session.compositionId(), "Orbit + Pulse");
    setup.emplace<commands::SetCompositionDuration>(fixture.session.compositionId(),
                                                    core::RationalTime::fromInteger(1));
    require(fixture.session.executeTransaction(std::move(setup)).succeeded(), "composition setup");
    fixture.assets->importFiles({directory.filePath("Orbit.png"), directory.filePath("Tone.wav")});
    QElapsedTimer timer;
    timer.start();
    while (fixture.assets->busy() && timer.elapsed() < 15000)
        QTest::qWait(10);
    require(fixture.session.snapshot().project().assets().size() == 2, "PNG and sequence import");
    const auto imported = fixture.session.snapshot();
    const auto records = imported.project().assets();
    for (const auto& asset : records) {
        commands::Transaction add(asset.kind == document::AssetKind::Audio ? "Place audio"
                                                                           : "Place image",
                                  fixture.session.snapshot().revision());
        if (asset.kind == document::AssetKind::Audio)
            add.emplace<commands::AddAudioLayer>(fixture.session.compositionId(), asset.id);
        else
            add.emplace<commands::AddImageLayer>(fixture.session.compositionId(), asset.id);
        const auto result = fixture.session.executeTransaction(std::move(add));
        const auto layer = result.outputId<document::LayerId>("layer");
        if (!result.succeeded() || !layer)
            throw std::runtime_error("wired media Layer");
        if (asset.kind != document::AssetKind::Audio) {
            fixture.session.selectLayer(*layer);
            require(fixture.session.setSelectedPosition(580.0, 540.0), "place image media");
            require(fixture.session.setSelectedScale(1.25, 1.25), "scale image content");
        }
    }
    const auto frame12 = core::RationalTime::create(12, 24);
    require(frame12 && fixture.session.setCurrentTime(*frame12), "frame 12");
}
int run(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setOrganizationName("BloomMediaTests");
    app.setApplicationName("MediaCapture");
    ui::kit::installKinetikTheme(app);
    QTemporaryDir directory;
    require(directory.isValid(), "capture fixtures directory");
    writeFixtures(directory);
    ui::test::WindowFixture fixture([&](auto& target) { author(target, directory); });
    document::NodeId audioNode;
    for (const auto& node : fixture.session.composition()->graph().nodes()) {
        if (node.typeId != document::kAudioSourceNodeType)
            continue;
        audioNode = node.id;
    }
    require(audioNode.isValid(), "audio source selected");
    fixture.session.selectNode(audioNode);
    for (auto* editor : fixture.window->findChildren<ui::NodeGraphEditor*>())
        editor->graphView()->frameGraph();
    QTest::qWait(200);
    fixture.clearInteraction();
    require(fixture.window->devicePixelRatioF() == 1.0, "capture requires DPR 1");
    QElapsedTimer waveformWait;
    waveformWait.start();
    document::AssetId audioAsset;
    for (const auto& binding :
         fixture.session.composition()->graph().findNode(audioNode)->parameters)
        if (binding.role == "asset")
            if (const auto value = fixture.session.constantStringValue(binding.parameterId))
                audioAsset = document::AssetId::fromRaw(value->toULongLong());
    while (fixture.assets->waveform(audioAsset) == nullptr && waveformWait.elapsed() < 15000)
        QTest::qWait(10);
    require(fixture.assets->waveform(audioAsset) != nullptr, "audio waveform ready");
    bool captured = false;
    for (auto* editor : fixture.window->findChildren<ui::NodeGraphEditor*>())
        for (auto* item : editor->graphView()->scene()->items())
            if (auto* card = dynamic_cast<ui::node_editor::NodeItem*>(item);
                card && card->id() == audioNode) {
                const auto rect = card->sceneBoundingRect();
                QImage image(rect.size().toSize(), QImage::Format_ARGB32_Premultiplied);
                image.fill(ui::kit::color(ui::kit::Color::Canvas));
                QPainter painter(&image);
                editor->graphView()->scene()->render(&painter, QRectF(image.rect()), rect);
                painter.end();
                captured =
                    image.save(QStringLiteral(BLOOM_GRAMMAR_ARTIFACT_DIR "/audio2-card.png"));
            }
    require(captured, "write Audio source card capture");
    const auto destination = QStringLiteral(BLOOM_GRAMMAR_ARTIFACT_DIR "/audio2-window-dpr1.png");
    require(fixture.window->grab().save(destination), "write full window capture");
    std::cout << destination.toStdString() << " — frame 12, WAV + image layer\n";
    return 0;
}
} // namespace
int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
