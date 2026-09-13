#include <bloom/ui/timeline_editor.hpp>

#include "composition_editor_support.hpp"
#include "timeline_property_rows.hpp"

#include <bloom/ui/composition_authoring.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/ram_preview_controller.hpp>
#include <bloom/ui/timeline_frame_math.hpp>
#include <bloom/ui/timeline_ruler.hpp>

#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/icons.hpp>
#include <bloom/ui/kit/painting.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/core/blend_mode.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/project.hpp>

#include <QAction>
#include <QApplication>
#include <QColorDialog>
#include <QContextMenuEvent>
#include <QEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QIcon>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
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
constexpr int kToggleCellCount = 3;
constexpr int kNameCellWidth = kit::px(kit::Size::ControlRoomy) * 6;
constexpr int kBlendingCellWidth = kit::px(kit::Size::ControlRoomy) * 3;

constexpr int kToggleStripX = kColumnPadding;
constexpr int kNameCellX = kToggleStripX + kToggleCellCount * kToggleCellWidth + kCellGap;
constexpr int kBlendingCellX = kNameCellX + kNameCellWidth + kCellGap;
constexpr int kLayerColumnWidthPx = kBlendingCellX + kBlendingCellWidth + kColumnPadding;

// The scroll gutter reserved to the right of the lane region. It is the scrollbar's HOVER extent,
// not its resting one: the kit stylesheet grows a hovered vertical scrollbar from Size::ScrollBar
// to Size::ScrollBarHover, and if that growth came out of the lane region's own width the ruler
// above it would stop agreeing with the lanes about where a frame is the instant the pointer
// touched the scrollbar. Reserving the larger extent once means the time axis never moves.
constexpr int kScrollGutterWidth = kit::px(kit::Size::ScrollBarHover);

[[nodiscard]] int toggleCellX(const int index) { return kToggleStripX + index * kToggleCellWidth; }

