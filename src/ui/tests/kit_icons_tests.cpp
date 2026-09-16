#include <bloom/ui/kit/icons.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <QApplication>
#include <QFile>
#include <QIcon>
#include <QImage>
#include <QPixmap>
#include <QSet>
#include <QSizeF>
#include <QString>

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

using namespace bloom::ui;

[[nodiscard]] bool hasVisibleInk(const QPixmap& pixmap) {
    const QImage image = pixmap.toImage();
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(image.pixel(x, y)) > 0) {
                return true;
            }
        }
    }
    return false;
}

void testEveryIconIdRendersAtEverySizeInEveryWeight(Expectations& expectations) {
    const auto sizes = {kit::Size::IconSmall, kit::Size::IconMedium, kit::Size::IconLarge};
    // Task VIEW-1 completed the Bold subset, so all three vendored weights are total over the
    // whole vocabulary -- IconRole::Chrome asks for Bold on every chrome glyph, and a missing file
    // renders as a silent blank rather than an error.
    const auto weights = {kit::IconWeight::Regular, kit::IconWeight::Fill, kit::IconWeight::Bold};
    int rendered = 0;
    for (const kit::IconId id : kit::iconIds()) {
        for (const kit::IconWeight weight : weights) {
            expectations.expect(QFile::exists(kit::iconResourcePath(id, weight)),
                                "the vendored asset backing this icon is in the resource pack: " +
                                    kit::iconResourcePath(id, weight).toStdString());
            for (const kit::Size size : sizes) {
                const QPixmap pixmap =
                    kit::iconPixmap(id, size, kit::Color::Muted, kit::State::Normal, weight, 1.0);
                expectations.expect(!pixmap.isNull(),
                                    "icon renders to a non-null pixmap: " +
                                        kit::iconResourcePath(id, weight).toStdString());
                expectations.expect(pixmap.deviceIndependentSize() ==
                                        QSizeF(kit::px(size), kit::px(size)),
                                    "the pixmap is its requested design-pixel box");
                expectations.expect(hasVisibleInk(pixmap),
                                    "the rendered icon actually carries ink: " +
                                        kit::iconResourcePath(id, weight).toStdString());
                ++rendered;
            }
        }
    }
    expectations.expect(rendered == static_cast<int>(kit::iconIds().size()) * 9,
                        "every id was rendered at all three sizes in all three weights");
    // The four media footer/kind glyphs extend the curated vocabulary by four.
    expectations.expect(kit::iconIds().size() >= 30 && kit::iconIds().size() <= 68,
                        "the curated vocabulary stays a curated vocabulary");
}

void testIconIdsAreUniqueAndTotal(Expectations& expectations) {
    QSet<int> seen;
    for (const kit::IconId id : kit::iconIds()) {
        seen.insert(static_cast<int>(id));
    }
    expectations.expect(seen.size() == static_cast<int>(kit::iconIds().size()),
                        "no icon id is listed twice");
    // Every declared enumerator has a backing asset: walking the numeric range catches an id added
    // to the enum but never bound to a file. Images is the last declared enumerator.
    for (int value = 0; value <= static_cast<int>(kit::IconId::Images); ++value) {
        expectations.expect(seen.contains(value),
                            "icon id " + std::to_string(value) + " has a vendored asset");
    }
}

void testTintDiffersByRoleAndByState(Expectations& expectations) {
    const auto pixel = [](const QPixmap& pixmap) {
        const QImage image = pixmap.toImage();
        // The densest pixel of the glyph, whatever the shape: enough to read the tint back.
        QRgb best = 0;
        int bestAlpha = -1;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                const QRgb value = image.pixel(x, y);
                if (qAlpha(value) > bestAlpha) {
                    bestAlpha = qAlpha(value);
                    best = value;
                }
            }
        }
        return best;
    };

    const QPixmap muted =
        kit::iconPixmap(kit::IconId::Close, kit::Size::IconLarge, kit::Color::Muted,
                        kit::State::Normal, kit::IconWeight::Regular, 1.0);
    const QPixmap accent =
        kit::iconPixmap(kit::IconId::Close, kit::Size::IconLarge, kit::Color::Accent,
                        kit::State::Normal, kit::IconWeight::Regular, 1.0);
    const QPixmap hovered =
        kit::iconPixmap(kit::IconId::Close, kit::Size::IconLarge, kit::Color::Muted,
                        kit::State::Hover, kit::IconWeight::Regular, 1.0);
    const QPixmap disabled =
        kit::iconPixmap(kit::IconId::Close, kit::Size::IconLarge, kit::Color::Muted,
                        kit::State::Disabled, kit::IconWeight::Regular, 1.0);

    expectations.expect(pixel(muted) != pixel(accent), "the tint follows the palette role");
    expectations.expect(pixel(muted) != pixel(hovered), "the tint follows the interaction state");
    expectations.expect(qAlpha(pixel(disabled)) < qAlpha(pixel(muted)),
                        "a disabled icon is the same glyph at reduced ink");

    expectations.expect(kit::iconTint(kit::Color::Muted, kit::State::Hover) ==
                            kit::color(kit::Color::Foreground),
                        "hover ink is full strength");
    expectations.expect(kit::iconTint(kit::Color::Muted, kit::State::Normal) ==
                            kit::color(kit::Color::Muted),
                        "the resting tint is the requested role");
}

