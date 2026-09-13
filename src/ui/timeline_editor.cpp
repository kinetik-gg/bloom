#include <bloom/ui/timeline_editor.hpp>

#include "composition_editor_support.hpp"

#include <bloom/ui/composition_authoring.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/ram_preview_controller.hpp>
#include <bloom/ui/timeline_frame_math.hpp>
#include <bloom/ui/timeline_ruler.hpp>

#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/icons.hpp>
#include <bloom/ui/kit/painting.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <bloom/core/blend_mode.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/project.hpp>

#include <QAction>
#include <QApplication>
#include <QEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QIcon>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QResizeEvent>
#include <QScrollBar>
#include <QSize>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>
#include <QVariant>
#include <QWheelEvent>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <utility>
#include <variant>

namespace bloom::ui {
namespace {

// ---------------------------------------------------------------------------------------------
// The LEFT column's cell table (task T1).
//
// One table, read by the column-header row AND by every layer row, so a header can never drift out
// of alignment with the cells underneath it. Every width resolves from a kit token -- tokens.hpp
// forbids a raw pixel gap outright -- and the column's own fixed width is their exact sum, which is
// also the x origin of the ruler, of every lane, and of the work-area strip.
constexpr int kCellGap = kit::px(kit::Spacing::XS);
constexpr int kColumnPadding = kit::px(kit::Spacing::XS);
constexpr int kToggleCellWidth = kit::px(kit::Size::IconMedium) + kit::px(kit::Spacing::XS);
constexpr int kToggleCellCount = 4;
constexpr int kNameCellWidth = kit::px(kit::Size::ControlRoomy) * 6;
constexpr int kBlendingCellWidth = kit::px(kit::Size::ControlRoomy) * 3;
constexpr int kParentCellWidth = kit::px(kit::Size::ControlRoomy) * 3;

constexpr int kToggleStripX = kColumnPadding;
constexpr int kNameCellX = kToggleStripX + kToggleCellCount * kToggleCellWidth + kCellGap;
constexpr int kBlendingCellX = kNameCellX + kNameCellWidth + kCellGap;
constexpr int kParentCellX = kBlendingCellX + kBlendingCellWidth + kCellGap;
constexpr int kLayerColumnWidthPx = kParentCellX + kParentCellWidth + kColumnPadding;

// The scroll gutter reserved to the right of the lane region. It is the scrollbar's HOVER extent,
// not its resting one: the kit stylesheet grows a hovered vertical scrollbar from Size::ScrollBar
// to Size::ScrollBarHover, and if that growth came out of the lane region's own width the ruler
// above it would stop agreeing with the lanes about where a frame is the instant the pointer
// touched the scrollbar. Reserving the larger extent once means the time axis never moves.
constexpr int kScrollGutterWidth = kit::px(kit::Size::ScrollBarHover);

[[nodiscard]] int toggleCellX(const int index) { return kToggleStripX + index * kToggleCellWidth; }

// ---------------------------------------------------------------------------------------------
// The four per-layer toggle columns.
//
// HONESTY DISCLOSURE (task T1's raw report): all four are painted permanently DISABLED, with a
// tooltip naming the exact reason, because not one of the four features exists. Verified by reading
// src/commands/include/bloom/commands/operations.hpp and animation_operations.hpp -- AddSolidLayer,
// AddTextLayer, SetProjectName, SetCompositionName, SetCompositionDuration, SetCompositionFormat,
// SetParameterSource, MoveLayerBefore and the animation-curve operations are the complete command
// vocabulary -- and src/document, which has no per-layer visibility, audio, solo, or lock field
// anywhere (document::LayerStackEntry is a slot id and a layer id, nothing else). The task package
// describes the eye as wired to an "existing command"; there is no such command in this repository,
// so the repository's own honesty rule applies verbatim: a permanently dimmed, non-interactive
// glyph that says why, never a toggle that would silently do nothing.
enum class ToggleCell : int { Visibility = 0, Audio = 1, Solo = 2, Lock = 3 };

[[nodiscard]] kit::IconId toggleIcon(const int index) {
    switch (static_cast<ToggleCell>(index)) {
    case ToggleCell::Visibility:
        return kit::IconId::Visible;
    case ToggleCell::Audio:
        return kit::IconId::AudioOn;
    case ToggleCell::Solo:
        // KIT GAP, disclosed: kit::IconId has no solo glyph (the vocabulary is fixed in
        // kit/icons.hpp and kit edits are outside this task's fence). Check is the closest existing
        // primitive -- the vocabulary's generic "this one is marked" mark -- and it is the only one
        // that carries no competing meaning inside this panel: Keyframe's diamond is the key marker
        // two rows down, Select is a pointer, and Info/Error are status glyphs.
        return kit::IconId::Check;
    case ToggleCell::Lock:
        return kit::IconId::Locked;
    }
    return kit::IconId::Visible;
}

[[nodiscard]] QString toggleToolTip(const int index) {
    switch (static_cast<ToggleCell>(index)) {
    case ToggleCell::Visibility:
        return TimelineEditor::tr("Layer visibility has no command yet -- every layer renders");
    case ToggleCell::Audio:
        return TimelineEditor::tr("There is no audio layer kind yet, so there is nothing to mute");
    case ToggleCell::Solo:
        return TimelineEditor::tr("Soloing a layer does not exist yet");
    case ToggleCell::Lock:
        return TimelineEditor::tr("Locking a layer does not exist yet");
    }
    return {};
}

// The tooltip for whatever cell `x` lands in, or an empty string for the cells that need none.
[[nodiscard]] QString cellToolTipAt(const int x) {
    for (int index = 0; index < kToggleCellCount; ++index) {
        const int left = toggleCellX(index);
        if (x >= left && x < left + kToggleCellWidth) {
            return toggleToolTip(index);
        }
    }
    return {};
}

// ---------------------------------------------------------------------------------------------

[[nodiscard]] QString layerKind(const CompositionSession& session,
                                const document::LayerId layerId) {
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

// The clip bar's fill, from the data-type palette (task T1).
//
// Task T1 shipped ONE color for both kinds and disclosed that as a blocked sub-item: with the Kind
// column removed, kind was supposed to be expressed by the clip color, but every role in the
// data-type palette names a kind of REFERENCED MEDIA (docs/ux/visual-language.md: image sequences,
// clips, compositions, still images, audio), and Bloom has no media import pipeline at all -- its
// only kinds are Solid and Text, both generated in-project. Task S3 makes text a real rendering
// layer kind, so the two kinds now need to be distinguishable on the lane, and this is the choice:
//
//   Solid -> DataComposition (#8B5CF6), unchanged, so no existing clip changes color.
//   Text  -> DataClip (#3FBF6B).
//
// Why DataClip, having rejected the others on their own terms:
//   * DataImage (#3AA5F0) is byte-identical to AccentHover and one step from Accent (#0C8CE9), so a
//     clip bar painted with it reads as an accent/selected surface and swallows the 1px Accent
//     playhead crossing it.
//   * DataSequence (#E0554E) is byte-identical to Error, so a text clip would read as a failed one.
//   * DataAudio (#7C5CFF) is a neighbouring purple to DataComposition's #8B5CF6 -- two kinds that
//     are supposed to be told apart at a glance would not be.
//   * DataClip is byte-identical to Ok (#3FBF6B), which is the one remaining collision, and a green
//     clip bar reading as "ready" is a far smaller misstatement than one reading as "error",
//     "selected", or "the same kind as a solid".
// Adding a DataText role to the kit would be the ideal fix and remains a kit-owner decision; it is
// not needed for the two kinds that exist.
//
// Kind is still named in text as well: TimelineLayerStack::toolTipAt() spells it on every row, so
// the color is a second channel rather than the only one. An unrecognized layer kind takes Muted,
// because there is no honest data-type color for "kind unknown". A future media-backed or
// pre-composition layer kind takes its own role here on the day it ships.
[[nodiscard]] kit::Color layerClipColorToken(const CompositionSession& session,
                                             const document::LayerId layerId) {
    const auto* sourceNode = directSourceNode(session, layerId);
    if (isKnownSource(sourceNode, document::kSolidSourceNodeType,
                      document::kSolidSourceNodeSchemaVersion)) {
        return kit::Color::DataComposition;
    }
    if (isKnownSource(sourceNode, document::kTextSourceNodeType,
                      document::kTextSourceNodeSchemaVersion)) {
        return kit::Color::DataClip;
    }
    return kit::Color::Muted;
}

// The layer the session currently points at: the primary selection when it IS a layer, otherwise
// the contextual layer a node/parameter selection carries. Exactly the resolution order the
// QTreeWidget stack used before this task.
[[nodiscard]] std::optional<document::LayerId> selectedLayer(const CompositionSession& session) {
    if (const auto* direct = std::get_if<document::LayerId>(&session.selection().primary)) {
        return *direct;
    }
    return session.selection().contextualLayer;
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
// break every existing findChild<QToolButton*>("playPauseButton") test contract. An icon never
// replaces an accessible name (ADR 0010; docs/ux/visual-language.md's Iconography section), so
// every call site still sets both a tooltip and setAccessibleName().
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
    // task U8, issue #131, fix 7: an icon-only QToolButton sizes to controlExtent x controlExtent
    // exactly, the same square target kit::KButton's own icon-only sizeHint() pins.
    button->setFixedSize(kit::px(kit::Size::Control), kit::px(kit::Size::Control));
    return button;
}

// Parent: one always-disabled KDropdown carrying its single honest value ("None"). No parenting
// feature exists in the document model or the command vocabulary, so there is nothing else to
// offer, and the tooltip says so rather than the control merely looking unresponsive. Compact
// control size so a real dropdown fits the 32px row.
//
// Blending is no longer one of these: a layer's blend mode is a real Layer Output parameter with a
// real command behind it, so that dropdown is built by makeBlendingDropdown() below instead.
kit::KDropdown* makeDisabledPlaceholderDropdown(const QString& value, const QString& toolTip,
                                                const QString& objectName, QWidget* parent) {
    auto* dropdown = new kit::KDropdown(parent);
    dropdown->setObjectName(objectName);
    dropdown->setControlSize(kit::KDropdown::ControlSize::Compact);
    dropdown->addItem(value);
    dropdown->setEnabled(false);
    dropdown->setToolTip(toolTip);
    return dropdown;
}

// The Blending dropdown: every implemented blend mode, in core::kBlendModes order, named by the one
// shared vocabulary blendModeDisplayName() owns. The item DATA is the mode's stored integer rather
// than its row index, so the control never depends on the order it happened to be filled in.
kit::KDropdown* makeBlendingDropdown(QWidget* parent) {
    auto* dropdown = new kit::KDropdown(parent);
    dropdown->setObjectName(QStringLiteral("layerBlendingDropdown"));
    dropdown->setAccessibleName(TimelineEditor::tr("Blending"));
    dropdown->setControlSize(kit::KDropdown::ControlSize::Compact);
    for (const auto mode : core::kBlendModes) {
        dropdown->addItem(blendModeDisplayName(mode),
                          QVariant::fromValue(core::blendModeStoredValue(mode)));
    }
    return dropdown;
}

// Which row of a blending dropdown shows `mode`, found by stored value rather than by assuming the
// fill order.
[[nodiscard]] int blendingDropdownIndex(const kit::KDropdown& dropdown,
                                        const core::BlendMode mode) {
    const auto stored = core::blendModeStoredValue(mode);
    for (int index = 0; index < dropdown.count(); ++index) {
        if (dropdown.itemData(index).value<std::int64_t>() == stored) {
            return index;
        }
    }
    return -1;
}

// The hairline that closes every row, in both halves of the grid: rows are FLAT (no striping at all
// -- task T1 replaces task U7's surface ladder), so the separator is the only thing giving the grid
// its rhythm.
void paintRowSeparator(QPainter& painter, const int top, const int widthPixels) {
    kit::applyHairlinePen(painter, kit::color(kit::Color::Border));
    const auto y = static_cast<qreal>(top + kTimelineRowHeight) - 0.5;
    painter.drawLine(QPointF(0.0, y), QPointF(static_cast<qreal>(widthPixels), y));
}

// The selected row's own fill, in both halves. SurfaceRaised FILL rather than a BorderActive
// outline (the task allows either): a fill is one rectangle per half that reads as a single
// continuous row across the column/lane boundary, where an outline would have to be stitched out of
// three edges in one widget and three in the other and would break wherever the two halves' widths
// disagreed.
void paintSelectedRowFill(QPainter& painter, const int top, const int widthPixels) {
    painter.fillRect(QRect(0, top, widthPixels, kTimelineRowHeight),
                     kit::color(kit::Color::SurfaceRaised));
}

} // namespace

// ---------------------------------------------------------------------------------------------
// One pooled row of the left layer-stack column.
//
// Deliberately not exposed via the header -- TimelineLayerStack owns it exclusively and
// forward-declares it (`class TimelineLayerRow*`) purely to type its pool -- so this definition
// must live directly in bloom::ui rather than in an anonymous namespace (which would make it a
// distinct, unrelated type from that forward declaration). It declares no Q_OBJECT: it has no
// signals, and it never connects to anything, so it stays free of moc, exactly like
// TimelineKeyframeRow.
//
// It is WA_TransparentForMouseEvents: selection, keyboard navigation, and tooltips all belong to
// the column as a whole, so there is exactly one hit-test and one tooltip table instead of one per
// row. The attribute applies only to this widget, never to its children, so its two dropdowns still
// receive their own events.
//
// It declares no Q_OBJECT: it emits nothing. It does CONNECT its blending dropdown to a lambda, but
// as the connection's context object rather than as a sender, which needs only QObject -- which
// QWidget already is -- so the row stays free of moc exactly as TimelineKeyframeRow does.
class TimelineLayerRow final : public QWidget {
  public:
    TimelineLayerRow(CompositionSession& session, QWidget* parent)
        : QWidget(parent), session_(&session) {
        setObjectName(QStringLiteral("timelineLayerRow"));
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
        setFixedHeight(kTimelineRowHeight);
        blending_ = makeBlendingDropdown(this);
        parentDropdown_ = makeDisabledPlaceholderDropdown(
            TimelineEditor::tr("None"), TimelineEditor::tr("Layer parenting does not exist yet"),
            QStringLiteral("layerParentDropdown"), this);
        // The connection is made once, for the life of the pooled row, and reads whichever layer
        // the row is bound to AT THE MOMENT the artist picks a mode -- a pooled row is re-pointed
        // on every scroll step, so capturing a layer id here would author the wrong layer.
        connect(blending_, &kit::KDropdown::currentIndexChanged, this, [this](const int index) {
            if (binding_ || !layerId_.has_value() || index < 0) {
                return;
            }
            const auto mode =
                core::blendModeFromStoredValue(blending_->itemData(index).value<std::int64_t>());
            if (!mode.has_value()) {
                return;
            }
            (void)session_->setLayerBlendMode(*layerId_, *mode);
        });
    }