// ---------------------------------------------------------------------------------------------
// Visibility, solo and lock share the same cell geometry in headings and rows.
enum class ToggleCell : int { Visibility = 0, Solo = 1, Lock = 2 };
kit::IconId toggleIcon(const int index) {
    switch (static_cast<ToggleCell>(index)) {
    case ToggleCell::Visibility:
        return kit::IconId::Visible;
    case ToggleCell::Solo:
        return kit::IconId::Check;
    case ToggleCell::Lock:
        return kit::IconId::Locked;
    }
    return kit::IconId::Visible;
}
QString toggleToolTip(const int index) {
    switch (static_cast<ToggleCell>(index)) {
    case ToggleCell::Visibility:
        return TimelineEditor::tr("Toggle layer visibility");
    case ToggleCell::Solo:
        return TimelineEditor::tr("Solo: render only soloed layers");
    case ToggleCell::Lock:
        return TimelineEditor::tr("Lock layer editing");
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

[[nodiscard]] bool isLayerSelected(const CompositionSession& session, document::LayerId layer) {
    const auto boundary = session.boundaryNodeForLayer(layer);
    return (boundary.has_value() && session.selectedNodes().contains(*boundary)) ||
           selectedLayer(session) == layer;
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
        setAttribute(Qt::WA_TransparentForMouseEvents, false);
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
        expanded_ = entry.expanded;
        color_ = entry.labelColor.isValid() ? entry.labelColor : kit::color(entry.clipColor);
        selected_ = selected;
        layerId_ = entry.layerId;
        // `binding_` (not just a QSignalBlocker) because setCurrentIndex() is a projection of
        // document truth, never an edit: a blocked signal would still leave the lambda armed for a
        // nested change, and a row re-pointed during a scroll must author nothing at all.
        binding_ = true;
        const auto mode = session_->blendModeForLayer(entry.layerId);
        const int row = mode.has_value() ? blendingDropdownIndex(*blending_, *mode) : -1;
        const auto* composition = session_->composition();
        const auto* layer = composition ? composition->graph().findLayer(entry.layerId) : nullptr;
        enabled_ = layer && layer->enabled;
        if (layer) {
            const auto layout = composition->nodeLayout().find(layer->nodeId);
            enabled_ =
                enabled_ && (layout == composition->nodeLayout().end() || !layout->second.muted);
        }
        solo_ = layer && layer->solo;
        locked_ = layer && layer->locked;
        blending_->setEnabled(mode.has_value() && !locked_);
        parentDropdown_->hide();
        blending_->setCurrentIndex(row >= 0 ? row : 0);
        blending_->setToolTip(mode.has_value()
                                  ? TimelineEditor::tr("How this layer combines with the layers "
                                                       "beneath it")
                                  : TimelineEditor::tr("This layer does not expose a blend mode"));
        binding_ = false;
        update();
    }

  protected:
    void mousePressEvent(QMouseEvent* event) override { forwardMouse(event); }
    void mouseMoveEvent(QMouseEvent* event) override { forwardMouse(event); }
    void mouseReleaseEvent(QMouseEvent* event) override { forwardMouse(event); }
    void mouseDoubleClickEvent(QMouseEvent* event) override { forwardMouse(event); }
    void contextMenuEvent(QContextMenuEvent* event) override {
        QContextMenuEvent mapped(event->reason(), mapToParent(event->pos()), event->globalPos(),
                                 event->modifiers());
        QApplication::sendEvent(parentWidget(), &mapped);
    }
    void forwardMouse(QMouseEvent* event) {
        QMouseEvent mapped(event->type(), mapToParent(event->position().toPoint()),
                           event->globalPosition(), event->button(), event->buttons(),
                           event->modifiers());
        QApplication::sendEvent(parentWidget(), &mapped);
        event->accept();
    }
    void resizeEvent(QResizeEvent* event) override {
        QWidget::resizeEvent(event);
        // Fixed cell geometry, assigned directly rather than through a nested layout: a row is a
        // fixed table of cells, and a QHBoxLayout per row would re-solve that same table on every
        // bind and every scroll step for no gain.
        const int dropdownHeight = std::min(height(), blending_->sizeHint().height());
        const int top = (height() - dropdownHeight) / 2;
        blending_->setGeometry(kBlendingCellX, top, kBlendingCellWidth, dropdownHeight);
        parentDropdown_->hide();
    }

    void paintEvent(QPaintEvent* event) override {
        Q_UNUSED(event)
        QPainter painter(this);
        // Flat rows: no alternating stripe at all. The selected row takes SurfaceRaised.
        painter.fillRect(
            rect(), kit::color(selected_ ? kit::Color::SurfaceRaised : kit::Color::Background));
        paintRowSeparator(painter, 0, width());

        for (int index = 0; index < kToggleCellCount; ++index) {
            const bool active = index == 0 ? enabled_ : index == 1 ? solo_ : locked_;
            const auto id = index == 0   ? (enabled_ ? kit::IconId::Visible : kit::IconId::Hidden)
                            : index == 1 ? kit::IconId::Check
                                         : (locked_ ? kit::IconId::Locked : kit::IconId::Unlocked);
            const auto glyph = kit::iconPixmap(id, kit::Size::IconMedium,
                                               active ? kit::Color::Foreground : kit::Color::Faint,
                                               kit::State::Normal, kit::IconWeight::Bold);
            const int extent = kit::px(kit::Size::IconMedium);
            painter.drawPixmap(QRect(toggleCellX(index) + (kToggleCellWidth - extent) / 2,
                                     (height() - extent) / 2, extent, extent),
                               glyph);
        }

        painter.setFont(kit::font(kit::TypeRole::Ui));
        painter.setPen(kit::color(kit::Color::Foreground));
        const int swatch = kit::px(kit::Spacing::S);
        painter.fillRect(QRect(kNameCellX, (height() - swatch) / 2, swatch, swatch), color_);
        const auto chevron =
            kit::iconPixmap(expanded_ ? kit::IconId::CaretDown : kit::IconId::CaretRight,
                            kit::Size::IconMedium, kit::Color::Muted);
        painter.drawPixmap(kNameCellX + swatch + kCellGap, (height() - chevron.height()) / 2,
                           chevron);
        const int indent = kit::px(kit::Size::IconMedium);
        const QRect nameRect(kNameCellX + swatch + kCellGap + indent, 0,
                             kNameCellWidth - swatch - kCellGap - indent, height());
        const QFontMetrics metrics = painter.fontMetrics();
        painter.drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter,
                         metrics.elidedText(name_, Qt::ElideRight, nameRect.width()));
    }

  private:
    CompositionSession* session_ = nullptr;
    QString name_;
    QColor color_;
    std::optional<document::LayerId> layerId_;
    bool selected_ = false;
    bool expanded_ = false;
    bool enabled_ = true, solo_ = false, locked_ = false;
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
        const auto glyph =
            kit::iconPixmap(toggleIcon(index), kit::Size::IconMedium, kit::Color::Muted,
                            kit::State::Normal, kit::IconWeight::Bold);
        const int glyphExtent = kit::px(kit::Size::IconMedium);
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
    dragRow_ = -1;
    insertionRow_ = -1;
    if (insertion_)
        insertion_->hide();
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
    while (static_cast<int>(propertyPool_.size()) < needed) {
        propertyPool_.push_back(nullptr);
    }
    for (std::size_t slot = 0; slot < rowPool_.size(); ++slot) {
        auto* row = rowPool_[slot];
        auto* property = propertyPool_[slot];
        if (static_cast<int>(slot) >= needed) {
            row->hide();
            if (property)
                property->hide();
            continue;
        }
        const int index = first + static_cast<int>(slot);
        const auto& entry = entries_[static_cast<std::size_t>(index)];
        if (entry.rowKind == TimelineLayerEntry::Kind::Layer) {
            if (property)
                property->hide();
            row->bind(entry, isLayerSelected(session_, entry.layerId));
            row->setGeometry(0, rowTop(index), width(), kTimelineRowHeight);
            row->show();
        } else {
            row->hide();
            if (!property)
                property = propertyPool_[slot] = new TimelinePropertyRow(session_, this);
            property->bind(entry);
            property->setGeometry(0, rowTop(index), width(), kTimelineRowHeight);
            property->show();
        }
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
        session_.clearSelection();
        return;
    }
    const auto& entry = entries_[static_cast<std::size_t>(index)];
    const auto id = entry.layerId;
    if (entry.rowKind != TimelineLayerEntry::Kind::Layer)
        return;
    const int chevronX = kNameCellX + kit::px(kit::Spacing::S) + kCellGap;
    if (event->position().x() >= chevronX &&
        event->position().x() < chevronX + kit::px(kit::Size::IconMedium)) {
        Q_EMIT expansionRequested(id);
        return;
    }
    const auto* composition = session_.composition();
    const auto* layer = composition ? composition->graph().findLayer(id) : nullptr;
    if (!layer)
        return;
    const int toggle = (static_cast<int>(event->position().x()) - kToggleStripX) / kToggleCellWidth;
    if (event->position().x() >= kToggleStripX &&
        event->position().x() < kToggleStripX + kToggleCellCount * kToggleCellWidth) {
        commands::Transaction transaction("Toggle Layer", session_.snapshot().revision());
        if (toggle == 0) {
            const auto layout = composition->nodeLayout().find(layer->nodeId);
            const bool visible = layer->enabled && (layout == composition->nodeLayout().end() ||
                                                    !layout->second.muted);
            transaction.emplace<commands::SetLayerEnabled>(session_.compositionId(), id, !visible);
        }
        if (toggle == 1)
            transaction.emplace<commands::SetLayerSolo>(session_.compositionId(), id, !layer->solo);
        if (toggle == 2)
            transaction.emplace<commands::SetLayerLocked>(session_.compositionId(), id,
                                                          !layer->locked);
        (void)session_.executeTransaction(std::move(transaction));
        return;
    }
    const auto node = layer->nodeId;
    auto nodes = session_.selectedNodes();
    if (event->modifiers().testFlag(Qt::ShiftModifier) && anchorRow_ >= 0) {
        for (int row = std::min(anchorRow_, index);
             row <= std::max(anchorRow_, index) && row < rowCount(); ++row)
            if (const auto boundary =
                    session_.boundaryNodeForLayer(entries_[static_cast<std::size_t>(row)].layerId))
                nodes.insert(*boundary);
        session_.selectNodes(nodes, node);
    } else if (event->modifiers().testFlag(Qt::ControlModifier)) {
        if (nodes.contains(node))
            nodes.erase(node);
        else
            nodes.insert(node);
        if (nodes.empty())
            session_.clearSelection();
        else
            session_.selectNodes(nodes, nodes.contains(node) ? node : *nodes.begin());
        anchorRow_ = index;
    } else {
        session_.selectLayer(id);
        anchorRow_ = index;
    }
    dragRow_ = index;
    dragStart_ = event->position().toPoint();
    dragRevision_ = session_.snapshot().revision();
    event->accept();
}