void testTheCacheReturnsTheSamePixmapForTheSameIdentity(Expectations& expectations) {
    kit::clearIconCache();
    expectations.expect(kit::iconCacheEntryCount() == 0, "clearing the cache empties it");

    const QPixmap first =
        kit::iconPixmap(kit::IconId::Play, kit::Size::IconMedium, kit::Color::Foreground,
                        kit::State::Normal, kit::IconWeight::Regular, 1.0);
    expectations.expect(kit::iconCacheEntryCount() == 1, "a render populates exactly one entry");
    const QPixmap second =
        kit::iconPixmap(kit::IconId::Play, kit::Size::IconMedium, kit::Color::Foreground,
                        kit::State::Normal, kit::IconWeight::Regular, 1.0);
    expectations.expect(kit::iconCacheEntryCount() == 1, "a repeat render does not add an entry");
    expectations.expect(first.cacheKey() == second.cacheKey(),
                        "the same identity yields the identical pixmap, not an equal copy");

    // Every component of the identity the ADR names must miss the cache on its own.
    const struct {
        qreal ratio;
        const char* what;
        kit::Size size;
        kit::IconId id;
        kit::Color role;
        kit::State state;
        kit::IconWeight weight;
    } variations[] = {
        {1.0, "identity", kit::Size::IconMedium, kit::IconId::Pause, kit::Color::Foreground,
         kit::State::Normal, kit::IconWeight::Regular},
        {1.0, "size", kit::Size::IconLarge, kit::IconId::Play, kit::Color::Foreground,
         kit::State::Normal, kit::IconWeight::Regular},
        {1.0, "palette color", kit::Size::IconMedium, kit::IconId::Play, kit::Color::Accent,
         kit::State::Normal, kit::IconWeight::Regular},
        {1.0, "state", kit::Size::IconMedium, kit::IconId::Play, kit::Color::Foreground,
         kit::State::Disabled, kit::IconWeight::Regular},
        {1.0, "weight", kit::Size::IconMedium, kit::IconId::Play, kit::Color::Foreground,
         kit::State::Normal, kit::IconWeight::Fill},
        {1.0, "a third weight", kit::Size::IconMedium, kit::IconId::Play, kit::Color::Foreground,
         kit::State::Normal, kit::IconWeight::Bold},
        {2.0, "device pixel ratio", kit::Size::IconMedium, kit::IconId::Play,
         kit::Color::Foreground, kit::State::Normal, kit::IconWeight::Regular},
    };
    std::size_t expected = 1;
    for (const auto& variation : variations) {
        (void)kit::iconPixmap(variation.id, variation.size, variation.role, variation.state,
                              variation.weight, variation.ratio);
        ++expected;
        expectations.expect(kit::iconCacheEntryCount() == expected,
                            std::string{"the cache key accounts for "} + variation.what);
    }
}

void testPixmapsAreDevicePixelRatioAware(Expectations& expectations) {
    const QPixmap oneX =
        kit::iconPixmap(kit::IconId::Settings, kit::Size::IconMedium, kit::Color::Muted,
                        kit::State::Normal, kit::IconWeight::Regular, 1.0);
    const QPixmap twoX =
        kit::iconPixmap(kit::IconId::Settings, kit::Size::IconMedium, kit::Color::Muted,
                        kit::State::Normal, kit::IconWeight::Regular, 2.0);
    const int box = kit::px(kit::Size::IconMedium);

    expectations.expect(oneX.toImage().width() == box, "at 1x the icon is box physical pixels");
    expectations.expect(twoX.toImage().width() == box * 2,
                        "at 2x the icon is twice as many physical pixels");
    expectations.expect(std::abs(oneX.devicePixelRatio() - 1.0) < 1e-9 &&
                            std::abs(twoX.devicePixelRatio() - 2.0) < 1e-9,
                        "each pixmap carries its own device pixel ratio");
    expectations.expect(oneX.deviceIndependentSize() == QSizeF(box, box) &&
                            twoX.deviceIndependentSize() == QSizeF(box, box),
                        "both are the same size in design pixels: only the pixel count changes");
}

