#include <bloom/ui/composition_editors.hpp>

#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/timeline_frame_math.hpp>
#include <bloom/ui/timeline_ruler.hpp>

#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/icons.hpp>
#include <bloom/ui/kit/painting.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/kit/value_field.hpp>

#include <bloom/core/color.hpp>
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/project.hpp>

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QBrush>
#include <QEnterEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QKeySequence>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QModelIndex>
#include <QPainter>
#include <QPalette>
#include <QSignalBlocker>
#include <QSize>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>

namespace bloom::ui {
namespace {

constexpr int kTimelineLayerIdRole = Qt::UserRole + 1;
constexpr int kTimelineSlotIdRole = Qt::UserRole + 2;
constexpr int kCompositionIdRole = Qt::UserRole + 1;
constexpr std::array kDefaultSolidPalette{
    core::Color4d{0.62, 0.08, 0.04, 1.0},
    core::Color4d{0.04, 0.20, 0.72, 1.0},
    core::Color4d{0.06, 0.52, 0.16, 1.0},
    core::Color4d{0.46, 0.07, 0.58, 1.0},
};

const document::LayerOutputBoundary* layerBoundary(const document::Composition& composition,
                                                   const document::LayerId layerId) {
    const auto boundaries = composition.graph().layerOutputs();
    const auto found = std::ranges::find_if(
        boundaries, [layerId](const auto& candidate) { return candidate.layerId == layerId; });
    return found == boundaries.end() ? nullptr : &*found;
}

QString sourceDescription(const document::ParameterRecord& parameter) {
    if (std::holds_alternative<document::AnimationCurveSource>(parameter.source)) {
        return QStringLiteral("Animated");
    }
    if (std::holds_alternative<document::DriverBindingSource>(parameter.source)) {
        return QStringLiteral("Driven by graph");
    }
    return QStringLiteral("Constant");
}

QString layerName(const document::Composition& composition, const document::LayerId layerId) {
    const auto* boundary = layerBoundary(composition, layerId);
    if (boundary == nullptr || boundary->name.empty()) {
        return QStringLiteral("Layer %1").arg(layerId.value());
    }
    return QString::fromStdString(boundary->name);
}

const document::NodeRecord* directSourceNode(const CompositionSession& session,
                                             const document::LayerId layerId) {
    const auto* composition = session.composition();
    const auto sourceNodeId = session.directSourceNodeForLayer(layerId);
    return composition != nullptr && sourceNodeId.has_value()
               ? composition->graph().findNode(*sourceNodeId)
               : nullptr;
}

bool isKnownSource(const document::NodeRecord* node, const std::string_view typeId,
                   const std::uint32_t schemaVersion) {
    return node != nullptr && node->typeId == typeId && node->schemaVersion == schemaVersion;
}

const document::NodeRecord* selectedPresentationSource(const CompositionSession& session) {
    if (const auto* layerId = std::get_if<document::LayerId>(&session.selection().primary)) {
        return directSourceNode(session, *layerId);
    }
    return session.selectedNode();
}

QString layerKind(const CompositionSession& session, const document::LayerId layerId) {
    const auto* sourceNode = directSourceNode(session, layerId);
    if (isKnownSource(sourceNode, document::kSolidSourceNodeType,
                      document::kSolidSourceNodeSchemaVersion)) {
        return TimelineEditor::tr("Solid");
    }
    if (isKnownSource(sourceNode, document::kTextSourceNodeType,
                      document::kTextSourceNodeSchemaVersion)) {
        return TimelineEditor::tr("Text");
    }
    return TimelineEditor::tr("Layer");
}

// Lane bar color mapping (task U7, issue #122, decision 2). Bloom's data-type palette
// (kit::Color::Data*) identifies external MEDIA kinds -- image sequences, clips, still images,
// audio -- that this project has no import pipeline for yet (verified: no media-backed layer type
// exists anywhere in src/document). Mapping a Solid or Text layer to one of those roles (e.g.
// "Solid = DataClip green", the literal example this task's own decision explicitly rejects) would
// misrepresent a generated, composition-local layer as referenced media it is not.
// `DataComposition` is the one palette entry that is NOT about referenced media: its own stated
// meaning is "Compositions" -- content that is authored/evaluated INSIDE a project rather than
// referenced from an external asset, exactly the property every layer kind Bloom has today (Solid,
// Text) shares. This is therefore ONE mapping applied uniformly to every layer kind that exists
// right now, not a per-kind table -- there is nothing to honestly differentiate yet. A future
// media-backed layer (image/clip/sequence/audio import) would take its own, more specific Data*
// token when that feature ships; this function is the one place that day's change would land.
[[nodiscard]] kit::Color layerLaneColorToken() { return kit::Color::DataComposition; }

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

QString exactColor(const core::Color4d color) {
    return QStringLiteral("R %1  G %2  B %3  A %4")
        .arg(exactNumber(color.red), exactNumber(color.green), exactNumber(color.blue),
             exactNumber(color.alpha));
}

QString selectionName(const CompositionSession& session) {
    const auto* composition = session.composition();
    if (composition == nullptr) {
        return QStringLiteral("No composition");
    }
    if (const auto* layerId = std::get_if<document::LayerId>(&session.selection().primary)) {
        return layerName(*composition, *layerId);
    }
    if (const auto* nodeId = std::get_if<document::NodeId>(&session.selection().primary)) {
        const auto* node = composition->graph().findNode(*nodeId);
        return node == nullptr ? QStringLiteral("Node unavailable")
                               : QString::fromStdString(node->typeId);
    }
    if (const auto* parameterId =
            std::get_if<document::ParameterId>(&session.selection().primary)) {
        const auto* parameter = composition->parameters().find(*parameterId);
        return parameter == nullptr ? QStringLiteral("Parameter unavailable")
                                    : QString::fromStdString(parameter->schemaKey);
    }
    return QStringLiteral("Nothing selected");
}

QToolButton* makeToolButton(const QString& text, const QString& accessibleName, QWidget* parent) {
    auto* button = new QToolButton(parent);
    button->setText(text);
    button->setAccessibleName(accessibleName);
    button->setAutoRaise(true);
    return button;
}

// Task U7 (issue #122), decision 5: transport controls get their kit icon glyph via the SAME
// "plain QToolButton + kit::icon()" idiom EditorArea's own header chrome already uses
// (editor_area.cpp's makeHeaderButton) -- not kit::KButton, which is not a QToolButton and would
// break every existing findChild<QToolButton*>("playPauseButton") test contract
// (composition_projection_test.cpp, playback_controller_tests.cpp). An icon never replaces an
// accessible name (ADR 0010; docs/ux/visual-language.md's Iconography section), so every call site
// still sets both a tooltip and setAccessibleName().
QToolButton* makeIconToolButton(const kit::IconId iconId, const QString& toolTip,
                                const QString& accessibleName, const QString& objectName,
                                QWidget* parent) {
    auto* button = new QToolButton(parent);
    button->setObjectName(objectName);
    button->setIcon(kit::icon(iconId, kit::Size::IconMedium));
    button->setIconSize(QSize(kit::px(kit::Size::IconMedium), kit::px(kit::Size::IconMedium)));
    button->setToolTip(toolTip);
    button->setAccessibleName(accessibleName);
    button->setAutoRaise(true);
    return button;
}

// Frame stepping / readout (issue #108): the frame rate, duration, and checked maximum frame index
// every stepFrame()/stepToStart()/stepToEnd()/updateTimeReadout() call needs, resolved the one
// place so they cannot silently drift from each other's notion of the composition's valid frame
// range -- mirroring mappingForComposition() in playback_controller.cpp, which this deliberately
// does NOT reuse (it is private to that translation unit and this task's fence forbids touching the
// playback controller beyond the smallest justified accessor -- see stepFrame()'s own comment on
// why none was needed). std::nullopt covers no live composition or a rate/duration
// bloom::core::FrameTimeMapping itself refuses, exactly like every other caller of
// timeline_frame_math.hpp's adapters.
struct TimelineFrameContext final {
    document::FrameRate frameRate;
    core::RationalTime duration;
    std::uint64_t maxFrameIndexValue;
};

[[nodiscard]] std::optional<TimelineFrameContext>
frameContextFor(const CompositionSession& session) {
    const auto* composition = session.composition();
    if (composition == nullptr) {
        return std::nullopt;
    }
    const auto frameRate = composition->format().frameRate();
    const auto duration = composition->duration();
    const auto maxIndex = maxFrameIndex(frameRate, duration);
    if (!maxIndex.has_value()) {
        return std::nullopt;
    }
    return TimelineFrameContext{frameRate, duration, *maxIndex};
}

// Formats an exact RationalTime as seconds with EXACTLY 3 truncated decimal digits (millisecond
// resolution), computed purely from the integer numerator/denominator -- never through
// RationalTime::toSeconds()'s binary64 conversion -- so the digits shown are always the value's
// true leading digits, never a rounded/binary64-approximated one (design decision 3: "no
// floating-point accumulation... a subframe time must display honestly"). Three places is a
// deliberately BOUNDED cut of what can be an infinite decimal expansion (e.g. 1 s / 3 has no exact
// finite decimal form); truncating rather than rounding means the displayed digits never overstate
// the exact value. The widening multiply uses a 128-bit intermediate purely so an extreme
// duration's denominator cannot silently overflow a 64-bit product -- unreachable for any realistic
// composition, kept checked rather than UB regardless; this is ordinary display arithmetic, not
// part of the sampling contract's own "no compiler-specific extended integers" rule
// (docs/architecture/animation-and-time.md, "Sampling Semantics Version 1"), which governs curve
// evaluation only.
[[nodiscard]] QString formatExactSeconds(const core::RationalTime time) {
    constexpr int kDecimalPlaces = 3;
    constexpr unsigned __int128 kScale = 1000;
    const std::int64_t numerator = time.numerator();
    const auto denominator = static_cast<unsigned __int128>(time.denominator());
    const bool negative = numerator < 0;
    const auto magnitude = negative
                               ? static_cast<unsigned __int128>(-static_cast<__int128>(numerator))
                               : static_cast<unsigned __int128>(numerator);
    const auto wholeSeconds = static_cast<qulonglong>(magnitude / denominator);
    const auto remainder = magnitude % denominator;
    const auto scaledFraction = static_cast<qulonglong>((remainder * kScale) / denominator);
    return QStringLiteral("%1%2.%3s")
        .arg(negative ? QStringLiteral("-") : QString())
        .arg(wholeSeconds)
        .arg(scaledFraction, kDecimalPlaces, 10, QChar('0'));
}

// The 2px inset selection edge (task U7, issue #122, decision 1: "selection = Accent inset edge
// per States" -- docs/ux/visual-language.md's "Selected: an Accent fill, or a 2px inset accent edge
// where a fill would hide content"; a full fill would hide this row's own name/kind text, so the
// edge variant applies).
constexpr qreal kTrackRowSelectionEdgeWidth = 2.0;

// Restyles layerStackView's rows (decision 1: "Row striping via surface ladder; selection = Accent
// inset edge per States") without touching a single byte of QTreeWidgetItem's own model data --
// text()/icon()/toolTip()/isSelected() and every existing test reading them through
// findChild<QTreeWidget*>("layerStackView") are completely untouched. Only sizeHint() (row height,
// decision 1's TimelineRow token) and paint() (background + selection) are overridden; the base
// QStyledItemDelegate::paint() still draws each column's own icon/text/embedded item-widget
// afterward, using whatever font/foreground QTreeWidgetItem::setFont()/setForeground() were given
// at row-build time (see TimelineEditor::rebuild()) -- so this delegate owns none of the actual ink
// color decisions, only the shared background/selection recipe every column sits on.
class TimelineTrackRowDelegate final : public QStyledItemDelegate {
  public:
    explicit TimelineTrackRowDelegate(QObject* parent) : QStyledItemDelegate(parent) {}