void TimelineLayerStack::mouseMoveEvent(QMouseEvent* event) {
    if (dragRow_ < 0 || !event->buttons().testFlag(Qt::LeftButton) ||
        (event->position().toPoint() - dragStart_).manhattanLength() <
            QApplication::startDragDistance())
        return;
    insertionRow_ = std::clamp(
        (static_cast<int>(event->position().y()) + scrollOffset_ + kTimelineRowHeight / 2) /
            kTimelineRowHeight,
        0, rowCount());
    while (insertionRow_ < rowCount() &&
           entries_[static_cast<std::size_t>(insertionRow_)].rowKind !=
               TimelineLayerEntry::Kind::Layer)
        ++insertionRow_;
    if (!insertion_) {
        insertion_ = new QWidget(this);
        insertion_->setObjectName("timelineLayerInsertionIndicator");
        insertion_->setAttribute(Qt::WA_TransparentForMouseEvents);
        insertion_->setAutoFillBackground(true);
        auto palette = insertion_->palette();
        palette.setColor(QPalette::Window, kit::color(kit::Color::Accent));
        insertion_->setPalette(palette);
    }
    insertion_->setGeometry(0, rowTop(insertionRow_), width(), kit::px(kit::Spacing::XXS));
    insertion_->show();
    insertion_->raise();
    event->accept();
}
void TimelineLayerStack::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton)
        return;
    const int from = std::exchange(dragRow_, -1), to = std::exchange(insertionRow_, -1);
    if (insertion_)
        insertion_->hide();
    if (from < 0 || from >= rowCount() || to < 0 || to > rowCount())
        return;
    commands::Transaction transaction("Reorder Layer", dragRevision_);
    transaction.emplace<commands::MoveLayerBefore>(
        session_.compositionId(), entries_[static_cast<std::size_t>(from)].slotId,
        to == rowCount() ? std::nullopt
                         : std::optional(entries_[static_cast<std::size_t>(to)].slotId));
    (void)session_.executeTransaction(std::move(transaction));
}
void TimelineLayerStack::renameLayer(const document::LayerId layerId) {
    const auto* composition = session_.composition();
    const auto* layer = composition ? composition->graph().findLayer(layerId) : nullptr;
    if (!layer)
        return;
    const auto found = std::ranges::find(entries_, layerId, &TimelineLayerEntry::layerId);
    if (found == entries_.end())
        return;
    auto* field = new QLineEdit(QString::fromStdString(layer->name), this);
    field->setObjectName("timelineLayerRenameEditor");
    field->setGeometry(kNameCellX, rowTop(static_cast<int>(found - entries_.begin())),
                       kNameCellWidth, kTimelineRowHeight);
    const auto revision = session_.snapshot().revision();
    const auto compositionId = session_.compositionId();
    connect(field, &QLineEdit::returnPressed, this,
            [this, field, layerId, revision, compositionId] {
                commands::Transaction transaction("Rename Layer", revision);
                transaction.emplace<commands::RenameLayer>(compositionId, layerId,
                                                           field->text().toStdString());
                field->hide();
                field->deleteLater();
                (void)session_.executeTransaction(std::move(transaction));
                setFocus();
            });
    connect(field, &QLineEdit::editingFinished, field, &QObject::deleteLater);
    auto* cancel = new QAction(field);
    cancel->setShortcut(QKeySequence(Qt::Key_Escape));
    cancel->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    field->addAction(cancel);
    connect(cancel, &QAction::triggered, field, &QObject::deleteLater);
    field->show();
    field->raise();
    field->setFocus();
    field->selectAll();
}
void TimelineLayerStack::mouseDoubleClickEvent(QMouseEvent* event) {
    const int row = (static_cast<int>(event->position().y()) + scrollOffset_) / kTimelineRowHeight;
    if (event->button() == Qt::LeftButton && row >= 0 && row < rowCount() &&
        entries_[static_cast<std::size_t>(row)].rowKind == TimelineLayerEntry::Kind::Layer &&
        event->position().x() >= kNameCellX && event->position().x() < kNameCellX + kNameCellWidth)
        renameLayer(entries_[static_cast<std::size_t>(row)].layerId);
}
void TimelineLayerStack::contextMenuEvent(QContextMenuEvent* event) {
    const int row = (event->pos().y() + scrollOffset_) / kTimelineRowHeight;
    if (row < 0 || row >= rowCount() ||
        entries_[static_cast<std::size_t>(row)].rowKind != TimelineLayerEntry::Kind::Layer)
        return;
    const auto layerId = entries_[static_cast<std::size_t>(row)].layerId;
    if (!isLayerSelected(session_, layerId))
        session_.selectLayer(layerId);
    const auto compositionId = session_.compositionId();
    const auto revision = session_.snapshot().revision();
    std::set<document::NodeId> nodes;
    for (const auto& entry : entries_)
        if (isLayerSelected(session_, entry.layerId))
            if (const auto node = session_.boundaryNodeForLayer(entry.layerId))
                nodes.insert(*node);
    auto* menu = new QMenu(this);
    menu->setObjectName("timelineLayerContextMenu");
    menu->setAttribute(Qt::WA_DeleteOnClose);
    connect(menu->addAction(tr("Duplicate")), &QAction::triggered, this,
            [this, compositionId, revision, nodes] {
                commands::Transaction transaction("Duplicate Layers", revision);
                transaction.emplace<commands::DuplicateNodes>(compositionId, nodes,
                                                              document::Vec2d{0, 0});
                (void)session_.executeTransaction(std::move(transaction));
            });
    connect(menu->addAction(tr("Delete")), &QAction::triggered, this,
            [this, compositionId, revision, nodes] {
                commands::Transaction transaction("Delete Layers", revision);
                transaction.emplace<commands::RemoveNodes>(compositionId, nodes);
                (void)session_.executeTransaction(std::move(transaction));
            });
    connect(menu->addAction(tr("Rename")), &QAction::triggered, this,
            [this, layerId] { renameLayer(layerId); });
    auto* blending = menu->addMenu(tr("Blending"));
    for (const auto mode : core::kBlendModes) {
        auto* action = blending->addAction(blendModeDisplayName(mode));
        action->setCheckable(true);
        action->setChecked(session_.blendModeForLayer(layerId) == mode);
        connect(action, &QAction::triggered, this,
                [this, layerId, mode] { (void)session_.setLayerBlendMode(layerId, mode); });
    }
    auto* colors = menu->addMenu(tr("Label Color"));
    const auto setColor = [this, compositionId, revision,
                           layerId](std::optional<std::array<std::uint8_t, 3>> color) {
        commands::Transaction transaction("Set Layer Label Color", revision);
        transaction.emplace<commands::SetLayerLabelColor>(compositionId, layerId, color);
        (void)session_.executeTransaction(std::move(transaction));
    };
    connect(colors->addAction(tr("Kind Default")), &QAction::triggered, this,
            [setColor] { setColor(std::nullopt); });
    int number = 0;
    for (const auto token :
         {kit::Color::Label1, kit::Color::Label2, kit::Color::Label3, kit::Color::Label4,
          kit::Color::Label5, kit::Color::Label6, kit::Color::Label7, kit::Color::Label8}) {
        const auto color = kit::color(token);
        QPixmap swatch(kit::px(kit::Size::IconMedium), kit::px(kit::Size::IconMedium));
        swatch.fill(color);
        auto* action = colors->addAction(QIcon(swatch), tr("Label %1").arg(++number));
        connect(action, &QAction::triggered, this, [setColor, color] {
            setColor(std::array<std::uint8_t, 3>{static_cast<std::uint8_t>(color.red()),
                                                 static_cast<std::uint8_t>(color.green()),
                                                 static_cast<std::uint8_t>(color.blue())});
        });
    }
    connect(colors->addAction(tr("Custom...")), &QAction::triggered, this, [this, setColor] {
        auto* picker = new QColorDialog(this);
        picker->setObjectName("timelineLayerLabelColorDialog");
        picker->setAttribute(Qt::WA_DeleteOnClose);
        connect(picker, &QColorDialog::colorSelected, this, [setColor](const QColor& color) {
            setColor(std::array<std::uint8_t, 3>{static_cast<std::uint8_t>(color.red()),
                                                 static_cast<std::uint8_t>(color.green()),
                                                 static_cast<std::uint8_t>(color.blue())});
        });
        picker->open();
    });
    connect(menu->addAction(tr("Split at Playhead")), &QAction::triggered, this,
            [this, compositionId, layerId, revision] {
                commands::Transaction transaction("Split Layer at Playhead", revision);
                transaction.emplace<commands::SplitLayerAtTime>(compositionId, layerId,
                                                                session_.currentTime());
                (void)session_.executeTransaction(std::move(transaction));
            });
    menu->popup(event->globalPos());
}