    // Re-points this pooled row at another layer. No widget is created or destroyed and no layout
    // is invalidated -- only the painted content, the bound layer, and the two dropdowns' geometry,
    // which is why a composition with hundreds of layers costs the same handful of widgets as one
    // with three.
    void bind(const TimelineLayerEntry& entry, const bool selected) {
        name_ = entry.name;
        selected_ = selected;
        layerId_ = entry.layerId;
        // `binding_` (not just a QSignalBlocker) because setCurrentIndex() is a projection of
        // document truth, never an edit: a blocked signal would still leave the lambda armed for a
        // nested change, and a row re-pointed during a scroll must author nothing at all.
        binding_ = true;
        const auto mode = session_->blendModeForLayer(entry.layerId);
        const int row = mode.has_value() ? blendingDropdownIndex(*blending_, *mode) : -1;
        blending_->setEnabled(mode.has_value());
        blending_->setCurrentIndex(row >= 0 ? row : 0);
        blending_->setToolTip(mode.has_value()
                                  ? TimelineEditor::tr("How this layer combines with the layers "
                                                       "beneath it")
                                  : TimelineEditor::tr("This layer does not expose a blend mode"));
        binding_ = false;
        update();
    }

  protected:
    void resizeEvent(QResizeEvent* event) override {
        QWidget::resizeEvent(event);
        // Fixed cell geometry, assigned directly rather than through a nested layout: a row is a
        // fixed table of cells, and a QHBoxLayout per row would re-solve that same table on every
        // bind and every scroll step for no gain.
        const int dropdownHeight = std::min(height(), blending_->sizeHint().height());
        const int top = (height() - dropdownHeight) / 2;
        blending_->setGeometry(kBlendingCellX, top, kBlendingCellWidth, dropdownHeight);
        parentDropdown_->setGeometry(kParentCellX, top, kParentCellWidth, dropdownHeight);
    }