    [[nodiscard]] QSize sizeHint(const QStyleOptionViewItem& option,
                                 const QModelIndex& index) const override {
        QSize hint = QStyledItemDelegate::sizeHint(option, index);
        hint.setHeight(kit::px(kit::Size::TimelineRow));
        return hint;
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        painter->save();
        const bool selected = (option.state & QStyle::State_Selected) != 0;
        // "Row striping via surface ladder": odd rows step one rung up from Background, the same
        // ladder the rest of the interface steps hover/press states along.
        const auto surfaceToken = (index.row() % 2) != 0
                                      ? kit::surfaceStep(kit::Color::Background, 1)
                                      : kit::Color::Background;
        painter->fillRect(option.rect, kit::color(surfaceToken));
        if (selected) {
            const qreal inset = kTrackRowSelectionEdgeWidth / 2.0;
            painter->setPen(QPen(kit::color(kit::Color::Accent), kTrackRowSelectionEdgeWidth));
            painter->setBrush(Qt::NoBrush);
            painter->drawRect(QRectF(option.rect).adjusted(inset, inset, -inset, -inset));
        }
        painter->restore();

        // Suppress Qt's own native full-cell selection fill (already replaced above by the inset
        // edge) before delegating icon/text painting to the base implementation.
        QStyleOptionViewItem plain(option);
        plain.state &= ~QStyle::State_Selected;
        plain.showDecorationSelected = false;
        QStyledItemDelegate::paint(painter, plain, index);
    }
};

// The per-row lane bar (decision 2): a single rounded, type-color-coded bar filling its ENTIRE
// allotted width. There is no trim/range feature (no in/out point exists in the document model at
// all), so "spans the full composition duration honestly" is true by construction here -- the bar
// is never partial, because nothing in this widget can express a partial state.
//
// Honesty disclosure (this task's raw report): this bar's width is the layerStackView TREE
// COLUMN's own width, not literally the same pixel-space TimelineRuler/TimelineKeyframePanel use --
// those are separate, independently-scrollable widgets from the layer list, and pixel-perfect
// cross-widget time-axis alignment between a QTreeWidget column and the ruler was never a wired
// feature before this task. Building that alignment is a genuine architecture change outside a
// restyle task's fence, so this bar honestly represents "this layer's content, for its entire
// duration" within its own dedicated column rather than pretending to share the ruler's exact
// scroll/zoom state.
class TimelineLaneBar final : public QWidget {
  public:
    explicit TimelineLaneBar(const kit::Color colorToken, QWidget* parent)
        : QWidget(parent), colorToken_(colorToken) {
        setObjectName(QStringLiteral("layerLaneBar"));
        setMinimumHeight(kit::px(kit::Size::TimelineRow) - 2 * kit::px(kit::Spacing::XXS));
    }

  protected:
    void paintEvent(QPaintEvent* event) override {
        Q_UNUSED(event)
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QRectF bounds =
            QRectF(rect()).adjusted(0.0, static_cast<qreal>(kit::px(kit::Spacing::XXS)), 0.0,
                                    -static_cast<qreal>(kit::px(kit::Spacing::XXS)));
        kit::fillRoundedSurface(painter, bounds, kit::color(colorToken_), QColor(),
                                kit::Radius::Small);
    }

  private:
    kit::Color colorToken_;
};

// Blending/Parent (decision 1): one always-disabled KDropdown per row, each carrying its single
// honest value ("Normal" / "None") -- no blend-mode breadth and no parenting feature exist yet, so
// there is nothing else to offer, and the tooltip says so rather than the control merely looking
// unresponsive.
kit::KDropdown* makeDisabledPlaceholderDropdown(const QString& value, const QString& toolTip,
                                                const QString& objectName, QWidget* parent) {
    auto* dropdown = new kit::KDropdown(parent);
    dropdown->setObjectName(objectName);
    dropdown->addItem(value);
    dropdown->setEnabled(false);
    dropdown->setToolTip(toolTip);
    return dropdown;
}

// Issue #120 (task U5), decisions 1/2: the properties panel's kit field grid. A row is
// [right-aligned Muted label][optional gold/dimmed Keyframe indicator][value widget], and every
// row lives inside a PropertiesRow so hovering anywhere across the row -- label, indicator, or
// value -- paints the SAME States-recipe highlight (docs/ux/visual-language.md, "State": "Hover:
// one surface step up, plus BorderHover"). Qt delivers Enter/Leave to the common ancestor of the
// previously- and newly-hovered leaf widgets only when that ancestor's OWN membership in the
// "currently entered" chain changes, so moving the pointer between a row's own label and its value
// cell never toggles this row's hover off: the row is entered once and left once, exactly as a
// single hoverable unit.
class PropertiesRow final : public QWidget {
  public:
    explicit PropertiesRow(QWidget* parent) : QWidget(parent) {
        setObjectName(QStringLiteral("propertiesRow"));
        setMinimumHeight(kit::px(kit::Size::Control));
    }

  protected:
    void enterEvent(QEnterEvent* event) override {
        hovered_ = true;
        update();
        QWidget::enterEvent(event);
    }

    void leaveEvent(QEvent* event) override {
        hovered_ = false;
        update();
        QWidget::leaveEvent(event);
    }

