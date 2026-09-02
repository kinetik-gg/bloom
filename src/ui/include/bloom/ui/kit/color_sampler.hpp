#pragma once

#include <bloom/ui/kit/color.hpp>

#include <QImage>
#include <QObject>
#include <QPoint>

#include <optional>

class QWidget;

namespace bloom::ui::kit {

// Task U6 (issue #121), decision 5: the eyedropper. Honestly scoped to Bloom's own windows --
// src/platform has no cursor/screen-capture surface (see staged_artifact.* -- that is the whole of
// src/platform today), so a system-wide sampler would need a platform/portal seam that does not
// exist yet. Faking one by e.g. grabbing whatever QScreen::grabWindow() can reach would silently
// promise the artist more than Bloom can actually see (another application's window, a
// screen-recording-permission dialog on macOS, ...); this samples only what QApplication already
// knows is a Bloom widget, so the scope is enforced by construction, not by a check someone could
// forget. Disclosed to the artist at the call site via kSamplerScopeTooltip.
inline constexpr const char* kSamplerScopeTooltip = "Samples inside Bloom";

// The pure pixel read: `point` in `image`'s own (device) pixel coordinates. Returns nullopt for a
// point outside the image, never a default-constructed KColor that could be mistaken for black.
[[nodiscard]] std::optional<KColor> sampleImagePixel(const QImage& image, const QPoint& point);

// Grabs `widget` and samples at `localPoint` (in the widget's logical pixel coordinates, scaled to
// the grabbed pixmap's own device pixel ratio internally) -- the "widget grab" half of decision 5's
// "a provided QImage/widget grab under the cursor".
[[nodiscard]] std::optional<KColor> sampleWidgetPixel(QWidget& widget, const QPoint& localPoint);

// The interactive affordance: while active, tracks the application's own mouse events (installed
// as an application-wide event filter, but QApplication::widgetAt() -- which every sample resolves
// through -- can only ever resolve to a widget this process owns) and reports the color under the
// cursor as it moves, committing on a left click and aborting on Escape or a right click.
class KColorSampler final : public QObject {
    Q_OBJECT

  public:
    explicit KColorSampler(QObject* parent = nullptr);
    ~KColorSampler() override;

    void begin();
    void cancel();
    [[nodiscard]] bool isActive() const noexcept;

    // The scoped sample at a global screen position: resolves to whichever of the application's
    // own widgets is under `globalPos` (nullopt if none), grabs it, and reads the pixel. Public
    // and independent of begin()/cancel() so a test can drive it directly against a widget it
    // constructed and shows, without simulating global cursor motion.
    [[nodiscard]] std::optional<KColor> sampleAt(const QPoint& globalPos) const;

  Q_SIGNALS:
    void colorHovered(const KColor& color);
    void colorPicked(const KColor& color);
    void sampleCancelled();

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void end();

    bool active_ = false;
};

} // namespace bloom::ui::kit