    void paintEvent(QPaintEvent* event) override {
        Q_UNUSED(event)
        QPainter painter(this);
        // Flat rows: no alternating stripe at all. The selected row takes SurfaceRaised.
        painter.fillRect(
            rect(), kit::color(selected_ ? kit::Color::SurfaceRaised : kit::Color::Background));
        paintRowSeparator(painter, 0, width());

        // The four reserved toggle glyphs, every one of them disabled ink (see toggleToolTip()).
        for (int index = 0; index < kToggleCellCount; ++index) {
            const auto glyph = kit::iconPixmap(toggleIcon(index), kit::Size::IconSmall,
                                               kit::Color::Muted, kit::State::Disabled);
            const int glyphExtent = kit::px(kit::Size::IconSmall);
            const int x = toggleCellX(index) + (kToggleCellWidth - glyphExtent) / 2;
            const int y = (height() - glyphExtent) / 2;
            painter.drawPixmap(QRect(x, y, glyphExtent, glyphExtent), glyph);
        }

        painter.setFont(kit::font(kit::TypeRole::Ui));
        painter.setPen(kit::color(kit::Color::Foreground));
        const QRect nameRect(kNameCellX, 0, kNameCellWidth, height());
        const QFontMetrics metrics = painter.fontMetrics();
        painter.drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter,
                         metrics.elidedText(name_, Qt::ElideRight, nameRect.width()));
    }

  private:
    CompositionSession* session_ = nullptr;
    QString name_;
    std::optional<document::LayerId> layerId_;
    bool selected_ = false;
    bool binding_ = false;
    kit::KDropdown* blending_ = nullptr;
    kit::KDropdown* parentDropdown_ = nullptr;
};

// ---------------------------------------------------------------------------------------------

TimelineColumnHeaders::TimelineColumnHeaders(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("timelineColumnHeaders"));
    setAccessibleName(tr("Layer columns"));
    setFixedWidth(kLayerColumnWidthPx);
    setFixedHeight(kit::px(kit::Size::Control));
}