    void paintEvent(QPaintEvent* event) override {
        Q_UNUSED(event)
        if (!hovered_) {
            return;
        }
        QPainter painter(this);
        // The panel itself paints no surface of its own (PropertiesEditor sits directly on the
        // editor area's Background), so the row's resting surface for the recipe is Background --
        // hover steps it to Surface, exactly one step up the ladder.
        kit::fillRoundedSurface(
            painter, rect(),
            kit::color(kit::surfaceForState(kit::Color::Background, kit::State::Hover)),
            kit::color(kit::borderForState(kit::State::Hover)), kit::Radius::Small);
    }

  private:
    bool hovered_ = false;
};

// The fixed right-aligned label column every row in the panel shares (decision 1), sized once from
// the widest label this panel can ever show rather than a spelled pixel width -- KValueField's own
// internal sub-label column (kLabelColumnWidth in value_field.cpp) is private to that widget and
// exists for a narrower purpose (a field-local "X"/"Y" prefix inside the cell itself), so the row's
// OUTER label column, which names the whole parameter, is measured independently here.
int propertyLabelColumnWidth() {
    static const std::array<QString, 10> kLabels{
        PropertiesEditor::tr("Position"), PropertiesEditor::tr("Opacity"),
        PropertiesEditor::tr("RGBA"),     PropertiesEditor::tr("Alpha"),
        PropertiesEditor::tr("Encoding"), PropertiesEditor::tr("Name"),
        PropertiesEditor::tr("Format"),   PropertiesEditor::tr("Frame Rate"),
        PropertiesEditor::tr("Duration"), PropertiesEditor::tr("Pixel Aspect"),
    };
    const QFontMetrics metrics(kit::font(kit::TypeRole::Ui));
    int widest = 0;
    for (const auto& label : kLabels) {
        widest = std::max(widest, metrics.horizontalAdvance(label));
    }
    return widest;
}

QLabel* makePropertyRowLabel(const QString& text, const int columnWidth, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("propertiesRowLabel"));
    label->setFont(kit::font(kit::TypeRole::Ui));
    QPalette palette = label->palette();
    palette.setColor(QPalette::WindowText, kit::color(kit::Color::Muted));
    label->setPalette(palette);
    label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    label->setFixedWidth(columnWidth);
    return label;
}

QLabel* makeKeyframeIndicator(QWidget* parent) {
    auto* indicator = new QLabel(parent);
    indicator->setObjectName(QStringLiteral("propertiesKeyframeIndicator"));
    indicator->setFixedSize(kit::px(kit::Size::IconSmall), kit::px(kit::Size::IconSmall));
    return indicator;
}

// A read-only value cell's text: `role` is Value (Geist Mono) for numeric-looking content --
// RGBA, format, frame rate, duration, pixel aspect -- and Ui for prose -- the alpha association
// sentence, the composition name. None of these are editable through the current session API
// (issue #120, decision 1's read-only carve-out: "do not add editing capability that doesn't
// exist today"), so they stay plain selectable text rather than kit::KValueField, which has no way
// to carry a string and would otherwise misrepresent them as steppable controls.
QLabel* makeReadOnlyValueLabel(const kit::TypeRole role, QWidget* parent) {
    auto* label = new QLabel(parent);
    label->setFont(kit::font(role));
    QPalette palette = label->palette();
    palette.setColor(QPalette::WindowText, kit::color(kit::Color::Foreground));
    label->setPalette(palette);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    return label;
}

// Wraps `label` (+ optional `indicator`) and `value` in one PropertiesRow, added to `section`'s
// layout. `section` also parents label/indicator/value at construction, but QBoxLayout::addWidget
// below reparents each into the row -- the same "construct with `this`, let the layout reparent"
// idiom the rest of this file already uses for its form rows.
PropertiesRow* addPropertyRow(QVBoxLayout* section, QWidget* sectionParent, QLabel* label,
                              QLabel* indicator, QWidget* value) {
    auto* row = new PropertiesRow(sectionParent);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(kit::px(kit::Spacing::XS), kit::px(kit::Spacing::XXS),
                               kit::px(kit::Spacing::XS), kit::px(kit::Spacing::XXS));
    layout->setSpacing(kit::px(kit::Spacing::S));
    layout->addWidget(label);
    if (indicator != nullptr) {
        layout->addWidget(indicator);
    }
    layout->addWidget(value, 1);
    section->addWidget(row);
    return row;
}

// A UISmall uppercase group header (decision 1: "Transform", "Appearance", a source-specific
// group) plus its hairline divider (decision 1: "section dividers as hairlines"), appended to
// `section`.
void addSectionHeader(QVBoxLayout* section, QWidget* parent, const QString& title) {
    // "editorSectionTitle" already names TimelineEditor's "Layers" title and MediaEditor's
    // "Project" title (unchanged by this task): reused here rather than a new name so every group
    // header in the workspace -- Transform/Appearance/Solid Source included -- is the same logical
    // widget kind, per decision 4's "existing objectNames preserved."
    auto* header = new QLabel(title.toUpper(), parent);
    header->setObjectName(QStringLiteral("editorSectionTitle"));
    header->setFont(kit::font(kit::TypeRole::UiSmall));
    QPalette headerPalette = header->palette();
    headerPalette.setColor(QPalette::WindowText, kit::color(kit::Color::Muted));
    header->setPalette(headerPalette);
    section->addWidget(header);

    auto* divider = new QWidget(parent);
    divider->setObjectName(QStringLiteral("propertiesSectionDivider"));
    divider->setFixedHeight(static_cast<int>(std::lround(kit::kHairlineWidth)));
    QPalette dividerPalette = divider->palette();
    dividerPalette.setColor(QPalette::Window, kit::color(kit::Color::Border));
    divider->setPalette(dividerPalette);
    divider->setAutoFillBackground(true);
    section->addWidget(divider);
}

// True when `parameter` carries an animation curve source (issue #120, decision 2: "truth from
// the session snapshot, no new session API") -- the same std::holds_alternative check
// sourceDescription() above already uses to report "Animated", read directly rather than by
// string-comparing that tooltip text.
bool isAnimatedParameter(const document::ParameterRecord* parameter) {
    return parameter != nullptr &&
           std::holds_alternative<document::AnimationCurveSource>(parameter->source);
}

// Paints `indicator` gold-filled when `parameter` is animation-sourced, Muted-dimmed-outline
// otherwise (decision 2). The dim opacity reuses tokens::kDisabledOpacity rather than a new
// literal: "dimmed" and "disabled ink" are the same fade recipe applied to a different ink. The
// weight switch follows docs/ux/visual-language.md's own iconography rule verbatim ("regular is
// the default visual weight and fill for selected or toggled states"): an animated parameter is
// this indicator's "on" state, so it takes the solid diamond-fill glyph rather than the outline
// one static rows show.
void updateKeyframeIndicator(QLabel* indicator, const document::ParameterRecord* parameter) {
    const bool animated = isAnimatedParameter(parameter);
    const QColor tint =
        animated ? kit::color(kit::Color::Keyframe)
                 : kit::withOpacity(kit::color(kit::Color::Muted), kit::kDisabledOpacity);
    const auto weight = animated ? kit::IconWeight::Fill : kit::IconWeight::Regular;
    indicator->setPixmap(
        kit::iconPixmap(kit::IconId::Keyframe, kit::Size::IconSmall, tint, 0.0, weight));
    indicator->setToolTip(animated ? PropertiesEditor::tr("Animated")
                                   : PropertiesEditor::tr("Static"));
}

// Frame rate as an exact rational (decision 3: "exact rational shown honestly"): numerator and
// denominator are exact std::uint32_t, so this never rounds -- it only omits the denominator when
// it is exactly 1 (the common whole-fps case) rather than always spelling "24/1 fps".
QString formatFrameRate(const document::FrameRate rate) {
    if (rate.denominator() == 1) {
        return PropertiesEditor::tr("%1 fps").arg(rate.numerator());
    }
    return PropertiesEditor::tr("%1/%2 fps").arg(rate.numerator()).arg(rate.denominator());
}

QString formatPixelAspect(const core::PixelAspectRatio pixelAspect) {
    return QStringLiteral("%1:%2").arg(pixelAspect.numerator()).arg(pixelAspect.denominator());
}

QString formatCompositionFormat(const document::CompositionFormat format) {
    return PropertiesEditor::tr("%1 × %2 px").arg(format.width()).arg(format.height());
}

// Duration as frame count + exact seconds (decision 3: "duration (frames + seconds via the exact
// formatting rule)"), reusing formatExactSeconds() above verbatim -- the SAME truncated-rational
// formatter TimelineEditor::updateTimeReadout() uses for the current-time readout, rather than a
// second, possibly-inconsistent formatting rule for duration. Frame count is maxFrameIndex + 1
// (the greatest valid index is 0-based).
QString formatDuration(const TimelineFrameContext& context) {
    return PropertiesEditor::tr("%1 frames · %2")
        .arg(context.maxFrameIndexValue + 1)
        .arg(formatExactSeconds(context.duration));
}

} // namespace

