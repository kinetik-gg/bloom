#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/render/image_types.hpp>
#include <bloom/ui/viewer_editor.hpp>

#include <QApplication>
#include <QRectF>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string_view>

namespace {

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << "Failure: " << message << '\n';
    }

    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

[[nodiscard]] bool near(const qreal lhs, const qreal rhs) noexcept {
    return std::abs(lhs - rhs) <= 0.0001;
}

bloom::render::ImageExtent extent(const std::uint64_t width, const std::uint64_t height) {
    auto result = bloom::render::ImageExtent::create(width, height);
    if (!result) {
        std::abort();
    }
    return *result.value();
}

void testSquarePixelFitting(Expectations& expectations) {
    const QRectF available(0.0, 0.0, 1000.0, 1000.0);
    const QRectF wide = bloom::ui::fitDisplayRect(available, extent(1920, 1080),
                                                  bloom::core::PixelAspectRatio::square());
    expectations.expect(near(wide.width(), 1000.0) && near(wide.height(), 562.5) &&
                            near(wide.left(), 0.0) && near(wide.top(), 218.75),
                        "wide square-pixel content is centered and width-limited");

    const QRectF tall = bloom::ui::fitDisplayRect(available, extent(100, 200),
                                                  bloom::core::PixelAspectRatio::square());
    expectations.expect(near(tall.width(), 500.0) && near(tall.height(), 1000.0) &&
                            near(tall.left(), 250.0) && near(tall.top(), 0.0),
                        "tall square-pixel content is centered and height-limited");
}

void testPixelAspectFitting(Expectations& expectations) {
    const auto widePixels = bloom::core::PixelAspectRatio::create(2, 1);
    expectations.expect(widePixels.has_value(), "non-square pixel aspect is valid");
    if (!widePixels.has_value()) {
        return;
    }

    const QRectF fitted =
        bloom::ui::fitDisplayRect(QRectF(10.0, 20.0, 300.0, 300.0), extent(100, 100), *widePixels);
    expectations.expect(near(fitted.width(), 300.0) && near(fitted.height(), 150.0) &&
                            near(fitted.left(), 10.0) && near(fitted.top(), 95.0),
                        "pixel width-to-height ratio changes display aspect before fitting");
}

void testDegenerateAvailableRect(Expectations& expectations) {
    const QRectF fitted =
        bloom::ui::fitDisplayRect(QRectF(), extent(1, 1), bloom::core::PixelAspectRatio::square());
    expectations.expect(fitted.isEmpty(), "empty available geometry produces no display rectangle");
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    Expectations expectations;
    testSquarePixelFitting(expectations);
    testPixelAspectFitting(expectations);
    testDegenerateAvailableRect(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
