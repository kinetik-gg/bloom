// ADR 0024 Timeline bar gestures: move, Alt slip, trim and Alt stretch, as exact frame math.

#include "../timeline_layer_timing_gesture.hpp"

#include <iostream>

using namespace bloom;
namespace {
int failures = 0;
void expect(const bool value, const char* message) {
    if (!value) {
        ++failures;
        std::cerr << message << '\n';
    }
}
core::RationalTime f(const std::int64_t frame) { return *core::RationalTime::create(frame, 24); }

ui::LayerTimingGestureInput base(const int handle, const bool alt, const std::int64_t frames) {
    return {.start = f(10),
            .in = f(10),
            .out = f(34),
            .speed = {},
            .rate = *document::FrameRate::create(24, 1),
            .duration = f(100),
            .extentStart = f(10),
            .extentEnd = f(34),
            .handle = handle,
            .alt = alt,
            .deltaSeconds = static_cast<double>(frames) / 24.0};
}
} // namespace

int main() {
    using ui::proposeLayerTiming;
    auto moved = proposeLayerTiming(base(0, false, 5));
    expect(moved && moved->start == f(15) && moved->in == f(15) && moved->out == f(39),
           "a body drag moves start, in and out together");
    moved = proposeLayerTiming(base(0, false, 500));
    expect(moved && moved->in == f(99) && moved->out == f(123) && moved->start == f(99),
           "a move keeps one frame inside the composition and may pass its end");
    moved = proposeLayerTiming(base(0, false, -500));
    expect(moved && moved->out == f(1) && moved->in == f(-23), "a move may start before zero");
    const auto slipped = proposeLayerTiming(base(0, true, -4));
    expect(slipped && slipped->start == f(6) && slipped->in == f(10) && slipped->out == f(34),
           "Alt + body drag slips the start only");
    auto trimmed = proposeLayerTiming(base(1, false, 40));
    expect(trimmed && trimmed->out == f(74) && trimmed->start == f(10),
           "a trim inside the composition passes the content extent (after mode decides)");
    auto input = base(1, false, 200);
    trimmed = proposeLayerTiming(input);
    expect(trimmed && trimmed->out == f(100), "a trim stops at the composition end without extent");
    input.extentEnd = f(140);
    trimmed = proposeLayerTiming(input);
    expect(trimmed && trimmed->out == f(140), "a trim may pass the end as far as the extent reaches");
    input = base(-1, false, -40);
    input.extentStart = f(-20);
    trimmed = proposeLayerTiming(input);
    expect(trimmed && trimmed->in == f(-20), "a trim may pass zero as far as the extent reaches");
    const auto stretched = proposeLayerTiming(base(1, true, 24));
    expect(stretched && stretched->speed == *document::LayerSpeed::create(1, 2) &&
               stretched->start == f(10) && stretched->out == f(58),
           "Alt + out-edge drag stretches about the in edge");
    const auto compressed = proposeLayerTiming(base(-1, true, 12));
    expect(compressed && compressed->speed == *document::LayerSpeed::create(2, 1) &&
               compressed->in == f(22) && compressed->start == f(22),
           "Alt + in-edge drag stretches about the out edge, keeping its content time");
    return failures == 0 ? 0 : 1;
}