TimelineEditor::TimelineEditor(CompositionSession& session,
                               CompositionPreviewController& previewController, QWidget* parent)
    : QWidget(parent), session_(session) {
    setObjectName("timelineEditor");
    setAccessibleName(tr("Layers timeline"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* controls = new QWidget(this);
    controls->setObjectName("timelineControls");
    auto* controlsLayout = new QHBoxLayout(controls);
    controlsLayout->setContentsMargins(kit::px(kit::Spacing::S), kit::px(kit::Spacing::XS),
                                       kit::px(kit::Spacing::S), kit::px(kit::Spacing::XS));
    controlsLayout->setSpacing(kit::px(kit::Spacing::XS));

    auto* title = new QLabel(tr("Layers"), controls);
    title->setObjectName("editorSectionTitle");
    addButton_ = makeToolButton(tr("Add"), tr("Add layer"), controls);
    addButton_->setObjectName("addLayerButton");
    // Decision 5: "Add menu restyled" -- the Add glyph itself; menu entries stay plain text (no
    // Data* token honestly names a generic structured layer the way Add names the action).
    addButton_->setIcon(kit::icon(kit::IconId::Add, kit::Size::IconMedium));
    addButton_->setIconSize(QSize(kit::px(kit::Size::IconMedium), kit::px(kit::Size::IconMedium)));
    addButton_->setPopupMode(QToolButton::InstantPopup);
    addButton_->setToolTip(tr("Add a structured layer"));
    auto* addMenu = new QMenu(tr("Add Layer"), addButton_);
    addMenu->setObjectName("addLayerMenu");
    addMenu->setAccessibleName(tr("Add layer menu"));
    auto* addSolidAction = addMenu->addAction(tr("Solid"));
    addSolidAction->setObjectName("addSolidLayerAction");
    addSolidAction->setToolTip(
        tr("Add a solid using the next built-in reference-linear-sRGB proof color"));
    auto* addTextAction = addMenu->addAction(tr("Text"));
    addTextAction->setObjectName("addTextLayerAction");
    addTextAction->setToolTip(tr("Add a text layer"));
    addButton_->setMenu(addMenu);

    // Playback transport (issue #105, decision 4; restyled task U7, issue #122, decision 5): a
    // StepBack/Play-Pause/StepForward/Loop row. playPauseButton_ MUST stay a QToolButton with its
    // existing text()/isChecked() contract (playback_controller_tests.cpp,
    // composition_projection_test.cpp both read it by exactly that type/objectName) -- only its
    // icon changes here; updatePlaybackButton() below swaps the icon alongside the existing
    // text/tooltip swap.
    stepBackButton_ = makeIconToolButton(kit::IconId::StepBack, tr("Step back one frame (Left)"),
                                         tr("Step back one frame"),
                                         QStringLiteral("timelineStepBackButton"), controls);
    playPauseButton_ = makeToolButton(tr("Play"), tr("Toggle playback"), controls);
    playPauseButton_->setObjectName("playPauseButton");
    playPauseButton_->setCheckable(true);
    stepForwardButton_ = makeIconToolButton(
        kit::IconId::StepForward, tr("Step forward one frame (Right)"),
        tr("Step forward one frame"), QStringLiteral("timelineStepForwardButton"), controls);
    // Loop indicator (decision 5): non-interactive status glyph, not a button -- playback always
    // loops (PlaybackController::tick()'s exact modulo wrap) and there is no command to disable it,
    // so a clickable control here would dishonestly imply a toggle that does not exist.
    loopIndicator_ = new QLabel(controls);
    loopIndicator_->setObjectName(QStringLiteral("timelineLoopIndicator"));
    loopIndicator_->setAccessibleName(tr("Playback loops continuously"));
    loopIndicator_->setToolTip(tr("Playback always loops; there is no command to disable it yet"));
    loopIndicator_->setPixmap(kit::iconPixmap(kit::IconId::Loop, kit::Size::IconMedium,
                                              kit::Color::Accent, kit::State::Normal,
                                              kit::IconWeight::Fill));
    // Current time readout (issue #108, decision 3): a small label beside the transport, matching
    // PropertiesEditor::selectionLabel_'s plain-QLabel idiom (setObjectName + selectable text, no
    // bespoke styling). Text is set by updateTimeReadout() below, not here. Restyled (decision 5:
    // "frame/time readout stays the exact formatter, Geist Mono") to the Value type role -- the
    // FORMATTER (formatExactSeconds(), updateTimeReadout()'s own "Frame %1 · %2" shape) is
    // completely unchanged, only the font.
    timeReadout_ = new QLabel(controls);
    timeReadout_->setObjectName("timelineTimeReadout");
    timeReadout_->setAccessibleName(tr("Current frame and time"));
    timeReadout_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    timeReadout_->setFont(kit::font(kit::TypeRole::Value));
    undoButton_ = makeToolButton(tr("Undo"), tr("Undo last edit"), controls);
    redoButton_ = makeToolButton(tr("Redo"), tr("Redo last edit"), controls);
    controlsLayout->addWidget(title);
    controlsLayout->addWidget(addButton_);
    controlsLayout->addWidget(stepBackButton_);
    controlsLayout->addWidget(playPauseButton_);
    controlsLayout->addWidget(stepForwardButton_);
    controlsLayout->addWidget(loopIndicator_);
    controlsLayout->addWidget(timeReadout_);
    controlsLayout->addStretch(1);
    controlsLayout->addWidget(undoButton_);
    controlsLayout->addWidget(redoButton_);

    workArea_ = new TimelineWorkAreaStrip(session_, this);
    ruler_ = new TimelineRuler(session_, previewController, this);
    keyframes_ = new TimelineKeyframePanel(session_, this);
    playback_ = new PlaybackController(session_, previewController, &std::chrono::steady_clock::now,
                                       std::chrono::milliseconds{16}, this);

    layers_ = new QTreeWidget(this);
    layers_->setObjectName("layerStackView");
    layers_->setAccessibleName(tr("Composition layers"));
    layers_->setColumnCount(kColumnCount);
    // Header text stays attached to its LOGICAL column below regardless of the visual reorder
    // (moveSection() a few lines down): text(kNameColumn)/text(kKindColumn) remain the pinned test
    // contract's exact logical columns 0/1.
    layers_->setHeaderLabels(
        {tr("Name"), tr("Kind"), tr("Visible"), tr("Blending"), tr("Parent"), tr("Lane")});
    layers_->setRootIsDecorated(false);
    // Striping now comes from TimelineTrackRowDelegate's own surface-ladder recipe (decision 1),
    // not Qt's native AlternateBase.
    layers_->setAlternatingRowColors(false);
    layers_->setSelectionMode(QAbstractItemView::SingleSelection);
    layers_->setUniformRowHeights(true);
    layers_->setItemDelegate(new TimelineTrackRowDelegate(layers_));
    layers_->header()->setSectionResizeMode(kNameColumn, QHeaderView::Interactive);
    layers_->header()->setSectionResizeMode(kKindColumn, QHeaderView::ResizeToContents);
    layers_->header()->setSectionResizeMode(kVisibilityColumn, QHeaderView::ResizeToContents);
    layers_->header()->setSectionResizeMode(kBlendingColumn, QHeaderView::ResizeToContents);
    layers_->header()->setSectionResizeMode(kParentColumn, QHeaderView::ResizeToContents);
    layers_->header()->setSectionResizeMode(kLaneColumn, QHeaderView::Stretch);
    // Decision 1's left-to-right narrative order (visibility, then name, kind, blending, parent,
    // with the lane body trailing) is a VISUAL reorder only -- QHeaderView::moveSection() moves
    // visual position, never logical index, so text(kNameColumn)/text(kKindColumn) and every
    // setItemWidget(item, kBlendingColumn/...) call below keep addressing the same logical columns
    // regardless of where the header paints them.
    layers_->header()->moveSection(kVisibilityColumn, 0);

    layout->addWidget(controls);
    layout->addWidget(workArea_);
    layout->addWidget(ruler_);
    layout->addWidget(keyframes_);
    layout->addWidget(layers_, 1);

    connect(addSolidAction, &QAction::triggered, this, [this] {
        const auto layerNumber = nextLayerNumber(session_, document::kSolidSourceNodeType,
                                                 document::kSolidSourceNodeSchemaVersion);
        const auto paletteIndex =
            static_cast<std::size_t>(layerNumber - 1) % kDefaultSolidPalette.size();
        (void)session_.addSolidLayer(tr("Solid %1").arg(layerNumber),
                                     kDefaultSolidPalette[paletteIndex]);
    });
    connect(addTextAction, &QAction::triggered, this, [this] {
        const auto layerNumber = nextLayerNumber(session_, document::kTextSourceNodeType,
                                                 document::kTextSourceNodeSchemaVersion);
        (void)session_.addTextLayer(tr("Text %1").arg(layerNumber), tr("Text"));
    });
    connect(undoButton_, &QToolButton::clicked, &session_, &CompositionSession::undo);
    connect(redoButton_, &QToolButton::clicked, &session_, &CompositionSession::redo);
    connect(playPauseButton_, &QToolButton::clicked, playback_, &PlaybackController::toggle);
    connect(playback_, &PlaybackController::stateChanged, this,
            &TimelineEditor::updatePlaybackButton);
    updatePlaybackButton(playback_->state());

    // Spacebar application shortcut (decision 4), gated against stealing Space from text-entry
    // focus using the SAME idiom main_window.cpp's own window-level shortcuts use (QAction +
    // setShortcutContext(Qt::WindowShortcut), e.g. undoAction_/redoAction_ in createMenus()):
    // Qt::WindowShortcut fires whenever this widget's top-level window is active, independent of
    // which descendant currently holds focus, EXCEPT that a focused text-entry widget (QLineEdit/
    // QTextEdit and friends) accepts the ShortcutOverride event for an ordinary printable key like
    // Space itself first, so the widget's own text-input handling wins over this action's shortcut
    // whenever a text field has focus -- the standard Qt mechanism for exactly this gating, not a
    // bespoke focus check.
    auto* playPauseAction = new QAction(tr("Play/Pause"), this);
    playPauseAction->setObjectName("playPauseAction");
    playPauseAction->setShortcut(QKeySequence(Qt::Key_Space));
    playPauseAction->setShortcutContext(Qt::WindowShortcut);
    addAction(playPauseAction);
    connect(playPauseAction, &QAction::triggered, playback_, &PlaybackController::toggle);

    // Frame-stepping shortcuts (issue #108, decisions 1/2), mirroring playPauseAction's own
    // WindowShortcut idiom exactly (same setObjectName/setShortcut/setShortcutContext/addAction
    // shape, same reliance on Qt's ShortcutOverride mechanism to let a focused text-entry widget
    // win over these shortcuts without any bespoke check here). This relied on PropertiesEditor's
    // Position X/Y editors being QDoubleSpinBox, whose embedded QLineEdit accepts ShortcutOverride
    // for Left/Right/Home/End. Issue #120 (task U5) replaced those editors with
    // kit::KValueField, which has no line edit and does NOT accept ShortcutOverride for those
    // keys -- so Left/Right/Home/End typed while a Position field has focus now ALSO fires these
    // WindowShortcut actions, unlike before. Task U5's own fence forbids touching TimelineEditor
    // beyond what shared code forces, so this is flagged in that task's raw report rather than
    // fixed here; the fix belongs to whoever next owns either kit::KValueField's key handling or
    // this focusChanged reconciliation below.
    stepBackwardAction_ = new QAction(tr("Step Back One Frame"), this);
    stepBackwardAction_->setObjectName("stepBackwardAction");
    stepBackwardAction_->setShortcut(QKeySequence(Qt::Key_Left));
    stepBackwardAction_->setShortcutContext(Qt::WindowShortcut);
    addAction(stepBackwardAction_);
    connect(stepBackwardAction_, &QAction::triggered, this, [this] { stepFrame(-1); });
    // Task U7 (issue #122), decision 5: the visible stepBackButton_ triggers this SAME action --
    // one behavior, two entry points (keyboard shortcut, mouse click) -- rather than a parallel
    // stepFrame(-1) call site.
    connect(stepBackButton_, &QToolButton::clicked, stepBackwardAction_, &QAction::trigger);

    stepForwardAction_ = new QAction(tr("Step Forward One Frame"), this);
    stepForwardAction_->setObjectName("stepForwardAction");
    stepForwardAction_->setShortcut(QKeySequence(Qt::Key_Right));
    stepForwardAction_->setShortcutContext(Qt::WindowShortcut);
    addAction(stepForwardAction_);
    connect(stepForwardAction_, &QAction::triggered, this, [this] { stepFrame(1); });
    connect(stepForwardButton_, &QToolButton::clicked, stepForwardAction_, &QAction::trigger);

    stepToStartAction_ = new QAction(tr("Go To Start"), this);
    stepToStartAction_->setObjectName("stepToStartAction");
    stepToStartAction_->setShortcut(QKeySequence(Qt::Key_Home));
    stepToStartAction_->setShortcutContext(Qt::WindowShortcut);
    addAction(stepToStartAction_);
    connect(stepToStartAction_, &QAction::triggered, this, &TimelineEditor::stepToStart);

    stepToEndAction_ = new QAction(tr("Go To End"), this);
    stepToEndAction_->setObjectName("stepToEndAction");
    stepToEndAction_->setShortcut(QKeySequence(Qt::Key_End));
    stepToEndAction_->setShortcutContext(Qt::WindowShortcut);
    addAction(stepToEndAction_);
    connect(stepToEndAction_, &QAction::triggered, this, &TimelineEditor::stepToEnd);

    // Arrow-key conflict finding (this task's own investigation, verified with a standalone Qt
    // harness before writing this code): layers_ (QTreeWidget) already consumes Left/Right/Home/End
    // itself -- Left/Right collapse/expand the current item, Home/End jump to the first/last item
    // -- but, unlike QLineEdit/QDoubleSpinBox, QAbstractItemView does NOT accept the
    // ShortcutOverride event for those keys. That means a WindowShortcut action bound to the same
    // key silently wins over the tree's OWN keyPressEvent()-based navigation the instant it exists:
    // the harness showed the tree's current item and expand state never change at all once a
    // same-key action is installed, with no error -- a genuine behavior change this task must not
    // ship silently. The reconciliation rule (frozen by this task): widget-focus wins; the step
    // action fires otherwise. Implemented by disabling these four actions outright while layers_
    // holds keyboard focus -- a disabled QAction never claims ShortcutOverride, so the key event
    // reaches layers_ and its native navigation runs completely unchanged; every other focus
    // location (including no focus, the ruler, and the keyframe panel) leaves the actions enabled
    // and the step fires. That "every other focus location" list no longer includes
    // PropertiesEditor's Position X/Y fields without a side effect -- see the stepBackwardAction_
    // construction comment above (issue #120, task U5): kit::KValueField does not win
    // Left/Right/Home/End via ShortcutOverride the way the QDoubleSpinBox it replaced did.
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget*, QWidget* now) {
        const bool layersFocused = (now == layers_);
        stepBackwardAction_->setEnabled(!layersFocused);
        stepForwardAction_->setEnabled(!layersFocused);
        stepToStartAction_->setEnabled(!layersFocused);
        stepToEndAction_->setEnabled(!layersFocused);
        // The visible stepBackButton_/stepForwardButton_ mirror their action's enabled state
        // exactly (task U7, issue #122, decision 5), so the same reconciliation this comment
        // block already documents for the keyboard shortcuts is visible on the mouse affordance
        // too rather than showing a clickable button that would silently do nothing.
        stepBackButton_->setEnabled(!layersFocused);
        stepForwardButton_->setEnabled(!layersFocused);
    });

    connect(layers_, &QTreeWidget::itemSelectionChanged, this, [this] {
        if (rebuilding_) {
            return;
        }
        const auto selected = layers_->selectedItems();
        if (selected.empty()) {
            session_.clearSelection();
            return;
        }
        session_.selectLayer(document::LayerId::fromRaw(
            selected.front()->data(0, kTimelineLayerIdRole).toULongLong()));
    });
    connect(&session_, &CompositionSession::snapshotChanged, this, &TimelineEditor::rebuild);
    connect(&session_, &CompositionSession::compositionChanged, this, &TimelineEditor::rebuild);
    connect(&session_, &CompositionSession::selectionChanged, this,
            &TimelineEditor::updateSelection);
    connect(&session_, &CompositionSession::historyChanged, this,
            &TimelineEditor::updateHistoryActions);
    // Readout updates on every session-time change and on a composition switch (which resets
    // session time to exact zero -- docs/architecture/animation-and-time.md, "Session Time And
    // Scrubbing": "Switching compositions resets the session time to exact zero in version 1"), so
    // the label always reflects the SAME time compositionChanged's reset already produced rather
    // than momentarily showing the previous composition's stale frame/time.
    connect(&session_, &CompositionSession::currentTimeChanged, this,
            &TimelineEditor::updateTimeReadout);
    connect(&session_, &CompositionSession::compositionChanged, this,
            &TimelineEditor::updateTimeReadout);

    rebuild();
    updateHistoryActions();
    updateTimeReadout();
}