void TimelineColumnHeaders::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event)
    QPainter painter(this);
    painter.fillRect(rect(), kit::color(kit::Color::Surface));
    kit::applyHairlinePen(painter, kit::color(kit::Color::Border));
    painter.drawLine(QPointF(0.0, static_cast<qreal>(height()) - 0.5),
                     QPointF(static_cast<qreal>(width()), static_cast<qreal>(height()) - 0.5));

    for (int index = 0; index < kToggleCellCount; ++index) {
        const auto glyph = kit::iconPixmap(toggleIcon(index), kit::Size::IconSmall,
                                           kit::Color::Muted, kit::State::Disabled);
        const int glyphExtent = kit::px(kit::Size::IconSmall);
        const int x = toggleCellX(index) + (kToggleCellWidth - glyphExtent) / 2;
        const int y = (height() - glyphExtent) / 2;
        painter.drawPixmap(QRect(x, y, glyphExtent, glyphExtent), glyph);
    }

    painter.setFont(kit::font(kit::TypeRole::UiSmall));
    painter.setPen(kit::color(kit::Color::Muted));
    painter.drawText(QRect(kNameCellX, 0, kNameCellWidth, height()),
                     Qt::AlignLeft | Qt::AlignVCenter, tr("Name"));
    painter.drawText(QRect(kBlendingCellX, 0, kBlendingCellWidth, height()),
                     Qt::AlignLeft | Qt::AlignVCenter, tr("Blending"));
    painter.drawText(QRect(kParentCellX, 0, kParentCellWidth, height()),
                     Qt::AlignLeft | Qt::AlignVCenter, tr("Parent"));
}

QString TimelineColumnHeaders::toolTipAtX(const int x) { return cellToolTipAt(x); }

bool TimelineColumnHeaders::event(QEvent* event) {
    if (event->type() == QEvent::ToolTip) {
        const auto* help = static_cast<QHelpEvent*>(event);
        const QString text = toolTipAtX(help->pos().x());
        if (text.isEmpty()) {
            QToolTip::hideText();
        } else {
            QToolTip::showText(help->globalPos(), text, this);
        }
        event->accept();
        return true;
    }
    return QWidget::event(event);
}

// ---------------------------------------------------------------------------------------------

TimelineLayerStack::TimelineLayerStack(CompositionSession& session, QScrollBar& scrollBar,
                                       QWidget* parent)
    : QWidget(parent), session_(session), scrollBar_(scrollBar) {
    // Unchanged objectName: same role (the composition's layer stack), new primitive.
    setObjectName(QStringLiteral("layerStackView"));
    setAccessibleName(tr("Composition layers"));
    setFixedWidth(kLayerColumnWidthPx);
    // Accepts focus so row navigation has somewhere to live, and so the frame-step shortcuts can
    // keep yielding to it exactly as they yielded to the QTreeWidget before this task.
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    connect(&session_, &CompositionSession::selectionChanged, this,
            &TimelineLayerStack::syncCurrentRowFromSelection);
}

void TimelineLayerStack::syncCurrentRowFromSelection() {
    const auto selected = selectedLayer(session_);
    int resolved = -1;
    if (selected.has_value()) {
        for (int index = 0; index < rowCount(); ++index) {
            if (entries_[static_cast<std::size_t>(index)].layerId == *selected) {
                resolved = index;
                break;
            }
        }
    }
    if (resolved == currentRow_) {
        // Still worth repainting: the row whose FILL changed may not be the current row at all (a
        // node selection can move the contextual layer without moving this column's current row).
        relayoutRows();
        update();
        return;
    }
    currentRow_ = resolved;
    relayoutRows();
    update();
}

int TimelineLayerStack::contentHeight() const noexcept { return rowCount() * kTimelineRowHeight; }

int TimelineLayerStack::rowTop(const int row) const noexcept {
    return row * kTimelineRowHeight - scrollOffset_;
}

void TimelineLayerStack::setEntries(std::vector<TimelineLayerEntry> entries) {
    entries_ = std::move(entries);
    syncCurrentRowFromSelection();
}

void TimelineLayerStack::setScrollOffset(const int offset) {
    if (offset == scrollOffset_) {
        return;
    }
    scrollOffset_ = offset;
    relayoutRows();
    update();
}

void TimelineLayerStack::setCurrentRow(const int row) {
    if (row < 0 || row >= rowCount()) {
        return;
    }
    currentRow_ = row;
    // Navigation IS selection here (single-selection, exactly like the QTreeWidget before it): the
    // session owns selection truth, and every other editor follows it from selectionChanged.
    session_.selectLayer(entries_[static_cast<std::size_t>(row)].layerId);
    relayoutRows();
    update();
}

void TimelineLayerStack::relayoutRows() {
    const int viewportRows = (height() + kTimelineRowHeight - 1) / kTimelineRowHeight + 1;
    const int first =
        std::clamp(scrollOffset_ / kTimelineRowHeight, 0, std::max(0, rowCount() - 1));
    const int needed = std::clamp(rowCount() - first, 0, viewportRows);
    while (static_cast<int>(rowPool_.size()) < needed) {
        rowPool_.push_back(new TimelineLayerRow(session_, this));
    }
    const auto selected = selectedLayer(session_);
    for (std::size_t slot = 0; slot < rowPool_.size(); ++slot) {
        auto* row = rowPool_[slot];
        if (static_cast<int>(slot) >= needed) {
            row->hide();
            continue;
        }
        const int index = first + static_cast<int>(slot);
        const auto& entry = entries_[static_cast<std::size_t>(index)];
        row->bind(entry, selected.has_value() && *selected == entry.layerId);
        row->setGeometry(0, rowTop(index), width(), kTimelineRowHeight);
        row->show();
    }
}

void TimelineLayerStack::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event)
    QPainter painter(this);
    // The rows paint themselves; this is the backdrop under and below them, plus the hairline that
    // divides the column from the lane region beside it.
    painter.fillRect(rect(), kit::color(kit::Color::Background));
    kit::applyHairlinePen(painter, kit::color(kit::Color::Border));
    painter.drawLine(QPointF(static_cast<qreal>(width()) - 0.5, 0.0),
                     QPointF(static_cast<qreal>(width()) - 0.5, static_cast<qreal>(height())));
}

void TimelineLayerStack::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    relayoutRows();
    Q_EMIT viewportResized();
}

void TimelineLayerStack::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    setFocus(Qt::MouseFocusReason);
    const int index =
        (static_cast<int>(event->position().y()) + scrollOffset_) / kTimelineRowHeight;
    if (index < 0 || index >= rowCount()) {
        // Clicking the empty area below the last row clears the selection, exactly as clicking the
        // blank area of the QTreeWidget this column replaces did.
        session_.clearSelection();
        event->accept();
        return;
    }
    setCurrentRow(index);
    event->accept();
}

