#include <bloom/ui/composition_authoring.hpp>

#include "composition_editor_support.hpp"

#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/timeline_editor.hpp>

#include <bloom/ui/kit/color.hpp>
#include <bloom/ui/kit/icons.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <bloom/core/color.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/parameter.hpp>

#include <QCoreApplication>
#include <QMouseEvent>
#include <QPainter>

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <system_error>
#include <variant>

namespace bloom::ui {
namespace {

constexpr std::array kDefaultSolidPalette{
    core::Color4d{0.62, 0.08, 0.04, 1.0},
    core::Color4d{0.04, 0.20, 0.72, 1.0},
    core::Color4d{0.06, 0.52, 0.16, 1.0},
    core::Color4d{0.46, 0.07, 0.58, 1.0},
};

qulonglong nextLayerNumber(const CompositionSession& session, const std::string_view typeId,
                           const std::uint32_t schemaVersion) {
    qulonglong count = 0;
    const auto* composition = session.composition();
    if (composition == nullptr) {
        return 1;
    }
    for (const auto& entry : composition->graph().layerStack().entries()) {
        const auto* sourceNode = directSourceNode(session, entry.layerId);
        if (isKnownSource(sourceNode, typeId, schemaVersion)) {
            ++count;
        }
    }
    return count + 1;
}

QString exactNumber(const double value) {
    std::array<char, 64> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                                      std::chars_format::general);
    if (result.ec != std::errc{}) {
        return QString::number(value, 'g', std::numeric_limits<double>::max_digits10);
    }
    return QString::fromLatin1(buffer.data(), static_cast<qsizetype>(result.ptr - buffer.data()));
}

} // namespace

QString parameterSourceDescription(const document::ParameterRecord& parameter) {
    if (std::holds_alternative<document::AnimationCurveSource>(parameter.source)) {
        return QStringLiteral("Animated");
    }
    if (std::holds_alternative<document::DriverBindingSource>(parameter.source)) {
        return QStringLiteral("Driven by graph");
    }
    return QStringLiteral("Constant");
}

QString blendModeDisplayName(const core::BlendMode mode) {
    // QCoreApplication::translate() rather than QStringLiteral: this is an artist-facing vocabulary
    // shown in three controls, so it has to be translatable, and a free function has no tr() of its
    // own. The context is the vocabulary, not any one widget, because all three surfaces show the
    // same words.
    const auto* const name = [mode]() -> const char* {
        switch (mode) {
        case core::BlendMode::Normal:
            return "Normal";
        case core::BlendMode::Add:
            return "Add";
        case core::BlendMode::Multiply:
            return "Multiply";
        case core::BlendMode::Screen:
            return "Screen";
        case core::BlendMode::Overlay:
            return "Overlay";
        case core::BlendMode::Darken:
            return "Darken";
        case core::BlendMode::Lighten:
            return "Lighten";
        case core::BlendMode::Difference:
            return "Difference";
        }
        return "Normal";
    }();
    return QCoreApplication::translate("bloom::ui::BlendMode", name);
}

QString exactColorText(const core::Color4d color) {
    return QStringLiteral("R %1  G %2  B %3  A %4")
        .arg(exactNumber(color.red), exactNumber(color.green), exactNumber(color.blue),
             exactNumber(color.alpha));
}

bool addDefaultSolidLayer(CompositionSession& session) {
    const auto layerNumber = nextLayerNumber(session, document::kSolidSourceNodeType,
                                             document::kSolidSourceNodeSchemaVersion);
    const auto paletteIndex =
        static_cast<std::size_t>(layerNumber - 1) % kDefaultSolidPalette.size();
    return session.addSolidLayer(TimelineEditor::tr("Solid %1").arg(layerNumber),
                                 kDefaultSolidPalette[paletteIndex]);
}

bool addDefaultTextLayer(CompositionSession& session) {
    const auto layerNumber = nextLayerNumber(session, document::kTextSourceNodeType,
                                             document::kTextSourceNodeSchemaVersion);
    return session.addTextLayer(TimelineEditor::tr("Text %1").arg(layerNumber),
                                TimelineEditor::tr("Text"));
}

namespace {

// The role, spelled for a human: the structural identifier with its hyphens opened out. Used only
// in this diamond's own tooltips, which have to name what the click will animate.
[[nodiscard]] QString roleDisplayName(const std::string& role) {
    QString display = QString::fromStdString(role);
    display.replace(QLatin1Char('-'), QLatin1Char(' '));
    return display;
}

} // namespace