void TimelineEditor::rebuild() {
    rebuilding_ = true;
    layers_->clear();
    const auto* composition = session_.composition();
    if (composition != nullptr) {
        for (const auto& entry : composition->graph().layerStack().entries()) {
            const QString name = layerName(*composition, entry.layerId);
            const QString kind = layerKind(session_, entry.layerId);

            // kNameColumn/kKindColumn stay exactly the pinned text() contract
            // (composition_projection_test.cpp, playback_controller_tests.cpp); the row's OTHER
            // columns are new (task U7, issue #122, decision 1) and carry no text of their own
            // (icon+tooltip or an embedded item widget instead), so this constructor still only
            // ever receives two strings.
            auto* row = new QTreeWidgetItem(layers_, {name, kind});
            row->setFont(kNameColumn, kit::font(kit::TypeRole::Ui));
            row->setForeground(kNameColumn, QBrush(kit::color(kit::Color::Foreground)));
            row->setFont(kKindColumn, kit::font(kit::TypeRole::Ui));
            row->setForeground(kKindColumn, QBrush(kit::color(kit::Color::Muted)));
            row->setData(0, kTimelineLayerIdRole,
                         QVariant::fromValue<qulonglong>(entry.layerId.value()));
            row->setData(0, kTimelineSlotIdRole,
                         QVariant::fromValue<qulonglong>(entry.slotId.value()));
            row->setToolTip(
                kNameColumn,
                tr("Layer %1 · Slot %2").arg(entry.layerId.value()).arg(entry.slotId.value()));

            // Visibility (decision 1): NO user-facing visibility command exists anywhere in
            // src/commands today (verified by reading src/commands/include/bloom/commands/
            // operations.hpp -- AddSolidLayer, AddTextLayer, SetParameterSource, MoveLayerBefore,
            // and the SetProjectName/SetCompositionName/SetCompositionDuration/
            // SetCompositionFormat document-settings ops are the complete list; no per-layer
            // visibility flag exists in the document model at all, layer_graph_model.md included).
            // The honesty rule therefore applies verbatim: a permanently dimmed, non-interactive
            // icon with a tooltip that says exactly why, never a toggle that would silently do
            // nothing. Reported in this task's raw report.
            row->setIcon(kVisibilityColumn,
                         QIcon(kit::iconPixmap(kit::IconId::Visible, kit::Size::IconSmall,
                                               kit::withOpacity(kit::color(kit::Color::Muted),
                                                                kit::kDisabledOpacity))));
            row->setToolTip(kVisibilityColumn,
                            tr("Layer visibility has no command yet -- every layer renders"));

            // Reserved audio-mute/solo/lock columns (decision 1) are deliberately OMITTED, not
            // rendered as disabled ghost icons: none of the three has ANY model presence today
            // (no audio-layer kind, no solo concept, no lock field anywhere in src/document), so
            // three permanently-inert icons crammed into this already-dense 34px row would read as
            // broken chrome rather than an honest "reserved" placeholder. Justification recorded in
            // this task's raw report per decision 1's own "omitted entirely... YOUR call,
            // justified" allowance.

            // Blending / Parent (decision 1): one always-disabled dropdown per row, each carrying
            // its single honest value.
            layers_->setItemWidget(row, kBlendingColumn,
                                   makeDisabledPlaceholderDropdown(
                                       tr("Normal"), tr("Blend modes are not implemented yet"),
                                       QStringLiteral("layerBlendingDropdown"), layers_));
            layers_->setItemWidget(row, kParentColumn,
                                   makeDisabledPlaceholderDropdown(
                                       tr("None"), tr("Layer parenting does not exist yet"),
                                       QStringLiteral("layerParentDropdown"), layers_));

            // Lane bar (decision 2): one type-color-coded bar per row, spanning this column's full
            // width (see TimelineLaneBar's own honesty disclosure comment above).
            layers_->setItemWidget(row, kLaneColumn,
                                   new TimelineLaneBar(layerLaneColorToken(), layers_));
        }
    }
    updateSelection();
    rebuilding_ = false;
}