void TimelineLayerStack::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        dragRow_ = -1;
        insertionRow_ = -1;
        if (insertion_)
            insertion_->hide();
        event->accept();
        return;
    }
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
    const int direction = event->key() == Qt::Key_Down ? 1 : -1;
    while (*target >= 0 && *target < rowCount() &&
           entries_[static_cast<std::size_t>(*target)].rowKind != TimelineLayerEntry::Kind::Layer)
        *target += direction;
    if (*target >= 0 && *target < rowCount())
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
    setFocusPolicy(Qt::StrongFocus);
    setAccessibleName(tr("Layer lanes"));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    connect(&ruler_, &TimelineRuler::axisChanged, this, qOverload<>(&TimelineLaneRegion::update));
    connect(&session_, &CompositionSession::currentTimeChanged, this,
            qOverload<>(&TimelineLaneRegion::update));
    connect(&session_, &CompositionSession::selectionChanged, this,
            qOverload<>(&TimelineLaneRegion::update));
    connect(&session_, &CompositionSession::snapshotChanged, this,
            qOverload<>(&TimelineLaneRegion::update));
    connect(&session_, &CompositionSession::compositionChanged, this,
            qOverload<>(&TimelineLaneRegion::update));
}

void TimelineLaneRegion::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (!keyframeArea_)
        return;
    keyframeArea_->setGeometry(rect());
    keyframePanel_->setGeometry(rect());
    if (keyframePanel_)
        keyframePanel_->setGridEntries(entries_, scrollOffset_);
}