KeyframeDiamond::KeyframeDiamond(CompositionSession& session, std::string role, QWidget* parent)
    : QWidget(parent), session_(session), role_(std::move(role)) {
    setObjectName(QStringLiteral("keyframeDiamond"));
    setAccessibleName(QStringLiteral("Keyframe for %1").arg(roleDisplayName(role_)));
    setFixedSize(kit::px(kit::Size::IconSmall), kit::px(kit::Size::IconSmall));
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::NoFocus);
    setAttribute(Qt::WA_Hover, true);
    refresh();
}

void KeyframeDiamond::setRole(std::string role) {
    role_ = std::move(role);
    refresh();
}

void KeyframeDiamond::setParameterId(const document::ParameterId parameterId) {
    auto next = parameterId.isValid() ? std::optional(parameterId) : std::nullopt;
    if (parameterId_ == next) {
        return;
    }
    parameterId_ = next;
    refresh();
}

void KeyframeDiamond::refresh() {
    const auto next = parameterId_.has_value()
                          ? session_.keyframeDiamondStateForParameter(*parameterId_)
                          : session_.keyframeDiamondState(role_);
    const bool visible = next != KeyframeDiamondState::Unsupported;
    // A parameter this gesture cannot key shows no diamond at all rather than an inert one: an
    // unclickable affordance is a worse lie than an absent one. setVisible() (not setEnabled()) so
    // the row's layout does not reserve an empty cell that reads as a missing glyph.
    setVisible(visible);
    const QString display = roleDisplayName(role_);
    switch (next) {
    case KeyframeDiamondState::Unsupported:
        setToolTip({});
        break;
    case KeyframeDiamondState::Constant:
        setToolTip(QStringLiteral("Constant — click to animate %1 from here").arg(display));
        break;
    case KeyframeDiamondState::AnimatedWithoutKey:
        setToolTip(QStringLiteral("Animated — click to add a %1 key at this time").arg(display));
        break;
    case KeyframeDiamondState::AnimatedWithKey:
        setToolTip(QStringLiteral("Key at this time — click to remove it"));
        break;
    }
    if (state_ != next) {
        state_ = next;
        update();
    }
}

void KeyframeDiamond::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event)
    if (state_ == KeyframeDiamondState::Unsupported) {
        return;
    }
    // The one place the three states become two kit inputs: which ink, and which of the kit's two
    // existing icon weights. "Dimmed" reuses tokens::kDisabledOpacity rather than a new literal --
    // dimmed ink and disabled ink are the same fade recipe on a different colour -- and a hover
    // brightens a constant row's diamond so the click target reads as clickable before it is an
    // "on" state.
    const bool animated = state_ != KeyframeDiamondState::Constant;
    QColor tint = animated || hovered_
                      ? kit::color(kit::Color::Keyframe)
                      : kit::withOpacity(kit::color(kit::Color::Muted), kit::kDisabledOpacity);
    if (!animated && hovered_) {
        tint = kit::withOpacity(tint, kit::kDisabledOpacity);
    }
    const auto weight = state_ == KeyframeDiamondState::AnimatedWithKey ? kit::IconWeight::Fill
                                                                        : kit::IconWeight::Regular;
    QPainter painter(this);
    painter.drawPixmap(
        rect(), kit::iconPixmap(kit::IconId::Keyframe, kit::Size::IconSmall, tint, 0.0, weight));
}

void KeyframeDiamond::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || state_ == KeyframeDiamondState::Unsupported) {
        QWidget::mousePressEvent(event);
        return;
    }
    // The whole gesture lives in the session: this widget decides nothing about which transaction
    // to build, and holds no state that could disagree with the document afterwards. The owning
    // surface rebuilds off snapshotChanged() and calls refresh() again. A parameter-bound diamond
    // deliberately does NOT select its node first -- the gesture needs no selection, so clicking a
    // key on one card cannot silently retarget the Properties panel.
    (void)(parameterId_.has_value() ? session_.toggleKeyframeForParameter(*parameterId_)
                                    : session_.toggleKeyframe(role_));
    event->accept();
}

void KeyframeDiamond::enterEvent(QEnterEvent* event) {
    hovered_ = true;
    update();
    QWidget::enterEvent(event);
}

void KeyframeDiamond::leaveEvent(QEvent* event) {
    hovered_ = false;
    update();
    QWidget::leaveEvent(event);
}

} // namespace bloom::ui