void TimelineEditor::updateSelection() {
    const QSignalBlocker blocker(layers_);
    layers_->clearSelection();
    const auto* layerId = std::get_if<document::LayerId>(&session_.selection().primary);
    if (layerId == nullptr && session_.selection().contextualLayer.has_value()) {
        layerId = &*session_.selection().contextualLayer;
    }
    if (layerId == nullptr) {
        return;
    }
    for (int index = 0; index < layers_->topLevelItemCount(); ++index) {
        auto* row = layers_->topLevelItem(index);
        if (row->data(0, kTimelineLayerIdRole).toULongLong() == layerId->value()) {
            row->setSelected(true);
            layers_->scrollToItem(row);
            break;
        }
    }
}

void TimelineEditor::updateHistoryActions() {
    undoButton_->setEnabled(session_.canUndo());
    redoButton_->setEnabled(session_.canRedo());
    const QString undoLabel = session_.undoLabel();
    const QString redoLabel = session_.redoLabel();
    undoButton_->setToolTip(undoLabel.isEmpty() ? tr("Nothing to undo")
                                                : tr("Undo %1").arg(undoLabel));
    redoButton_->setToolTip(redoLabel.isEmpty() ? tr("Nothing to redo")
                                                : tr("Redo %1").arg(redoLabel));
}

void TimelineEditor::updatePlaybackButton(const PlaybackState state) {
    const bool playing = state == PlaybackState::Playing;
    playPauseButton_->setChecked(playing);
    // text()/isChecked() stay the pinned test contract verbatim (playback_controller_tests.cpp);
    // the icon swap (task U7, issue #122, decision 5: "Play/Pause swap") is purely additive.
    playPauseButton_->setText(playing ? tr("Pause") : tr("Play"));
    playPauseButton_->setIcon(
        kit::icon(playing ? kit::IconId::Pause : kit::IconId::Play, kit::Size::IconMedium));
    playPauseButton_->setIconSize(
        QSize(kit::px(kit::Size::IconMedium), kit::px(kit::Size::IconMedium)));
    playPauseButton_->setToolTip(playing ? tr("Pause playback (Space)")
                                         : tr("Play from the current time (Space)"));
}

void TimelineEditor::stepFrame(const int delta) {
    const auto context = frameContextFor(session_);
    if (!context.has_value()) {
        return;
    }
    const auto nearest =
        nearestFrameIndexForTime(context->frameRate, context->duration, session_.currentTime());
    if (!nearest.has_value()) {
        return;
    }
    // Stepping while playing pauses playback FIRST through PlaybackController's own public
    // transport API (design decision 1) -- composing with pause() explicitly here rather than
    // relying on handleCurrentTimeChanged()'s existing "any external setCurrentTime() while playing
    // pauses" side effect (playback_controller.cpp), so this call site is honest about what it does
    // and the transport state change is never a coincidental side effect of the time write below.
    // Called unconditionally (idempotent no-op if already Stopped -- PlaybackController::pause()'s
    // own documented guard), not only when the step actually moves the playhead, matching the
    // decision's "stepping... pauses playback FIRST" without making pausing conditional on the
    // clamp outcome.
    playback_->pause();

    // Left/Right move exactly one frame index from the nearest index to the CURRENT (possibly
    // subframe) time, clamped to [0, maxFrameIndex] (design decision 1). nearestFrameIndex()'s own
    // tie rule (bloom::core::FrameTimeMapping::nearestFrameIndex(), and docs/architecture/
    // animation-and-time.md's "Session Time And Scrubbing": "selects the nearest index with an
    // exact halfway tie going to the greater index") decides which frame a subframe time steps
    // from, not this call site.
    std::uint64_t target = *nearest;
    if (delta < 0) {
        target = target > 0 ? target - 1 : 0;
    } else {
        target = target < context->maxFrameIndexValue ? target + 1 : context->maxFrameIndexValue;
    }
    const auto targetTime = frameTimeForIndex(context->frameRate, context->duration, target);
    if (targetTime.has_value()) {
        // A clamped step that lands back on the CURRENT exact time (e.g. Left at frame 0) is a
        // true no-op through CompositionSession::setCurrentTime()'s own early-return-on-equal-time
        // guard (composition_session.cpp) -- no currentTimeChanged signal churn, verified by that
        // mutator's own implementation rather than re-checked here.
        (void)session_.setCurrentTime(*targetTime);
    }
}