void TimelineLayerStack::keyPressEvent(QKeyEvent* event) {
    if (rowCount() == 0) {
        QWidget::keyPressEvent(event);
        return;
    }
    // Up/Down/Home/End navigate rows, exactly the keys the QTreeWidget this column replaces claimed
    // for the same purpose -- which is why the frame-step actions still have to yield while this
    // widget holds focus (see TimelineEditor's focusChanged reconciliation).
    const int from = currentRow_ < 0 ? 0 : currentRow_;
    std::optional<int> target;
    switch (event->key()) {
    case Qt::Key_Up:
        target = currentRow_ < 0 ? 0 : std::max(0, from - 1);
        break;
    case Qt::Key_Down:
        target = currentRow_ < 0 ? 0 : std::min(rowCount() - 1, from + 1);
        break;
    case Qt::Key_Home:
        target = 0;
        break;
    case Qt::Key_End:
        target = rowCount() - 1;
        break;
    default:
        break;
    }
    if (!target.has_value()) {
        QWidget::keyPressEvent(event);
        return;
    }
    setCurrentRow(*target);
    event->accept();
}

QString TimelineLayerStack::toolTipAt(const QPoint position) const {
    QString toggle = cellToolTipAt(position.x());
    if (!toggle.isEmpty()) {
        return toggle;
    }
    const int index = (position.y() + scrollOffset_) / kTimelineRowHeight;
    if (index < 0 || index >= rowCount()) {
        return {};
    }
    // The row's kind lives here: task T1 removes the Kind COLUMN, and this is where that
    // information keeps reaching the artist rather than being silently dropped with the column.
    const auto& entry = entries_[static_cast<std::size_t>(index)];
    return tr("%1 · Layer %2 · Slot %3")
        .arg(entry.kind)
        .arg(entry.layerId.value())
        .arg(entry.slotId.value());
}

bool TimelineLayerStack::event(QEvent* event) {
    if (event->type() == QEvent::ToolTip) {
        const auto* help = static_cast<QHelpEvent*>(event);
        const QString text = toolTipAt(help->pos());
        if (text.isEmpty()) {
            QToolTip::hideText();
        } else {
            QToolTip::showText(help->globalPos(), text, this);
        }
        event->accept();
        return true;
    }
    return QWidget::event(event);
}

void TimelineLayerStack::wheelEvent(QWheelEvent* event) {
    const int steps = event->angleDelta().y() / 120;
    if (steps == 0) {
        QWidget::wheelEvent(event);
        return;
    }
    scrollBar_.setValue(scrollBar_.value() - steps * scrollBar_.singleStep());
    event->accept();
}

// ---------------------------------------------------------------------------------------------

TimelineLaneRegion::TimelineLaneRegion(CompositionSession& session, TimelineRuler& ruler,
                                       QScrollBar& scrollBar, QWidget* parent)
    : QWidget(parent), session_(session), ruler_(ruler), scrollBar_(scrollBar) {
    setObjectName(QStringLiteral("timelineLaneRegion"));
    setAccessibleName(tr("Layer lanes"));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    connect(&session_, &CompositionSession::currentTimeChanged, this,
            qOverload<>(&TimelineLaneRegion::update));
    connect(&session_, &CompositionSession::selectionChanged, this,
            qOverload<>(&TimelineLaneRegion::update));
    connect(&session_, &CompositionSession::snapshotChanged, this,
            qOverload<>(&TimelineLaneRegion::update));
    connect(&session_, &CompositionSession::compositionChanged, this,
            qOverload<>(&TimelineLaneRegion::update));
}

int TimelineLaneRegion::contentHeight() const noexcept {
    return static_cast<int>(entries_.size()) * kTimelineRowHeight;
}

int TimelineLaneRegion::rowTop(const int row) const noexcept {
    return row * kTimelineRowHeight - scrollOffset_;
}

void TimelineLaneRegion::setEntries(std::vector<TimelineLayerEntry> entries) {
    entries_ = std::move(entries);
    update();
}

void TimelineLaneRegion::setScrollOffset(const int offset) {
    if (offset == scrollOffset_) {
        return;
    }
    scrollOffset_ = offset;
    update();
}

std::optional<QRect> TimelineLaneRegion::clipBarRect(const int row) const {
    if (row < 0 || row >= static_cast<int>(entries_.size())) {
        return std::nullopt;
    }
    const auto* composition = session_.composition();
    if (composition == nullptr) {
        return std::nullopt;
    }
    const auto axis = TimelineAxis::create(*composition, width());
    if (!axis.has_value()) {
        return std::nullopt;
    }
    // No trim exists anywhere in the document model (document::LayerStackEntry carries a slot id
    // and a layer id, and no in/out point exists on a layer at all), so the honest extent is the
    // WHOLE composition range -- derived from the axis rather than assumed to be the full widget
    // width, so the day a trim feature lands this is already asking the right question.
    const qreal left = axis->pixelForTime(core::RationalTime::fromInteger(0));
    const qreal right = axis->pixelForTime(composition->duration());
    const int inset = kit::px(kit::Spacing::XXS);
    const int top = rowTop(row) + inset;
    const int barWidth = std::max(1, static_cast<int>(std::lround(right - left)) + 1);
    return QRect(static_cast<int>(std::lround(left)), top, barWidth,
                 kTimelineRowHeight - 2 * inset);
}

void TimelineLaneRegion::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event)
    QPainter painter(this);
    painter.fillRect(rect(), kit::color(kit::Color::Background));

    const auto selected = selectedLayer(session_);
    const int firstRow = std::max(0, scrollOffset_ / kTimelineRowHeight);
    const int lastRow = std::min(static_cast<int>(entries_.size()) - 1,
                                 (scrollOffset_ + height()) / kTimelineRowHeight);
    for (int row = firstRow; row <= lastRow; ++row) {
        const auto& entry = entries_[static_cast<std::size_t>(row)];
        const int top = rowTop(row);
        painter.setRenderHint(QPainter::Antialiasing, false);
        if (selected.has_value() && *selected == entry.layerId) {
            paintSelectedRowFill(painter, top, width());
        }
        paintRowSeparator(painter, top, width());
        if (const auto bar = clipBarRect(row)) {
            painter.setRenderHint(QPainter::Antialiasing, true);
            kit::fillRoundedSurface(painter, QRectF(*bar), kit::color(entry.clipColor), QColor(),
                                    kit::Radius::Small);
        }
    }

    const auto* composition = session_.composition();
    if (composition == nullptr) {
        return;
    }
    const auto axis = TimelineAxis::create(*composition, width());
    if (!axis.has_value()) {
        return;
    }
    // One stroke for the whole region, painted after every lane, so the playhead reads as a single
    // continuous line down all of them and moving it never touches a layout.
    paintPlayheadLine(painter, *axis, session_.currentTime(), height());
}

