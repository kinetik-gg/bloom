#include <bloom/ui/text_box_overlay.hpp>

#include <cmath>
#include <iostream>

namespace {
int failures = 0;

void expect(const bool value, const char* message) {
    if (!value) {
        std::cerr << message << '\n';
        ++failures;
    }
}
} // namespace

int main() {
    const QRectF box(10.0, 20.0, 100.0, 50.0);
    const auto geometry = bloom::ui::textBoxOverlayGeometry(box, 10.0);
    expect(geometry.box == box, "overlay retains the box geometry");
    expect(geometry.handles[0].contains(box.topLeft()), "corner handle is centred on the corner");
    expect(bloom::ui::hitTestTextBoxHandle(geometry, box.bottomRight()) ==
               bloom::ui::TextBoxHandle::BottomRight,
           "handle hit testing uses the corresponding handle");

    int commits = 0;
    QRectF committed;
    bloom::ui::TextBoxResizeInteraction interaction;
    interaction.begin(
        box, bloom::ui::TextBoxHandle::BottomRight, box.bottomRight(),
        QRectF(0.0, 0.0, 320.0, 200.0),
        [&](const QRectF result) {
            ++commits;
            committed = result;
        },
        true);
    interaction.update(QPointF(210.0, 145.0), true);
    interaction.release();
    expect(commits == 1, "box resize commits exactly once on release");
    expect(std::abs(committed.width() / committed.height() - 2.0) < 1e-9,
           "shift preserves the box aspect ratio");

    commits = 0;
    interaction.begin(box, bloom::ui::TextBoxHandle::TopLeft, box.topLeft(),
                      QRectF(0.0, 0.0, 320.0, 200.0), [&](const QRectF) { ++commits; });
    interaction.update(QPointF(3.0, 4.0));
    interaction.release();
    expect(commits == 1 && interaction.box().left() == 0.0 && interaction.box().top() == 0.0,
           "box resize snaps handles to nearby composition edges");
    return failures == 0 ? 0 : 1;
}
