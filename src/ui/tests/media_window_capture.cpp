#include "window_fixture.hpp"
#include <QDir>
#include <QPainter>
#include <QTemporaryDir>
#include <bloom/commands/operations.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/node_editor.hpp>
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
}
void author(ui::test::WindowFixture& fixture, const QTemporaryDir& directory) {
    commands::Transaction setup("Media composition", fixture.session.snapshot().revision());
    setup.emplace<commands::SetProjectName>("Image Sources");
    setup.emplace<commands::SetCompositionName>(fixture.session.compositionId(), "Orbit + Pulse");
    setup.emplace<commands::SetCompositionDuration>(fixture.session.compositionId(),
                                                    core::RationalTime::fromInteger(1));
    require(fixture.session.executeTransaction(std::move(setup)).succeeded(), "composition setup");
    require(fixture.session.addSolidLayer("Backdrop", {0.018, 0.03, 0.05, 1.0}), "solid backdrop");
    fixture.assets->importFiles(
        {directory.filePath("Orbit.png"), directory.filePath("Pulse.0001.png")});
    QElapsedTimer timer;
    timer.start();
    while (fixture.assets->busy() && timer.elapsed() < 15000)
        QTest::qWait(10);
    require(fixture.session.snapshot().project().assets().size() == 2, "PNG and sequence import");
    const auto imported = fixture.session.snapshot();
    const auto records = imported.project().assets();
    for (const auto& asset : records) {
        commands::Transaction add("Place image", fixture.session.snapshot().revision());
        add.emplace<commands::AddImageLayer>(fixture.session.compositionId(), asset.id);
        const auto result = fixture.session.executeTransaction(std::move(add));
        const auto layer = result.outputId<document::LayerId>("layer");
        if (!result.succeeded() || !layer)
            throw std::runtime_error("wired Image Layer");
        fixture.session.selectLayer(*layer);
        require(fixture.session.setSelectedPosition(
                    asset.kind == document::AssetKind::Image ? 580.0 : 1310.0, 540.0),
                "place media beside each other");
        require(fixture.session.setSelectedScale(1.25, 1.25), "scale source content");
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
    document::NodeId sequenceNode;
    for (const auto& node : fixture.session.composition()->graph().nodes()) {
        if (node.typeId != "bloom.image-source")
            continue;
        for (const auto& binding : node.parameters)
            if (binding.role == "asset") {
                const auto value = fixture.session.constantStringValue(binding.parameterId);
                const auto* asset = value ? fixture.session.snapshot().project().findAsset(
                                                document::AssetId::fromRaw(value->toULongLong()))
                                          : nullptr;
                if (asset && asset->kind == document::AssetKind::Sequence)
                    sequenceNode = node.id;
            }
    }
    require(sequenceNode.isValid(), "sequence source selected");
    fixture.session.selectNode(sequenceNode);
    for (auto* editor : fixture.window->findChildren<ui::NodeGraphEditor*>())
        editor->graphView()->frameGraph();
    QTest::qWait(200);
    fixture.clearInteraction();
    require(fixture.session.snapshot().project().assets().back().manifest.members.size() == 24,
            "capture contains all 24 members");
    require(fixture.window->devicePixelRatioF() == 1.0, "capture requires DPR 1");
    const auto destination = QStringLiteral(BLOOM_GRAMMAR_ARTIFACT_DIR "/media1-window-dpr1.png");
    require(fixture.window->grab().save(destination), "write full window capture");
    std::cout << destination.toStdString() << " — frame 12, PNG + 24-frame sequence over solid\n";
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