void TimelineLaneRegion::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    // Dragging a lane scrubs, through the ruler's own scrub path. Selection is the left column's
    // job: a lane carries no trim, no clip edge, and no per-clip gesture that a press could mean
    // instead, so making the whole lane region a second scrub surface is the honest reading of it.
    ruler_.beginScrub(static_cast<int>(event->position().x()));
    event->accept();
}

void TimelineLaneRegion::mouseMoveEvent(QMouseEvent* event) {
    ruler_.updateScrub(static_cast<int>(event->position().x()));
    event->accept();
}

void TimelineLaneRegion::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    ruler_.endScrub(static_cast<int>(event->position().x()));
    event->accept();
}

void TimelineLaneRegion::wheelEvent(QWheelEvent* event) {
    const int steps = event->angleDelta().y() / 120;
    if (steps == 0) {
        QWidget::wheelEvent(event);
        return;
    }
    scrollBar_.setValue(scrollBar_.value() - steps * scrollBar_.singleStep());
    event->accept();
}

// ---------------------------------------------------------------------------------------------

int TimelineEditor::layerColumnWidth() { return kLayerColumnWidthPx; }

TimelineEditor::TimelineEditor(CompositionSession& session,
                               CompositionPreviewController& previewController,
                               RamPreviewController* const ramPreview, QWidget* parent)
    : QWidget(parent), session_(session), ramPreview_(ramPreview) {
    setObjectName("timelineEditor");
    setAccessibleName(tr("Layers timeline"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    playback_ = &previewController.playbackController();

    // ---- Header row: the transport/readout cluster on the left, the work area on the right
    // -------
    auto* headerRow = new QWidget(this);
    headerRow->setObjectName("timelineHeaderRow");
    headerRow->setFixedHeight(kit::px(kit::Size::Control));
    auto* headerLayout = new QHBoxLayout(headerRow);
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(0);

    auto* controls = new QWidget(headerRow);
    controls->setObjectName("timelineControls");
    controls->setFixedWidth(kLayerColumnWidthPx);
    auto* controlsLayout = new QHBoxLayout(controls);
    controlsLayout->setContentsMargins(kColumnPadding, 0, kColumnPadding, 0);
    controlsLayout->setSpacing(kit::px(kit::Spacing::XS));

    auto* title = new QLabel(tr("Layers"), controls);
    title->setObjectName("editorSectionTitle");
    addButton_ = makeToolButton(tr("Add"), tr("Add layer"), controls);
    addButton_->setObjectName("addLayerButton");
    addButton_->setIcon(kit::icon(kit::IconId::Add, kit::Size::IconMedium));
    addButton_->setIconSize(QSize(kit::px(kit::Size::IconMedium), kit::px(kit::Size::IconMedium)));
    addButton_->setFixedSize(kit::px(kit::Size::Control), kit::px(kit::Size::Control));
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

    // Playback transport (issue #105, decision 4): a StepBack/Play-Pause/StepForward/Loop row.
    // playPauseButton_ MUST stay a QToolButton with its existing text()/isChecked() contract
    // (playback_controller_tests.cpp, composition_projection_test.cpp both read it by exactly that
    // type/objectName).
    stepBackButton_ = makeIconToolButton(kit::IconId::StepBack, tr("Step back one frame (Left)"),
                                         tr("Step back one frame"),
                                         QStringLiteral("timelineStepBackButton"), controls);
    playPauseButton_ = makeToolButton(tr("Play"), tr("Toggle playback"), controls);
    playPauseButton_->setObjectName("playPauseButton");
    playPauseButton_->setCheckable(true);
    playPauseButton_->setFixedSize(kit::px(kit::Size::Control), kit::px(kit::Size::Control));
    stepForwardButton_ = makeIconToolButton(
        kit::IconId::StepForward, tr("Step forward one frame (Right)"),
        tr("Step forward one frame"), QStringLiteral("timelineStepForwardButton"), controls);
    // RAM Preview (task PERF1, item 3). IconId::Sequence is the nearest honest glyph in the kit's
    // existing vocabulary -- a run of frames -- rather than a new vendored asset for one button;
    // the tooltip and accessible name carry the meaning, as iconography rules require of an
    // icon-only control.
    ramPreviewButton_ = makeIconToolButton(
        kit::IconId::Sequence,
        tr("RAM Preview: cache this composition, then play it (Ctrl+Shift+Space)"),
        tr("RAM preview"), QStringLiteral("timelineRamPreviewButton"), controls);
    ramPreviewButton_->setCheckable(true);
    ramPreviewButton_->setEnabled(ramPreview_ != nullptr);
    // Loop indicator: non-interactive status glyph, not a button -- playback always loops
    // (PlaybackController::tick()'s exact modulo wrap) and there is no command to disable it, so a
    // clickable control here would dishonestly imply a toggle that does not exist.
    loopIndicator_ = new QLabel(controls);
    loopIndicator_->setObjectName(QStringLiteral("timelineLoopIndicator"));
    loopIndicator_->setAccessibleName(tr("Playback loops continuously"));
    loopIndicator_->setToolTip(tr("Playback always loops; there is no command to disable it yet"));
    loopIndicator_->setPixmap(kit::iconPixmap(kit::IconId::Loop, kit::Size::IconMedium,
                                              kit::Color::Accent, kit::State::Normal,
                                              kit::IconWeight::Fill));
    // Current time readout (issue #108, decision 3). Text is set by updateTimeReadout(), not here.
    timeReadout_ = new QLabel(controls);
    timeReadout_->setObjectName("timelineTimeReadout");
    timeReadout_->setAccessibleName(tr("Current frame and time"));
    timeReadout_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    timeReadout_->setFont(kit::font(kit::TypeRole::Value));
    undoButton_ = makeToolButton(tr("Undo"), tr("Undo last edit"), controls);
    redoButton_ = makeToolButton(tr("Redo"), tr("Redo last edit"), controls);
    // Left-aligned, in the order the design mock reads them; the trailing stretch is what makes the
    // whole cluster sit against the left edge of the column rather than spreading across it.
    controlsLayout->addWidget(title);
    controlsLayout->addWidget(addButton_);
    controlsLayout->addWidget(stepBackButton_);
    controlsLayout->addWidget(playPauseButton_);
    controlsLayout->addWidget(stepForwardButton_);
    controlsLayout->addWidget(ramPreviewButton_);
    controlsLayout->addWidget(loopIndicator_);
    controlsLayout->addWidget(timeReadout_);
    controlsLayout->addWidget(undoButton_);
    controlsLayout->addWidget(redoButton_);
    controlsLayout->addStretch(1);

    workArea_ = new TimelineWorkAreaRow(session_, headerRow);
    auto* headerGutter = new QWidget(headerRow);
    headerGutter->setObjectName("timelineHeaderScrollGutter");
    headerGutter->setFixedWidth(kScrollGutterWidth);
    headerLayout->addWidget(controls);
    headerLayout->addWidget(workArea_, 1);
    headerLayout->addWidget(headerGutter);

    // ---- Column-header row: the icon/name/blending/parent headers, then the RULER ---------------
    // This is what puts frame 0 at the lane region's left edge: the ruler is the RIGHT member of
    // this row, so its own x origin IS the left column's width, and it never paints over that
    // column.
    auto* columnHeaderRow = new QWidget(this);
    columnHeaderRow->setObjectName("timelineColumnHeaderRow");
    columnHeaderRow->setFixedHeight(kit::px(kit::Size::Control));
    auto* columnHeaderLayout = new QHBoxLayout(columnHeaderRow);
    columnHeaderLayout->setContentsMargins(0, 0, 0, 0);
    columnHeaderLayout->setSpacing(0);
    columnHeaders_ = new TimelineColumnHeaders(columnHeaderRow);
    ruler_ = new TimelineRuler(session_, previewController, columnHeaderRow);
    auto* rulerGutter = new QWidget(columnHeaderRow);
    rulerGutter->setObjectName("timelineRulerScrollGutter");
    rulerGutter->setFixedWidth(kScrollGutterWidth);
    columnHeaderLayout->addWidget(columnHeaders_);
    columnHeaderLayout->addWidget(ruler_, 1);
    columnHeaderLayout->addWidget(rulerGutter);

    // ---- Body: the layer column and the lane region, under ONE scrollbar ------------------------
    auto* body = new QWidget(this);
    body->setObjectName("timelineBody");
    auto* bodyLayout = new QHBoxLayout(body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(0);
    scrollBar_ = new QScrollBar(Qt::Vertical, body);
    scrollBar_->setObjectName("timelineVerticalScrollBar");
    scrollBar_->setAccessibleName(tr("Layer stack scroll"));
    stack_ = new TimelineLayerStack(session_, *scrollBar_, body);
    lanes_ = new TimelineLaneRegion(session_, *ruler_, *scrollBar_, body);
    auto* bodyGutter = new QWidget(body);
    bodyGutter->setObjectName("timelineBodyScrollGutter");
    bodyGutter->setFixedWidth(kScrollGutterWidth);
    auto* gutterLayout = new QHBoxLayout(bodyGutter);
    gutterLayout->setContentsMargins(0, 0, 0, 0);
    gutterLayout->setSpacing(0);
    // The scrollbar sits inside a gutter of the scrollbar's own HOVER extent, so the kit
    // stylesheet's hover growth happens INSIDE the reserved width and the lane region's own width
    // -- and therefore the time axis -- never moves under the pointer.
    gutterLayout->addWidget(scrollBar_, 0, Qt::AlignRight);
    bodyLayout->addWidget(stack_);
    bodyLayout->addWidget(lanes_, 1);
    bodyLayout->addWidget(bodyGutter);

    // ---- The keyframe lanes, indented onto the same time axis -----------------------------------
    auto* keyframeArea = new QWidget(this);
    keyframeArea->setObjectName("timelineKeyframeArea");
    auto* keyframeLayout = new QHBoxLayout(keyframeArea);
    keyframeLayout->setContentsMargins(0, 0, 0, 0);
    keyframeLayout->setSpacing(0);
    auto* keyframeIndent = new QWidget(keyframeArea);
    keyframeIndent->setObjectName("timelineKeyframeIndent");
    keyframeIndent->setFixedWidth(kLayerColumnWidthPx);
    keyframes_ = new TimelineKeyframePanel(session_, keyframeArea);
    auto* keyframeGutter = new QWidget(keyframeArea);
    keyframeGutter->setObjectName("timelineKeyframeScrollGutter");
    keyframeGutter->setFixedWidth(kScrollGutterWidth);
    keyframeLayout->addWidget(keyframeIndent);
    keyframeLayout->addWidget(keyframes_, 1);
    keyframeLayout->addWidget(keyframeGutter);

    layout->addWidget(headerRow);
    layout->addWidget(columnHeaderRow);
    layout->addWidget(body, 1);
    layout->addWidget(keyframeArea);

    // One definition of the "Add Solid"/"Add Text" default name and proof color, shared with the
    // Nodes canvas context menu (task U4, issue #123) -- see composition_editors.hpp.
    connect(addSolidAction, &QAction::triggered, this,
            [this] { (void)addDefaultSolidLayer(session_); });
    connect(addTextAction, &QAction::triggered, this,
            [this] { (void)addDefaultTextLayer(session_); });
    connect(undoButton_, &QToolButton::clicked, &session_, &CompositionSession::undo);
    connect(redoButton_, &QToolButton::clicked, &session_, &CompositionSession::redo);
    connect(playPauseButton_, &QToolButton::clicked, playback_, &PlaybackController::toggle);
    connect(playback_, &PlaybackController::stateChanged, this,
            &TimelineEditor::updatePlaybackButton);
    updatePlaybackButton(playback_->state());

    // ONE scrollbar drives both halves of the grid: left/right scroll sync is structural here, not
    // a pair of handlers keeping two scroll areas in step.
    connect(scrollBar_, &QScrollBar::valueChanged, this, [this](const int value) {
        stack_->setScrollOffset(value);
        lanes_->setScrollOffset(value);
    });
    connect(stack_, &TimelineLayerStack::viewportResized, this, &TimelineEditor::updateScrollRange);

    // RAM Preview's KEYS are not declared here. Ctrl+Shift+Space and the Escape that cancels a run
    // are application-wide commands owned by the Composition menu (main_window.cpp): one
    // Qt::WindowShortcut owner per sequence, or Qt reports an ambiguous overload and fires neither.
    // This button is the transport's own affordance for that same command, and it calls the same
    // RamPreviewController::toggle() the menu item calls -- never a synthesized key press.
    if (ramPreview_ != nullptr) {
        connect(ramPreviewButton_, &QToolButton::clicked, ramPreview_,
                &RamPreviewController::toggle);
        connect(ramPreview_, &RamPreviewController::stateChanged, this,
                &TimelineEditor::updateRamPreviewButton);
        updateRamPreviewButton();
    }

    // Frame-stepping shortcuts (issue #108, decisions 1/2), mirroring playPauseAction's own
    // WindowShortcut idiom exactly. Issue #120 (task U5) replaced PropertiesEditor's Position X/Y
    // QDoubleSpinBoxes with kit::KValueField, which has no line edit and does NOT accept
    // ShortcutOverride for Left/Right/Home/End -- so those keys typed while a Position field has
    // focus ALSO fire these actions. Still flagged rather than fixed here: the fix belongs to
    // whoever next owns kit::KValueField's key handling.
    stepBackwardAction_ = new QAction(tr("Step Back One Frame"), this);
    stepBackwardAction_->setObjectName("stepBackwardAction");
    stepBackwardAction_->setShortcut(QKeySequence(Qt::Key_Left));
    stepBackwardAction_->setShortcutContext(Qt::WindowShortcut);
    addAction(stepBackwardAction_);
    connect(stepBackwardAction_, &QAction::triggered, this, [this] { stepFrame(-1); });
    // The visible stepBackButton_ triggers this SAME action -- one behavior, two entry points.
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

    // Arrow-key conflict reconciliation, carried over verbatim from the QTreeWidget this panel used
    // to be (issue #108's own investigation): the layer stack consumes Up/Down/Home/End for its OWN
    // row navigation but, unlike a text-entry widget, does not claim the ShortcutOverride event for
    // them, so a same-key WindowShortcut action would silently swallow that navigation. The frozen
    // rule -- widget focus wins, the step action fires otherwise -- is implemented by disabling
    // these four actions outright while the stack holds keyboard focus: a disabled QAction never
    // claims ShortcutOverride, so the key event reaches the stack and its navigation runs
    // unchanged.
    focusConnection_ =
        connect(qApp, &QApplication::focusChanged, this, [this](QWidget*, QWidget* now) {
            const bool stackFocused =
                now != nullptr && (now == stack_ || stack_->isAncestorOf(now));
            stepBackwardAction_->setEnabled(!stackFocused);
            stepForwardAction_->setEnabled(!stackFocused);
            stepToStartAction_->setEnabled(!stackFocused);
            stepToEndAction_->setEnabled(!stackFocused);
            // The visible step buttons mirror their action's enabled state exactly, so the same
            // reconciliation is visible on the mouse affordance too rather than showing a clickable
            // button that would silently do nothing.
            stepBackButton_->setEnabled(!stackFocused);
            stepForwardButton_->setEnabled(!stackFocused);
        });

    connect(&session_, &CompositionSession::snapshotChanged, this, &TimelineEditor::rebuild);
    connect(&session_, &CompositionSession::compositionChanged, this, &TimelineEditor::rebuild);
    connect(&session_, &CompositionSession::selectionChanged, this,
            &TimelineEditor::updateSelection);
    connect(&session_, &CompositionSession::historyChanged, this,
            &TimelineEditor::updateHistoryActions);
    // Readout updates on every session-time change and on a composition switch (which resets
    // session time to exact zero -- docs/architecture/animation-and-time.md, "Session Time And
    // Scrubbing"), so the label always reflects the SAME time compositionChanged's reset already
    // produced rather than momentarily showing the previous composition's stale frame/time.
    connect(&session_, &CompositionSession::currentTimeChanged, this,
            &TimelineEditor::updateTimeReadout);
    connect(&session_, &CompositionSession::compositionChanged, this,
            &TimelineEditor::updateTimeReadout);

    rebuild();
    updateHistoryActions();
    updateTimeReadout();
}

TimelineEditor::~TimelineEditor() { QObject::disconnect(focusConnection_); }

void TimelineEditor::rebuild() {
    std::vector<TimelineLayerEntry> entries;
    const auto* composition = session_.composition();
    if (composition != nullptr) {
        const auto stackEntries = composition->graph().layerStack().entries();
        entries.reserve(stackEntries.size());
        for (const auto& entry : stackEntries) {
            entries.push_back({.layerId = entry.layerId,
                               .slotId = entry.slotId,
                               .name = layerName(*composition, entry.layerId),
                               .kind = layerKind(session_, entry.layerId),
                               .clipColor = layerClipColorToken(session_, entry.layerId)});
        }
    }
    stack_->setEntries(entries);
    lanes_->setEntries(std::move(entries));
    updateScrollRange();
    updateSelection();
}

void TimelineEditor::updateScrollRange() {
    const int viewport = stack_->height();
    const int maximum = std::max(0, stack_->contentHeight() - viewport);
    scrollBar_->setRange(0, maximum);
    scrollBar_->setPageStep(std::max(1, viewport));
    scrollBar_->setSingleStep(kTimelineRowHeight);
}

void TimelineEditor::updateSelection() {
    // Both halves repaint themselves off selectionChanged (the stack re-binds its rows, the lane
    // region repaints its fills), so the editor's only remaining job is bringing the selected row
    // into view -- exactly what QTreeWidget::scrollToItem() did before this task.
    const int current = stack_->currentRow();
    if (current < 0) {
        return;
    }
    const int top = current * kTimelineRowHeight;
    const int viewport = stack_->height();
    if (top < scrollBar_->value()) {
        scrollBar_->setValue(top);
    } else if (top + kTimelineRowHeight > scrollBar_->value() + viewport) {
        scrollBar_->setValue(top + kTimelineRowHeight - viewport);
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

void TimelineEditor::updateRamPreviewButton() {
    if (ramPreview_ == nullptr) {
        return;
    }
    const bool caching = ramPreview_->isCaching();
    ramPreviewButton_->setChecked(caching);
    ramPreviewButton_->setToolTip(
        caching ? tr("Cancel the RAM preview being cached (Esc)")
                : tr("RAM Preview: cache this composition, then play it (Ctrl+Shift+Space)"));
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
    // pauses" side effect, so this call site is honest about what it does and the transport state
    // change is never a coincidental side effect of the time write below. Called unconditionally
    // (idempotent no-op if already Stopped), not only when the step actually moves the playhead.
    playback_->pause();

    // Left/Right move exactly one frame index from the nearest index to the CURRENT (possibly
    // subframe) time, clamped to [0, maxFrameIndex] (design decision 1). nearestFrameIndex()'s own
    // tie rule decides which frame a subframe time steps from, not this call site.
    std::uint64_t target = *nearest;
    if (delta < 0) {
        target = target > 0 ? target - 1 : 0;
    } else {
        target = target < context->maxFrameIndexValue ? target + 1 : context->maxFrameIndexValue;
    }
    const auto targetTime = frameTimeForIndex(context->frameRate, context->duration, target);
    if (targetTime.has_value()) {
        // A clamped step that lands back on the CURRENT exact time (e.g. Left at frame 0) is a true
        // no-op through CompositionSession::setCurrentTime()'s own early-return-on-equal-time guard
        // -- no currentTimeChanged signal churn.
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

} // namespace bloom::ui