int TimelineLaneRegion::contentHeight() const noexcept {
    return static_cast<int>(entries_.size()) * kTimelineRowHeight;
}

int TimelineLaneRegion::rowTop(const int row) const noexcept {
    return row * kTimelineRowHeight - scrollOffset_;
}

void TimelineLaneRegion::setEntries(std::vector<TimelineLayerEntry> entries) {
    if (!keyframePanel_) {
        keyframeArea_ = new QWidget(this);
        keyframeArea_->setObjectName("timelineKeyframeArea");
        keyframePanel_ = new TimelineKeyframePanel(session_, keyframeArea_);
        keyframePanel_->setRuler(ruler_);
        keyframePanel_->setGridEntries({}, 0);
        keyframeArea_->setGeometry(rect());
        keyframePanel_->setGeometry(rect());
    }
    drag_.reset();
    guide_.reset();
    entries_ = std::move(entries);
    if (keyframePanel_)
        keyframePanel_->setGridEntries(entries_, scrollOffset_);
    update();
}

void TimelineLaneRegion::setScrollOffset(const int offset) {
    if (offset == scrollOffset_) {
        return;
    }
    scrollOffset_ = offset;
    if (keyframePanel_)
        keyframePanel_->setGridEntries(entries_, scrollOffset_);
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
    const auto axis = ruler_.axisForWidth(width());
    if (!axis.has_value()) {
        return std::nullopt;
    }
    const auto* layer =
        composition->graph().findLayer(entries_[static_cast<std::size_t>(row)].layerId);
    if (!layer ||
        entries_[static_cast<std::size_t>(row)].rowKind != TimelineLayerEntry::Kind::Layer)
        return std::nullopt;
    const auto range =
        drag_ && drag_->layer == layer->layerId
            ? drag_->preview
            : document::WorkArea{layer->inPoint, layer->endPoint(composition->duration())};
    const qreal mappedLeft = axis->pixelForTime(range.start);
    const qreal mappedRight = axis->pixelForTime(range.end);
    if (mappedRight < 0 || mappedLeft >= width())
        return std::nullopt;
    const qreal clipMargin = kit::px(kit::Spacing::M);
    const qreal left = std::max(-clipMargin, mappedLeft);
    const qreal right = std::min(static_cast<qreal>(width()) + clipMargin, mappedRight);
    const int inset = kit::px(kit::Spacing::XS);
    const int top = rowTop(row) + inset;
    const int barWidth = std::max(1, static_cast<int>(std::lround(right - left)) + 1);
    return QRect(static_cast<int>(std::lround(left)), top, barWidth,
                 kTimelineRowHeight - 2 * inset);
}

std::vector<core::RationalTime> TimelineLaneRegion::keySummaryTimes(int row) const {
    std::set<core::RationalTime> times;
    if (row < 0 || row >= static_cast<int>(entries_.size()))
        return {};
    const auto& entry = entries_[static_cast<std::size_t>(row)];
    const auto* composition = session_.composition();
    if (!composition || entry.rowKind != TimelineLayerEntry::Kind::Layer || entry.expanded)
        return {};
    for (const auto nodeId : {session_.boundaryNodeForLayer(entry.layerId),
                              session_.directSourceNodeForLayer(entry.layerId)}) {
        const auto* node = nodeId ? composition->graph().findNode(*nodeId) : nullptr;
        if (!node)
            continue;
        for (const auto& binding : node->parameters) {
            const auto* parameter = composition->parameters().find(binding.parameterId);
            const auto* source =
                parameter ? std::get_if<document::AnimationCurveSource>(&parameter->source)
                          : nullptr;
            const auto* curve =
                source ? composition->animationCurves().find(source->curveId) : nullptr;
            if (curve)
                std::visit(
                    [&](const auto& record) {
                        for (const auto& key : record.keyframes)
                            times.insert(key.time);
                    },
                    *curve);
        }
    }
    return {times.begin(), times.end()};
}