void testQIconCarriesTheStateMachine(Expectations& expectations) {
    const QIcon value = kit::icon(kit::IconId::Visible, kit::Size::IconMedium, kit::Color::Muted);
    expectations.expect(!value.isNull(), "the QIcon is populated");
    for (const auto mode : {QIcon::Normal, QIcon::Active, QIcon::Selected, QIcon::Disabled}) {
        const QPixmap pixmap =
            value.pixmap(QSize(kit::px(kit::Size::IconMedium), kit::px(kit::Size::IconMedium)),
                         mode, QIcon::Off);
        expectations.expect(!pixmap.isNull() && hasVisibleInk(pixmap),
                            "the QIcon renders in every interaction mode");
    }
}

// Task VIEW-1: the two icon ROLES are the single definition of "which weight, which box" for every
// icon in the application. A call site names a role; the role -- not the call site -- resolves the
// pair, so changing one of the two rows below is an app-wide change by construction.
void testIconRolesResolveOneWeightAndOneBox(Expectations& expectations) {
    expectations.expect(kit::iconWeight(kit::IconRole::Chrome) == kit::IconWeight::Bold &&
                            kit::iconSize(kit::IconRole::Chrome) == kit::Size::IconMedium,
                        "Chrome (headers, menus, toggles) is Phosphor Bold at 16 px");
    expectations.expect(kit::iconWeight(kit::IconRole::Control) == kit::IconWeight::Fill &&
                            kit::iconSize(kit::IconRole::Control) == kit::Size::IconLarge,
                        "Control (transport, viewer footer) is Phosphor Fill at 20 px");
    expectations.expect(kit::px(kit::iconSize(kit::IconRole::Chrome)) == 16 &&
                            kit::px(kit::iconSize(kit::IconRole::Control)) == 20,
                        "both role boxes are pinned in design pixels, not merely named");

    // The role spellings are the explicit pair, not a second rendering path: same cache identity,
    // therefore the identical pixmap rather than an equal copy.
    for (const auto role : {kit::IconRole::Chrome, kit::IconRole::Control}) {
        const QPixmap viaRole = kit::iconPixmap(kit::IconId::Play, role, kit::Color::Foreground,
                                                kit::State::Normal, 1.0);
        const QPixmap viaPair =
            kit::iconPixmap(kit::IconId::Play, kit::iconSize(role), kit::Color::Foreground,
                            kit::State::Normal, kit::iconWeight(role), 1.0);
        expectations.expect(!viaRole.isNull() && viaRole.cacheKey() == viaPair.cacheKey(),
                            "the role overload resolves to exactly the weight/size pair it names");
        expectations.expect(viaRole.deviceIndependentSize() ==
                                QSizeF(kit::px(kit::iconSize(role)), kit::px(kit::iconSize(role))),
                            "a role-rendered icon is its role's own design-pixel box");
        expectations.expect(hasVisibleInk(viaRole), "a role-rendered icon carries ink");
    }

    // Every id has a Bold asset, not just the five the timeline's toggles used to need.
    for (const kit::IconId id : kit::iconIds()) {
        expectations.expect(
            QFile::exists(kit::iconResourcePath(id, kit::IconWeight::Bold)),
            "every id has a vendored Bold asset, because Chrome asks for Bold everywhere: " +
                kit::iconResourcePath(id, kit::IconWeight::Bold).toStdString());
        expectations.expect(kit::iconResourcePath(id, kit::IconWeight::Bold) !=
                                kit::iconResourcePath(id, kit::IconWeight::Regular),
                            "Bold resolves to its own file, never a silent fallback to Regular");
    }
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    const QApplication application(argc, argv);
    Expectations expectations;
    testEveryIconIdRendersAtEverySizeInEveryWeight(expectations);
    testIconRolesResolveOneWeightAndOneBox(expectations);
    testIconIdsAreUniqueAndTotal(expectations);
    testTintDiffersByRoleAndByState(expectations);
    testTheCacheReturnsTheSamePixmapForTheSameIdentity(expectations);
    testPixmapsAreDevicePixelRatioAware(expectations);
    testQIconCarriesTheStateMachine(expectations);
    for (const qreal dpr : {1.0, 1.25, 1.5, 2.0}) {
        for (const auto id : kit::iconIds()) {
            for (const auto weight : {kit::IconWeight::Regular, kit::IconWeight::Fill}) {
                const auto direct = kit::iconPixmap(id, kit::Size::IconChrome, kit::Color::Muted,
                                                    kit::State::Normal, weight, dpr);
                const auto engine = kit::icon(id, kit::Size::IconChrome, kit::Color::Muted, weight)
                                        .pixmap(QSize(16, 16), dpr);
                expectations.expect(direct.size() == QSize(qRound(16 * dpr), qRound(16 * dpr)),
                                    "SVG rasterization has integer device-pixel dimensions");
                expectations.expect(engine.size() == direct.size() &&
                                        engine.devicePixelRatio() == dpr,
                                    "QIcon engine rerasterizes at the requested DPR");
                expectations.expect(engine.toImage() == direct.toImage(),
                                    "QIcon never scales a cached lower-resolution glyph");
            }
        }
    }

    return expectations.failures() == 0 ? 0 : 1;
}
