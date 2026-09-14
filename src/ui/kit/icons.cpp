#include <bloom/ui/kit/icons.hpp>

#include <QByteArray>
#include <QFile>
#include <QGuiApplication>
#include <QHash>
#include <QIconEngine>
#include <QImage>
#include <QLatin1StringView>
#include <QPainter>
#include <QSvgRenderer>

#include <algorithm>
#include <array>
#include <cmath>

namespace bloom::ui::kit {
namespace {

struct IconAsset {
    IconId id;
    QLatin1StringView upstreamName;
};

// The one place a Bloom icon meaning is bound to an upstream Phosphor filename. Nothing else in
// the codebase may name a vendored asset.
const auto& iconAssets() {
    static const auto entries = std::to_array<IconAsset>({
        {IconId::Close, QLatin1StringView("x")},
        {IconId::Minimize, QLatin1StringView("minus")},
        {IconId::Maximize, QLatin1StringView("corners-out")},
        {IconId::Restore, QLatin1StringView("corners-in")},
        {IconId::SplitHorizontal, QLatin1StringView("square-split-horizontal")},
        {IconId::SplitVertical, QLatin1StringView("square-split-vertical")},
        {IconId::ContextMenu, QLatin1StringView("dots-three-vertical")},
        {IconId::Menu, QLatin1StringView("list")},
        {IconId::CaretDown, QLatin1StringView("caret-down")},
        {IconId::CaretUp, QLatin1StringView("caret-up")},
        {IconId::CaretRight, QLatin1StringView("caret-right")},
        {IconId::CaretLeft, QLatin1StringView("caret-left")},
        {IconId::CaretUpDown, QLatin1StringView("caret-up-down")},
        {IconId::Play, QLatin1StringView("play")},
        {IconId::Pause, QLatin1StringView("pause")},
        {IconId::StepBack, QLatin1StringView("skip-back")},
        {IconId::StepForward, QLatin1StringView("skip-forward")},
        {IconId::Loop, QLatin1StringView("repeat")},
        {IconId::Visible, QLatin1StringView("eye")},
        {IconId::Hidden, QLatin1StringView("eye-slash")},
        {IconId::AudioOn, QLatin1StringView("speaker-simple-high")},
        {IconId::AudioOff, QLatin1StringView("speaker-simple-slash")},
        {IconId::Solo, QLatin1StringView("circle")},
        {IconId::Locked, QLatin1StringView("lock-simple")},
        {IconId::Unlocked, QLatin1StringView("lock-simple-open")},
        {IconId::Add, QLatin1StringView("plus")},
        {IconId::Subtract, QLatin1StringView("minus")},
        {IconId::Zoom, QLatin1StringView("magnifying-glass")},
        {IconId::Pan, QLatin1StringView("hand")},
        {IconId::Grab, QLatin1StringView("hand-grabbing")},
        {IconId::Select, QLatin1StringView("cursor")},
        {IconId::Settings, QLatin1StringView("gear")},
        {IconId::Delete, QLatin1StringView("trash")},
        {IconId::Check, QLatin1StringView("check")},
        {IconId::Warning, QLatin1StringView("warning")},
        {IconId::Error, QLatin1StringView("warning-circle")},
        {IconId::Info, QLatin1StringView("info")},
        {IconId::Link, QLatin1StringView("link-simple")},
        {IconId::Keyframe, QLatin1StringView("diamond")},
        {IconId::Folder, QLatin1StringView("folder")},
        {IconId::Sequence, QLatin1StringView("film-strip")},
        {IconId::Clip, QLatin1StringView("film-slate")},
        {IconId::Image, QLatin1StringView("image")},
        {IconId::Audio, QLatin1StringView("music-notes")},
        {IconId::Composition, QLatin1StringView("cube")},
        {IconId::Text, QLatin1StringView("text-t")},
        {IconId::Stack, QLatin1StringView("stack")},
        {IconId::Clock, QLatin1StringView("clock")},
        {IconId::SlidersHorizontal, QLatin1StringView("sliders-horizontal")},
        {IconId::Graph, QLatin1StringView("graph")},
        {IconId::Reset, QLatin1StringView("arrow-counter-clockwise")},
        {IconId::Handle, QLatin1StringView("dots-six-vertical")},
        {IconId::Jump, QLatin1StringView("arrow-square-out")},
        {IconId::AlignLeft, QLatin1StringView("text-align-left")},
        {IconId::AlignCenter, QLatin1StringView("text-align-center")},
        {IconId::AlignRight, QLatin1StringView("text-align-right")},
        {IconId::Rectangle, QLatin1StringView("rectangle")},
        {IconId::Pen, QLatin1StringView("pen-nib")},
        {IconId::Snap, QLatin1StringView("magnet")},
    });
    return entries;
}

const auto& allIconIds() {
    static const std::vector<IconId> ids = [] {
        std::vector<IconId> values;
        values.reserve(iconAssets().size());
        for (const auto& [id, name] : iconAssets()) {
            values.push_back(id);
        }
        return values;
    }();
    return ids;
}

struct CacheKey {
    IconId id;
    IconWeight weight;
    Size size;
    QRgb tint;
    int ratioMilli;