void TimelineLaneRegion::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event)
    QPainter painter(this);
    painter.fillRect(rect(), kit::color(kit::Color::Background));

    const int firstRow = std::max(0, scrollOffset_ / kTimelineRowHeight);
    const int lastRow = std::min(static_cast<int>(entries_.size()) - 1,
                                 (scrollOffset_ + height()) / kTimelineRowHeight);
    for (int row = firstRow; row <= lastRow; ++row) {
        const auto& entry = entries_[static_cast<std::size_t>(row)];
        const int top = rowTop(row);
        painter.setRenderHint(QPainter::Antialiasing, false);
        if (isLayerSelected(session_, entry.layerId)) {
            paintSelectedRowFill(painter, top, width());
        }
        paintRowSeparator(painter, top, width());
        if (const auto bar = clipBarRect(row)) {
            painter.setRenderHint(QPainter::Antialiasing, true);
            const auto fill =
                entry.labelColor.isValid() ? entry.labelColor : kit::color(entry.clipColor);
            const auto shadow = kit::shadow(kit::Elevation::TimelineBar);
            kit::fillRoundedSurface(painter,
                                    QRectF(*bar).translated(shadow.offsetX, shadow.offsetY),
                                    shadow.color, QColor(), kit::Radius::Small);
            kit::fillRoundedSurface(painter, QRectF(*bar).adjusted(0.5, 0.5, -0.5, -0.5), fill,
                                    kit::hoverFillFor(fill), kit::Radius::Small);
            const int grip = kit::px(kit::Spacing::XXS);
            painter.fillRect(
                QRect(bar->left() + grip, bar->top() + grip, 1, bar->height() - 2 * grip),
                kit::hoverFillFor(fill));
            painter.fillRect(
                QRect(bar->right() - grip, bar->top() + grip, 1, bar->height() - 2 * grip),
                kit::hoverFillFor(fill));
        }
        if (const auto axis = ruler_.axisForWidth(width())) {
            painter.setRenderHint(QPainter::Antialiasing, true);
            painter.setPen(kit::color(kit::Color::Keyframe));
            painter.setBrush(kit::color(kit::Color::Keyframe));
            const qreal radius = kit::px(kit::Spacing::XXS) + kit::kHairlineWidth;
            for (const auto time : keySummaryTimes(row)) {
                if (time.toSeconds() < axis->t0 || time.toSeconds() >= axis->t1)
                    continue;
                const qreal x = axis->pixelForTime(time), y = top + kTimelineRowHeight / 2;
                painter.drawPolygon(QPolygonF{QPointF(x, y - radius), QPointF(x + radius, y),
                                              QPointF(x, y + radius), QPointF(x - radius, y)});
            }
        }
    }

    if (guide_) {
        if (const auto mapping = ruler_.axisForWidth(width())) {
            painter.setPen(QPen(kit::color(kit::Color::Foreground), 1, Qt::DashLine));
            const auto x = mapping->pixelForTime(*guide_);
            painter.drawLine(QPointF(x, 0), QPointF(x, height()));
        }
    }
    const auto* composition = session_.composition();
    if (composition == nullptr) {
        return;
    }
    const auto axis = ruler_.axisForWidth(width());
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
    setFocus(Qt::MouseFocusReason);
    const int row = (static_cast<int>(event->position().y()) + scrollOffset_) / kTimelineRowHeight;
    if (const auto axis = ruler_.axisForWidth(width())) {
        const auto center = rowTop(row) + kTimelineRowHeight / 2;
        if (std::abs(event->position().y() - center) <= kit::px(kit::Spacing::S))
            for (const auto time : keySummaryTimes(row)) {
                if (time.toSeconds() < axis->t0 || time.toSeconds() >= axis->t1)
                    continue;
                if (std::abs(axis->pixelForTime(time) - event->position().x()) <=
                    kit::px(kit::Spacing::S)) {
                    Q_EMIT expansionRequested(entries_[static_cast<std::size_t>(row)].layerId);
                    event->accept();
                    return;
                }
            }
    }
    const auto bar = clipBarRect(row);
    const auto* composition = session_.composition();
    const auto mapping = ruler_.axisForWidth(width());
    if (bar && bar->contains(event->position().toPoint()) && composition && mapping) {
        const auto layerId = entries_[static_cast<std::size_t>(row)].layerId;
        const auto* layer = composition->graph().findLayer(layerId);
        if (!layer)
            return;
        const auto range =
            document::WorkArea{layer->inPoint, layer->endPoint(composition->duration())};
        const bool locked = layer->locked;
        const int hit = kit::px(kit::Spacing::S);
        const auto x = event->position().x();
        const int handle = std::abs(x - bar->left()) <= hit    ? -1
                           : std::abs(x - bar->right()) <= hit ? 1
                                                               : 0;
        session_.selectLayer(layerId);
        if (!locked)
            drag_ = RangeDrag{layerId, session_.snapshot().revision(), range,
                              range,   mapping->secondsForPixel(x),    handle};
        update();
        event->accept();
        return;
    }
    ruler_.beginScrub(static_cast<int>(event->position().x()));
    event->accept();
}
void TimelineLaneRegion::mouseMoveEvent(QMouseEvent* event) {
    if (!drag_) {
        ruler_.updateScrub(static_cast<int>(event->position().x()));
        event->accept();
        return;
    }
    const auto mapping = ruler_.axisForWidth(width());
    const auto* composition = session_.composition();
    if (!mapping || !composition || session_.snapshot().revision() != drag_->revision) {
        drag_.reset();
        guide_.reset();
        update();
        return;
    }
    const double duration = composition->duration().toSeconds();
    const double frame =
        static_cast<double>(mapping->frameRate.denominator()) / mapping->frameRate.numerator();
    const double delta = mapping->secondsForPixel(event->position().x()) - drag_->pressSeconds;
    double start = drag_->original.start.toSeconds(), end = drag_->original.end.toSeconds();
    const auto minimumSpan = std::min(frame, end - start);
    if (drag_->handle < 0)
        start = std::clamp(start + delta, 0.0, end - minimumSpan);
    else if (drag_->handle > 0)
        end = std::clamp(end + delta, start + minimumSpan, duration);
    else {
        const auto shift = std::clamp(delta, -start, duration - end);
        start += shift;
        end += shift;
    }
    guide_.reset();
    if (!event->modifiers().testFlag(Qt::ShiftModifier)) {
        std::vector<core::RationalTime> targets{session_.currentTime(), session_.workArea().start,
                                                session_.workArea().end};
        for (const auto& layer : composition->graph().layerOutputs())
            if (layer.layerId != drag_->layer) {
                targets.push_back(layer.inPoint);
                targets.push_back(layer.endPoint(composition->duration()));
            }
        double best = kit::px(kit::Spacing::S), adjustment = 0;
        for (const auto target : targets) {
            for (const int edge : {-1, 1}) {
                if (drag_->handle != 0 && drag_->handle != edge)
                    continue;
                const auto seconds = edge < 0 ? start : end;
                const auto distance =
                    std::abs(mapping->pixelForTime(target) - mapping->pixelForSeconds(seconds));
                if (distance < best) {
                    best = distance;
                    adjustment = target.toSeconds() - seconds;
                    guide_ = target;
                }
            }
        }
        if (guide_) {
            if (drag_->handle <= 0)
                start += adjustment;
            if (drag_->handle >= 0)
                end += adjustment;
        }
    }
    const auto snap = [&](const double seconds) -> std::optional<core::RationalTime> {
        if (seconds >= duration)
            return composition->duration();
        if (seconds < 0)
            return std::nullopt;
        const auto index = static_cast<std::uint64_t>(std::llround(seconds / frame));
        return frameTimeForIndex(mapping->frameRate, mapping->duration,
                                 std::min(index, mapping->maxIndex));
    };
    const auto in = snap(start), out = snap(end);
    if (in && out && *in < *out && start >= 0 && end <= duration)
        drag_->preview = {*in, *out};
    update();
    event->accept();
}
void TimelineLaneRegion::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    if (drag_) {
        mouseMoveEvent(event);
        if (!drag_)
            return;
        const auto completed = *drag_;
        drag_.reset();
        guide_.reset();
        commands::Transaction transaction("Set Layer Range", completed.revision);
        transaction.emplace<commands::SetLayerRange>(session_.compositionId(), completed.layer,
                                                     completed.preview.start,
                                                     completed.preview.end);
        (void)session_.executeTransaction(std::move(transaction));
        update();
        event->accept();
        return;
    }
    ruler_.endScrub(static_cast<int>(event->position().x()));
    event->accept();
}
void TimelineLaneRegion::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        drag_.reset();
        guide_.reset();
        update();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void TimelineLaneRegion::wheelEvent(QWheelEvent* event) {
    if (ruler_.handleWheel(event)) {
        return;
    }
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

    // Standalone panels keep a local header; EditorArea takes its two cells when hosted.
    headerFallback_ = new QWidget(this);
    auto* fallbackLayout = new QHBoxLayout(headerFallback_);
    fallbackLayout->setContentsMargins(0, 0, 0, 0);
    fallbackLayout->setSpacing(0);
    createHeaderMenus();
    auto* fallbackLeft = new QWidget(headerFallback_);
    fallbackLeft->setFixedWidth(kLayerColumnWidthPx);
    auto* fallbackLeftLayout = new QHBoxLayout(fallbackLeft);
    fallbackLeftLayout->setContentsMargins(0, 0, 0, 0);
    fallbackLeftLayout->addWidget(headerMenus_);
    fallbackLayout->addWidget(fallbackLeft);
    auto* headerRow = new QWidget(headerFallback_);
    headerRight_ = headerRow;
    headerRow->setObjectName("timelineHeaderRow");
    headerRow->setFixedHeight(kit::px(kit::Size::EditorHeader));
    auto* headerLayout = new QHBoxLayout(headerRow);
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(0);

    auto* transportRow = new QWidget(this);
    auto* transportLayout = new QHBoxLayout(transportRow);
    transportLayout->setContentsMargins(0, 0, 0, 0);
    transportLayout->setSpacing(0);
    auto* controls = new QWidget(transportRow);
    controls->setObjectName("timelineControls");
    controls->setFixedWidth(kLayerColumnWidthPx);
    auto* controlsLayout = new QHBoxLayout(controls);
    controlsLayout->setContentsMargins(kColumnPadding, 0, kColumnPadding, 0);
    controlsLayout->setSpacing(kit::px(kit::Spacing::XS));

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
    // Left-aligned, in the order the design mock reads them; the trailing stretch is what makes the
    // whole cluster sit against the left edge of the column rather than spreading across it.
    controlsLayout->addWidget(stepBackButton_);
    controlsLayout->addWidget(playPauseButton_);
    controlsLayout->addWidget(stepForwardButton_);
    controlsLayout->addWidget(ramPreviewButton_);
    controlsLayout->addWidget(loopIndicator_);
    controlsLayout->addWidget(timeReadout_);
    controlsLayout->addStretch(1);

    auto* rulerColumn = new QWidget(headerRow);
    auto* rulerLayout = new QVBoxLayout(rulerColumn);
    rulerLayout->setContentsMargins(0, 0, 0, 0);
    rulerLayout->setSpacing(0);
    for (const auto& [key, start] : {std::pair{Qt::Key_B, true}, std::pair{Qt::Key_N, false}}) {
        auto* action = new QAction(this);
        action->setObjectName(start ? "timelineSetWorkAreaStartAction"
                                    : "timelineSetWorkAreaEndAction");
        action->setShortcut(QKeySequence(key));
        action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        addAction(action);
        headerRight_->addAction(action);
        connect(action, &QAction::triggered, this, [this, start] {
            auto area = session_.workArea();
            if (start)
                area.start = session_.currentTime();
            else
                area.end = session_.currentTime();
            commands::Transaction transaction("Set Work Area", session_.snapshot().revision());
            transaction.emplace<commands::SetWorkArea>(session_.compositionId(), area.start,
                                                       area.end);
            (void)session_.executeTransaction(std::move(transaction));
        });
    }
    headerRight_->addAction(splitLayerAction_);
    workArea_ = new TimelineWorkAreaRow(session_, rulerColumn);
    workArea_->setFixedHeight(kit::px(kit::Spacing::M));
    ruler_ = new TimelineRuler(session_, previewController, rulerColumn);
    ruler_->setFixedHeight(kit::px(kit::Size::EditorHeader) - workArea_->height());
    ruler_->setTimecodeLabels(timecodeFormat_);
    workArea_->setRuler(*ruler_);
    rulerLayout->addWidget(workArea_);
    rulerLayout->addWidget(ruler_);
    auto* headerGutter = new QWidget(headerRow);
    headerGutter->setObjectName("timelineHeaderScrollGutter");
    headerGutter->setFixedWidth(kScrollGutterWidth);
    headerLayout->addWidget(rulerColumn, 1);
    headerLayout->addWidget(headerGutter);
    fallbackLayout->addWidget(headerRow, 1);
    transportLayout->addWidget(controls);
    transportLayout->addWidget(new TimelineNavigator(*ruler_, transportRow), 1);
    auto* navigatorGutter = new QWidget(transportRow);
    navigatorGutter->setObjectName("timelineNavigatorScrollGutter");
    navigatorGutter->setFixedWidth(kScrollGutterWidth);
    transportLayout->addWidget(navigatorGutter);

    // Column headings start the body. Their right cell continues the header playhead into the
    // lanes, using the same viewport mapping and scrollbar gutter.
    auto* columnHeaderRow = new QWidget(this);
    columnHeaderRow->setObjectName("timelineColumnHeaderRow");
    columnHeaderRow->setFixedHeight(kit::px(kit::Size::Control));
    auto* columnHeaderLayout = new QHBoxLayout(columnHeaderRow);
    columnHeaderLayout->setContentsMargins(0, 0, 0, 0);
    columnHeaderLayout->setSpacing(0);
    columnHeaders_ = new TimelineColumnHeaders(columnHeaderRow);
    scrollBar_ = new QScrollBar(Qt::Vertical, this);
    scrollBar_->setObjectName("timelineVerticalScrollBar");
    scrollBar_->setAccessibleName(tr("Layer stack scroll"));
    auto* columnLanes = new TimelineLaneRegion(session_, *ruler_, *scrollBar_, columnHeaderRow);
    columnLanes->setObjectName("timelineColumnLaneRegion");
    auto* rulerGutter = new QWidget(columnHeaderRow);
    rulerGutter->setObjectName("timelineRulerScrollGutter");
    rulerGutter->setFixedWidth(kScrollGutterWidth);
    columnHeaderLayout->addWidget(columnHeaders_);
    columnHeaderLayout->addWidget(columnLanes, 1);
    columnHeaderLayout->addWidget(rulerGutter);

    // ---- Body: the layer column and the lane region, under ONE scrollbar ------------------------
    auto* body = new QWidget(this);
    body->setObjectName("timelineBody");
    auto* bodyLayout = new QHBoxLayout(body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(0);
    stack_ = new TimelineLayerStack(session_, *scrollBar_, body);
    lanes_ = new TimelineLaneRegion(session_, *ruler_, *scrollBar_, body);
    const auto toggleExpansion = [this](document::LayerId layer) {
        if (!expandedLayers_.erase(layer))
            expandedLayers_.insert(layer);
        rebuild();
    };
    connect(stack_, &TimelineLayerStack::expansionRequested, this, toggleExpansion);
    connect(lanes_, &TimelineLaneRegion::expansionRequested, this, toggleExpansion);
    connect(&session_, &CompositionSession::compositionChanged, this, [this] {
        expandedLayers_.clear();
        rebuild();
    });
    stack_->addAction(deleteLayerAction_);
    lanes_->addAction(deleteLayerAction_);
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

    layout->addWidget(headerFallback_);
    layout->addWidget(columnHeaderRow);
    layout->addWidget(body, 1);
    layout->addWidget(transportRow);

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

QWidget* TimelineEditor::takeHeaderMenuWidget() { return std::exchange(headerMenus_, nullptr); }

QWidget* TimelineEditor::takeHeaderRightWidget() {
    if (headerRight_ != nullptr) {
        for (auto* action : actions()) {
            if (action->shortcutContext() == Qt::WidgetWithChildrenShortcut) {
                headerRight_->addAction(action);
            }
        }
    }
    headerFallback_->hide();
    return std::exchange(headerRight_, nullptr);
}

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
            if (const auto* layer = composition->graph().findLayer(entry.layerId);
                layer && layer->labelColor) {
                const auto rgb = *layer->labelColor;
                entries.back().labelColor = QColor(rgb[0], rgb[1], rgb[2]);
            }
        }
    }
    entries = timelinePropertyEntries(session_, entries, expandedLayers_);
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
    if (std::holds_alternative<KeyframeSelection>(session_.selection().primary))
        return;
    const int top = current * kTimelineRowHeight;
    const int viewport = stack_->height();
    if (top < scrollBar_->value()) {
        scrollBar_->setValue(top);
    } else if (top + kTimelineRowHeight > scrollBar_->value() + viewport) {
        scrollBar_->setValue(top + kTimelineRowHeight - viewport);
    }
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
    if (timecodeFormat_ && context.has_value()) {
        const auto nearest = nearestFrameIndexForTime(context->frameRate, context->duration, time);
        if (nearest.has_value()) {
            frameText = formatTimelineFrameLabel(*nearest, context->frameRate, true);
        }
    }
    timeReadout_->setText((timecodeFormat_ ? tr("TC %1 · %2") : tr("Frame %1 · %2"))
                              .arg(frameText, formatExactSeconds(time)));
    timeReadout_->setToolTip(timecodeFormat_ ? tr("Non-drop timecode · exact composition time")
                                             : tr("Frame index · exact composition time"));
}

} // namespace bloom::ui