void TimelineEditor::stepToStart() {
    const auto context = frameContextFor(session_);
    if (!context.has_value()) {
        return;
    }
    playback_->pause();
    const auto targetTime = frameTimeForIndex(context->frameRate, context->duration, 0);
    if (targetTime.has_value()) {
        (void)session_.setCurrentTime(*targetTime);
    }
}

void TimelineEditor::stepToEnd() {
    const auto context = frameContextFor(session_);
    if (!context.has_value()) {
        return;
    }
    playback_->pause();
    const auto targetTime =
        frameTimeForIndex(context->frameRate, context->duration, context->maxFrameIndexValue);
    if (targetTime.has_value()) {
        (void)session_.setCurrentTime(*targetTime);
    }
}

void TimelineEditor::updateTimeReadout() {
    const auto time = session_.currentTime();
    const auto context = frameContextFor(session_);
    QString frameText = QStringLiteral("—");
    if (context.has_value()) {
        const auto nearest = nearestFrameIndexForTime(context->frameRate, context->duration, time);
        if (nearest.has_value()) {
            frameText = QString::number(*nearest);
        }
    }
    timeReadout_->setText(tr("Frame %1 · %2").arg(frameText, formatExactSeconds(time)));
}

PropertiesEditor::PropertiesEditor(CompositionSession& session, QWidget* parent)
    : QWidget(parent), session_(session) {
    setObjectName("propertiesEditor");
    setAccessibleName(tr("Properties editor"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(kit::px(kit::Spacing::M), kit::px(kit::Spacing::M),
                               kit::px(kit::Spacing::M), kit::px(kit::Spacing::M));
    layout->setSpacing(kit::px(kit::Spacing::S));

    selectionLabel_ = new QLabel(this);
    selectionLabel_->setObjectName("propertiesSelectionTitle");
    selectionLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(selectionLabel_);

    // Issue #120 (task U5), decision 1: the kit field grid. Every row's label lives in ONE fixed,
    // right-aligned column shared across the whole panel -- Transform, Appearance, the
    // source-specific group, and the no-selection Composition group alike -- computed once from
    // the widest label text this panel can ever show (propertyLabelColumnWidth() above), not from
    // a spelled pixel width.
    const int labelColumnWidth = propertyLabelColumnWidth();

    // --- Selection-driven groups (Transform / Appearance / source-specific) --------------------
    selectionSection_ = new QWidget(this);
    selectionSection_->setObjectName(QStringLiteral("propertiesSelectionSection"));
    auto* selectionLayout = new QVBoxLayout(selectionSection_);
    selectionLayout->setContentsMargins(0, 0, 0, 0);
    selectionLayout->setSpacing(kit::px(kit::Spacing::XS));

    addSectionHeader(selectionLayout, selectionSection_, tr("Transform"));

    positionX_ = new kit::KValueField(selectionSection_);
    positionY_ = new kit::KValueField(selectionSection_);
    for (auto* field : {positionX_, positionY_}) {
        field->setRange(-1'000'000.0, 1'000'000.0);
        field->setDecimals(2);
        field->setSingleStep(1.0);
        field->setUnit(QStringLiteral("px"));
    }
    positionX_->setObjectName("positionXEditor");
    positionX_->setAccessibleName(tr("Position X"));
    positionX_->setLabel(QStringLiteral("X"));
    positionY_->setObjectName("positionYEditor");
    positionY_->setAccessibleName(tr("Position Y"));
    positionY_->setLabel(QStringLiteral("Y"));

    auto* positionFields = new QWidget(selectionSection_);
    positionFields->setObjectName(QStringLiteral("positionFieldGroup"));
    auto* positionFieldsLayout = new QHBoxLayout(positionFields);
    positionFieldsLayout->setContentsMargins(0, 0, 0, 0);
    positionFieldsLayout->setSpacing(kit::px(kit::Spacing::S));
    positionFieldsLayout->addWidget(positionX_);
    positionFieldsLayout->addWidget(positionY_);

    positionKeyframe_ = makeKeyframeIndicator(selectionSection_);
    addPropertyRow(selectionLayout, selectionSection_,
                   makePropertyRowLabel(tr("Position"), labelColumnWidth, selectionSection_),
                   positionKeyframe_, positionFields);

    addSectionHeader(selectionLayout, selectionSection_, tr("Appearance"));

    opacity_ = new kit::KValueField(selectionSection_);
    opacity_->setObjectName("opacityEditor");
    opacity_->setAccessibleName(tr("Opacity"));
    opacity_->setRange(0.0, 100.0);
    opacity_->setDecimals(1);
    opacity_->setSingleStep(1.0);
    opacity_->setUnit(QStringLiteral("%"));

    opacityKeyframe_ = makeKeyframeIndicator(selectionSection_);
    addPropertyRow(selectionLayout, selectionSection_,
                   makePropertyRowLabel(tr("Opacity"), labelColumnWidth, selectionSection_),
                   opacityKeyframe_, opacity_);

    solidColorPanel_ = new QWidget(selectionSection_);
    solidColorPanel_->setObjectName("solidColorProperties");
    auto* solidColorLayout = new QVBoxLayout(solidColorPanel_);
    solidColorLayout->setContentsMargins(0, 0, 0, 0);
    solidColorLayout->setSpacing(kit::px(kit::Spacing::XS));
    addSectionHeader(solidColorLayout, solidColorPanel_, tr("Solid Source"));

    solidColorValue_ = makeReadOnlyValueLabel(kit::TypeRole::Value, solidColorPanel_);
    solidColorValue_->setObjectName("solidColorValue");
    solidColorValue_->setAccessibleName(tr("Solid RGBA value"));
    solidColorValue_->setWordWrap(true);
    solidColorKeyframe_ = makeKeyframeIndicator(solidColorPanel_);
    addPropertyRow(solidColorLayout, solidColorPanel_,
                   makePropertyRowLabel(tr("RGBA"), labelColumnWidth, solidColorPanel_),
                   solidColorKeyframe_, solidColorValue_);

    solidAlphaAssociation_ = makeReadOnlyValueLabel(kit::TypeRole::Ui, solidColorPanel_);
    solidAlphaAssociation_->setObjectName("solidAlphaAssociation");
    solidAlphaAssociation_->setAccessibleName(tr("Solid alpha association"));
    addPropertyRow(solidColorLayout, solidColorPanel_,
                   makePropertyRowLabel(tr("Alpha"), labelColumnWidth, solidColorPanel_), nullptr,
                   solidAlphaAssociation_);

    solidColorEncoding_ = makeReadOnlyValueLabel(kit::TypeRole::Ui, solidColorPanel_);
    solidColorEncoding_->setObjectName("solidColorEncoding");
    solidColorEncoding_->setAccessibleName(tr("Solid color encoding"));
    addPropertyRow(solidColorLayout, solidColorPanel_,
                   makePropertyRowLabel(tr("Encoding"), labelColumnWidth, solidColorPanel_),
                   nullptr, solidColorEncoding_);

    selectionLayout->addWidget(solidColorPanel_);
    selectionLayout->addStretch(1);
    layout->addWidget(selectionSection_);

    // --- No-selection document/composition view (decision 3) ----------------------------------
    documentSection_ = new QWidget(this);
    documentSection_->setObjectName(QStringLiteral("propertiesDocumentSection"));
    auto* documentLayout = new QVBoxLayout(documentSection_);
    documentLayout->setContentsMargins(0, 0, 0, 0);
    documentLayout->setSpacing(kit::px(kit::Spacing::XS));
    addSectionHeader(documentLayout, documentSection_, tr("Composition"));

    documentName_ = makeReadOnlyValueLabel(kit::TypeRole::Ui, documentSection_);
    documentName_->setObjectName(QStringLiteral("documentName"));
    documentName_->setAccessibleName(tr("Composition name"));
    addPropertyRow(documentLayout, documentSection_,
                   makePropertyRowLabel(tr("Name"), labelColumnWidth, documentSection_), nullptr,
                   documentName_);

    documentFormat_ = makeReadOnlyValueLabel(kit::TypeRole::Value, documentSection_);
    documentFormat_->setObjectName(QStringLiteral("documentFormat"));
    documentFormat_->setAccessibleName(tr("Composition format"));
    addPropertyRow(documentLayout, documentSection_,
                   makePropertyRowLabel(tr("Format"), labelColumnWidth, documentSection_), nullptr,
                   documentFormat_);

    documentFrameRate_ = makeReadOnlyValueLabel(kit::TypeRole::Value, documentSection_);
    documentFrameRate_->setObjectName(QStringLiteral("documentFrameRate"));
    documentFrameRate_->setAccessibleName(tr("Composition frame rate"));
    addPropertyRow(documentLayout, documentSection_,
                   makePropertyRowLabel(tr("Frame Rate"), labelColumnWidth, documentSection_),
                   nullptr, documentFrameRate_);

    documentDuration_ = makeReadOnlyValueLabel(kit::TypeRole::Value, documentSection_);
    documentDuration_->setObjectName(QStringLiteral("documentDuration"));
    documentDuration_->setAccessibleName(tr("Composition duration"));
    addPropertyRow(documentLayout, documentSection_,
                   makePropertyRowLabel(tr("Duration"), labelColumnWidth, documentSection_),
                   nullptr, documentDuration_);

    documentPixelAspect_ = makeReadOnlyValueLabel(kit::TypeRole::Value, documentSection_);
    documentPixelAspect_->setObjectName(QStringLiteral("documentPixelAspect"));
    documentPixelAspect_->setAccessibleName(tr("Composition pixel aspect ratio"));
    addPropertyRow(documentLayout, documentSection_,
                   makePropertyRowLabel(tr("Pixel Aspect"), labelColumnWidth, documentSection_),
                   nullptr, documentPixelAspect_);

    // Color settings (process space + config name) are read from ProjectSession, not from
    // anything CompositionSession exposes (src/host/include/bloom/host/project_session.hpp) --
    // document::Composition/Snapshot carry no ColorSettings at all (verified: grep finds
    // ColorSettings only under src/host and src/project, never src/document). Per decision 3 ("if
    // a listed fact is not reachable via existing read-only API, omit it and report rather than
    // adding API"), the color settings summary row is omitted here; see this task's raw report.

    documentLayout->addStretch(1);
    layout->addWidget(documentSection_);

    const auto commitPosition = [this] {
        if (!rebuilding_) {
            (void)session_.setSelectedPosition(positionX_->value(), positionY_->value());
        }
    };
    connect(positionX_, &kit::KValueField::valueChanged, this, commitPosition);
    connect(positionY_, &kit::KValueField::valueChanged, this, commitPosition);
    connect(opacity_, &kit::KValueField::valueChanged, this, [this](const double value) {
        if (!rebuilding_) {
            (void)session_.setSelectedOpacity(value / 100.0);
        }
    });
    connect(&session_, &CompositionSession::snapshotChanged, this, &PropertiesEditor::rebuild);
    connect(&session_, &CompositionSession::compositionChanged, this, &PropertiesEditor::rebuild);
    connect(&session_, &CompositionSession::selectionChanged, this, &PropertiesEditor::rebuild);

    rebuild();
}

void PropertiesEditor::rebuild() {
    rebuilding_ = true;
    selectionLabel_->setText(selectionName(session_));

    configurePosition();
    configureOpacity();
    configureSolidColor();
    configureDocumentProperties();
    rebuilding_ = false;
}

void PropertiesEditor::configurePosition() {
    const auto* position = session_.parameterForSelection(document::kPositionParameterRole);
    const auto positionValue =
        position == nullptr ? std::nullopt : session_.constantVec2Value(position->id);
    const bool canEditPosition = positionValue.has_value();
    positionX_->setEnabled(canEditPosition);
    positionY_->setEnabled(canEditPosition);
    if (canEditPosition) {
        const QSignalBlocker blockX(positionX_);
        const QSignalBlocker blockY(positionY_);
        positionX_->setValue(positionValue->x);
        positionY_->setValue(positionValue->y);
    }
    const QString positionTip = position == nullptr
                                    ? tr("Position is not exposed by this selection")
                                    : sourceDescription(*position);
    positionX_->setToolTip(positionTip);
    positionY_->setToolTip(positionTip);
    updateKeyframeIndicator(positionKeyframe_, position);
}

void PropertiesEditor::configureOpacity() {
    const auto* parameter = session_.parameterForSelection(document::kOpacityParameterRole);
    const auto value = parameter == nullptr ? std::nullopt : session_.constantValue(parameter->id);
    opacity_->setEnabled(value.has_value());
    const QSignalBlocker blocker(opacity_);
    opacity_->setValue(value.has_value() ? *value * 100.0 : 100.0);
    opacity_->setToolTip(parameter == nullptr ? tr("Opacity is not exposed by this selection")
                                              : sourceDescription(*parameter));
    updateKeyframeIndicator(opacityKeyframe_, parameter);
}

void PropertiesEditor::configureSolidColor() {
    const auto* parameter = session_.parameterForSelection(document::kSolidColorParameterRole);
    const auto* sourceNode = selectedPresentationSource(session_);
    const bool isSolid = isKnownSource(sourceNode, document::kSolidSourceNodeType,
                                       document::kSolidSourceNodeSchemaVersion) &&
                         parameter != nullptr &&
                         parameter->schemaKey == document::kSolidColorParameterSchemaKey;
    solidColorPanel_->setVisible(isSolid);
    if (!isSolid) {
        return;
    }

    updateKeyframeIndicator(solidColorKeyframe_, parameter);
    const auto value = session_.constantColorValue(parameter->id);
    solidColorValue_->setText(value.has_value() ? exactColor(*value)
                                                : sourceDescription(*parameter));
    solidColorValue_->setToolTip(
        tr("Straight scene-linear authoring values; negative and HDR RGB are not clipped"));
    solidAlphaAssociation_->setText(tr("Straight (unassociated)"));
    solidColorEncoding_->setText(
        QString::fromUtf8(document::kSolidColorEncoding.data(),
                          static_cast<qsizetype>(document::kSolidColorEncoding.size())));
}

void PropertiesEditor::configureDocumentProperties() {
    const auto* composition = session_.composition();
    const bool hasSelection = !std::holds_alternative<std::monostate>(session_.selection().primary);
    const bool showDocument = composition != nullptr && !hasSelection;
    documentSection_->setVisible(showDocument);
    selectionSection_->setVisible(!showDocument);
    if (!showDocument) {
        return;
    }

    documentName_->setText(QString::fromStdString(composition->name()));
    const auto format = composition->format();
    documentFormat_->setText(formatCompositionFormat(format));
    documentFrameRate_->setText(formatFrameRate(format.frameRate()));
    documentPixelAspect_->setText(formatPixelAspect(format.pixelAspect()));
    const auto context = frameContextFor(session_);
    documentDuration_->setText(context.has_value() ? formatDuration(*context)
                                                   : QStringLiteral("—"));
}

MediaEditor::MediaEditor(CompositionSession& session, QWidget* parent)
    : QWidget(parent), session_(session) {
    setObjectName("mediaEditor");
    setAccessibleName(tr("Project media editor"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(7);
    auto* title = new QLabel(tr("Project"), this);
    title->setObjectName("editorSectionTitle");
    compositions_ = new QListWidget(this);
    compositions_->setObjectName("compositionList");
    compositions_->setAccessibleName(tr("Project compositions"));
    compositions_->setAlternatingRowColors(true);
    layout->addWidget(title);
    layout->addWidget(compositions_, 1);

    connect(compositions_, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem* current) {
                if (!rebuilding_ && current != nullptr) {
                    (void)session_.setComposition(document::CompositionId::fromRaw(
                        current->data(kCompositionIdRole).toULongLong()));
                }
            });
    connect(&session_, &CompositionSession::snapshotChanged, this, &MediaEditor::rebuild);
    connect(&session_, &CompositionSession::compositionChanged, this,
            &MediaEditor::updateSelection);

    rebuild();
}

void MediaEditor::rebuild() {
    rebuilding_ = true;
    compositions_->clear();
    for (const auto& composition : session_.snapshot().project().compositions()) {
        auto* item = new QListWidgetItem(QString::fromStdString(composition.name()), compositions_);
        item->setData(kCompositionIdRole,
                      QVariant::fromValue<qulonglong>(composition.id().value()));
        item->setToolTip(tr("Composition %1").arg(composition.id().value()));
    }
    updateSelection();
    rebuilding_ = false;
}

void MediaEditor::updateSelection() {
    const QSignalBlocker blocker(compositions_);
    for (int index = 0; index < compositions_->count(); ++index) {
        auto* item = compositions_->item(index);
        if (item->data(kCompositionIdRole).toULongLong() == session_.compositionId().value()) {
            compositions_->setCurrentItem(item);
            return;
        }
    }
    compositions_->setCurrentItem(nullptr);
}

} // namespace bloom::ui