    [[nodiscard]] bool operator==(const CacheKey& other) const noexcept = default;
};

[[nodiscard]] std::size_t qHash(const CacheKey& key, const std::size_t seed = 0) noexcept {
    // The interaction state and the palette role are not separate key components: both exist only
    // to choose a tint, and two states that resolve to the same tint render the same pixels.
    return qHashMulti(seed, static_cast<int>(key.id), static_cast<int>(key.weight),
                      static_cast<int>(key.size), key.tint, key.ratioMilli);
}

QHash<CacheKey, QPixmap>& cache() {
    static QHash<CacheKey, QPixmap> entries;
    return entries;
}

[[nodiscard]] qreal resolvedDevicePixelRatio(const qreal requested) {
    if (requested > 0.0) {
        return requested;
    }
    return QGuiApplication::instance() != nullptr ? qApp->devicePixelRatio() : 1.0;
}

[[nodiscard]] QPixmap renderIcon(const QString& resourcePath, const int sizePx, const QColor& tint,
                                 const qreal devicePixelRatio) {
    QFile source(resourcePath);
    if (!source.open(QIODevice::ReadOnly)) {
        return {};
    }
    QByteArray markup = source.readAll();
    source.close();

    // Phosphor's SVGs paint with fill="currentColor", and Qt has no API to supply a current color
    // to QSvgRenderer through 6.11. Resolving the palette role into a copy of the markup is the
    // ADR's "resolve currentColor from the applicable Qt palette role" -- the vendored bytes on
    // disk are never touched, and the result is an exactly-tinted icon rather than a mask
    // composited over a guess.
    markup.replace("currentColor", tint.name(QColor::HexRgb).toLatin1());

    QSvgRenderer renderer(markup);
    if (!renderer.isValid()) {
        return {};
    }

    const int physical =
        std::max(1, static_cast<int>(std::lround(static_cast<double>(sizePx) * devicePixelRatio)));
    QImage image(physical, physical, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    {
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        renderer.render(&painter);
    }
    if (tint.alphaF() < 1.0F) {
        // A disabled icon is the same glyph at kDisabledOpacity, not a different grey.
        QImage faded(image.size(), QImage::Format_ARGB32_Premultiplied);
        faded.fill(Qt::transparent);
        QPainter painter(&faded);
        painter.setOpacity(static_cast<double>(tint.alphaF()));
        painter.drawImage(0, 0, image);
        painter.end();
        image = faded;
    }
    image.setDevicePixelRatio(devicePixelRatio);
    return QPixmap::fromImage(image);
}

} // namespace

std::span<const IconId> iconIds() { return allIconIds(); }

QString iconResourcePath(const IconId id, const IconWeight weight) {
    const auto* const asset = std::ranges::find(iconAssets(), id, &IconAsset::id);
    if (asset == iconAssets().end()) {
        return {};
    }
    // Task VIEW-1: all three vendored weights are now COMPLETE subsets (48 ids each), so this is a
    // total mapping rather than the five-id Bold exception it used to carry. IconRole::Chrome asks
    // for Bold on every chrome glyph in the application, and an id without a Bold file would have
    // rendered as a silent blank.
    switch (weight) {
    case IconWeight::Bold:
        return QStringLiteral(":/bloom/kit/phosphor-icons/bold/%1-bold.svg")
            .arg(asset->upstreamName);
    case IconWeight::Fill:
        return QStringLiteral(":/bloom/kit/phosphor-icons/fill/%1-fill.svg")
            .arg(asset->upstreamName);
    case IconWeight::Regular:
        break;
    }
    return QStringLiteral(":/bloom/kit/phosphor-icons/regular/%1.svg").arg(asset->upstreamName);
}

QColor iconTint(const Color role, const State state) {
    switch (state) {
    case State::Normal:
    case State::Focused:
        return color(role);
    case State::Hover:
    case State::Pressed:
    case State::Selected:
        // On hover, press, and selection the icon sits on a raised or accent surface, so it takes
        // full-strength ink rather than the muted role it rests at.
        return color(Color::Foreground);
    case State::Disabled:
        return withOpacity(color(role), kDisabledOpacity);
    }
    return color(role);
}

QPixmap iconPixmap(const IconId id, const Size size, const QColor& tint,
                   const qreal devicePixelRatio, const IconWeight weight) {
    const qreal ratio = resolvedDevicePixelRatio(devicePixelRatio);
    const CacheKey key{id, weight, size, tint.rgba(),
                       static_cast<int>(std::lround(ratio * 1000.0))};

    if (const auto found = cache().constFind(key); found != cache().constEnd()) {
        return found.value();
    }
    QPixmap rendered = renderIcon(iconResourcePath(id, weight), px(size), tint, ratio);
    cache().insert(key, rendered);
    return rendered;
}

QPixmap iconPixmap(const IconId id, const Size size, const Color role, const State state,
                   const IconWeight weight, const qreal devicePixelRatio) {
    return iconPixmap(id, size, iconTint(role, state), devicePixelRatio, weight);
}

QPixmap iconPixmap(const IconId id, const IconRole iconRole, const Color role, const State state,
                   const qreal devicePixelRatio) {
    return iconPixmap(id, iconSize(iconRole), iconTint(role, state), devicePixelRatio,
                      iconWeight(iconRole));
}

QIcon icon(const IconId id, const IconRole iconRole, const Color role) {
    return icon(id, iconSize(iconRole), role, iconWeight(iconRole));
}

namespace {
class SvgIconEngine final : public QIconEngine {
  public:
    SvgIconEngine(IconId id, Color role, IconWeight weight)
        : id_(id), role_(role), weight_(weight) {}
    QIconEngine* clone() const override { return new SvgIconEngine(id_, role_, weight_); }
    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override {
        return scaledPixmap(size, mode, state, 1.0);
    }
    QPixmap scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State, qreal scale) override {
        const auto state = mode == QIcon::Disabled ? State::Disabled
                           : mode == QIcon::Normal ? State::Normal
                                                   : State::Hover;
        return renderIcon(iconResourcePath(id_, weight_), std::min(size.width(), size.height()),
                          iconTint(role_, state), scale);
    }
    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode,
               QIcon::State state) override {
        const qreal dpr = painter->device()->devicePixelRatioF();
        const auto value = scaledPixmap(rect.size(), mode, state, dpr);
        painter->drawPixmap(rect.topLeft(), value);
    }

  private:
    IconId id_;
    Color role_;
    IconWeight weight_;
};
} // namespace
QIcon icon(const IconId id, const Size, const Color role, const IconWeight weight) {
    return QIcon(new SvgIconEngine(id, role, weight));
}

std::size_t iconCacheEntryCount() { return static_cast<std::size_t>(cache().size()); }

void clearIconCache() { cache().clear(); }

} // namespace bloom::ui::kit
