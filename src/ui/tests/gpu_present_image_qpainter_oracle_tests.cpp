// Independent Qt/QPainter oracle for the present-image shader's filtering and channel rules. It
// does not mirror the shader algebra: it asks QPainter itself how a straight
// QImage::Format_RGBA8888 is resampled under SmoothPixmapTransform, which is the behavior the
// shader must reproduce. The render-side CPU tests then assert the same rule. Qt-only; not linked
// into any render target.

#include <QGuiApplication>
#include <QImage>
#include <QPainter>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <source_location>
#include <string>

namespace {

class Expectations final {
  public:
    void expect(const bool condition, const std::string& message,
                const std::source_location location = std::source_location::current()) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }
    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

// QPainter SmoothPixmapTransform on a straight RGBA8888 image must premultiply before filtering, so
// the opaque-red / transparent-blue edge stays red with no blue halo. If Qt filtered straight RGB
// and alpha independently, the boundary would drift toward purple (blue > 0.3).
void premultipliedFilterRule(Expectations& expectations) {
    QImage source(2, 1, QImage::Format_RGBA8888);
    source.setPixelColor(0, 0, QColor(255, 0, 0, 255));
    source.setPixelColor(1, 0, QColor(0, 0, 255, 0));
    QImage target(8, 1, QImage::Format_RGBA8888);
    target.fill(QColor(0, 0, 0, 0));
    {
        QPainter painter(&target);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.drawImage(QRectF(0.0, 0.0, 8.0, 1.0), source, QRectF(0.0, 0.0, 2.0, 1.0));
    }
    float maxBlue = 0.0F;
    float maxRed = 0.0F;
    for (int x = 0; x < 8; ++x) {
        const QColor c = target.pixelColor(x, 0);
        maxBlue = std::max(maxBlue, static_cast<float>(c.blueF()));
        maxRed = std::max(maxRed, static_cast<float>(c.redF()));
    }
    expectations.expect(maxBlue < 0.05F,
                        "QPainter premultiplies before filtering: no blue halo at the edge");
    expectations.expect(maxRed > 0.9F, "the opaque red texel survives the filter");
    std::cout << "QPainter smooth filter: maxRed=" << maxRed << " maxBlue=" << maxBlue << '\n';
}

// The red/blue sentinels pin the RGBA channel identity through the paint path (the GPU attachment
// format owns packing; there is no manual swizzle in the shader).
void channelSentinels(Expectations& expectations) {
    QImage source(2, 1, QImage::Format_RGBA8888);
    source.setPixelColor(0, 0, QColor(255, 0, 0, 255));
    source.setPixelColor(1, 0, QColor(0, 0, 255, 255));
    QImage target(2, 1, QImage::Format_RGBA8888);
    {
        QPainter painter(&target);
        painter.drawImage(0, 0, source);
    }
    const QColor left = target.pixelColor(0, 0);
    const QColor right = target.pixelColor(1, 0);
    expectations.expect(left.red() == 255 && left.blue() == 0,
                        "the red sentinel stays in the red channel");
    expectations.expect(right.blue() == 255 && right.red() == 0,
                        "the blue sentinel stays in the blue channel");
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication application(argc, argv);
    Expectations expectations;
    premultipliedFilterRule(expectations);
    channelSentinels(expectations);
    std::cout << (expectations.failures() == 0 ? "PASS" : "FAIL") << ": QPainter oracle\n";
    return expectations.failures() == 0 ? 0 : 1;
}
