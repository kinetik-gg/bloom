#include "window_fixture.hpp"
#include <QDir>
#include <QImage>
#include <bloom/ui/kit/theme.hpp>
#include <iostream>

namespace {
int run(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setOrganizationName("BloomGrammarTests");
    app.setApplicationName("WindowGoldens");
    bloom::ui::kit::installKinetikTheme(app);
    bloom::ui::test::WindowFixture fixture;
    const auto ratio = fixture.window->devicePixelRatioF();
    if (ratio != 1.0 && ratio != 1.5) {
        std::cerr << "Goldens require DPR 1 or 1.5\n";
        return 1;
    }
    if (app.arguments().contains("--layout-report")) {
        for (auto* widget : fixture.window->findChildren<QWidget*>()) {
            if (widget->property("collapseThreshold").isValid()) {
                std::cout << widget->objectName().toStdString() << " width=" << widget->width()
                          << " threshold=" << widget->property("collapseThreshold").toInt()
                          << " collapsed=" << widget->property("collapsed").toBool() << '\n';
            }
        }
    }
    fixture.clearInteraction();
    const auto actual = fixture.window->grab().toImage().convertToFormat(QImage::Format_RGB32);
    const QString suffix = ratio == 1.0 ? "dpr1" : "dpr15";
    const QString referencePath =
        QStringLiteral(BLOOM_GRAMMAR_GOLDEN_DIR "/window-") + suffix + ".png";
    const QString capturePath =
        QStringLiteral(BLOOM_GRAMMAR_ARTIFACT_DIR "/grammar1-window-") + suffix + ".png";
    if (!actual.save(capturePath))
        return 1;
    if (app.arguments().contains("--update-goldens")) {
        if (!actual.save(referencePath))
            return 1;
        std::cout << "Updated " << referencePath.toStdString() << '\n';
        return 0;
    }
    const QImage reference = QImage(referencePath).convertToFormat(QImage::Format_RGB32);
    if (reference.isNull() || reference.size() != actual.size()) {
        std::cerr << "Missing golden or changed window dimensions\n";
        return 1;
    }
    std::uint64_t total = 0, changed = 0;
    for (int y = 0; y < actual.height(); ++y)
        for (int x = 0; x < actual.width(); ++x) {
            const auto a = actual.pixel(x, y), b = reference.pixel(x, y);
            const int delta =
                std::max({std::abs(qRed(a) - qRed(b)), std::abs(qGreen(a) - qGreen(b)),
                          std::abs(qBlue(a) - qBlue(b))});
            total += static_cast<std::uint64_t>(delta);
            if (delta > 24)
                ++changed;
        }
    const double pixels = static_cast<double>(actual.width()) * actual.height();
    const double mean = static_cast<double>(total) / pixels;
    const double fraction = static_cast<double>(changed) / pixels;
    std::cout << "Golden " << suffix.toStdString() << ": mean channel error " << mean
              << ", changed fraction " << fraction << '\n';
    return mean <= 1.2 && fraction <= 0.01 ? 0 : 1;
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
