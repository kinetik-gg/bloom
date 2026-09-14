#include <bloom/ui/editor_area.hpp>
#include <bloom/ui/kit/controls.hpp>

#include <bloom/ui/editor_registry.hpp>
#include <bloom/ui/kit/icons.hpp>
#include <bloom/ui/kit/panel_switcher.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <QAction>
#include <QChildEvent>
#include <QContextMenuEvent>
#include <QEvent>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMenu>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSize>
#include <QSizePolicy>
#include <QString>
#include <QStyle>
#include <QToolButton>
#include <QUuid>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>

namespace bloom::ui {
namespace {

// task U8, issue #131, formal amendment 2, A9: the header's one remaining icon-only button
// takes a full IconMedium glyph now (the earlier IconSmall/dense-chrome sizing read as
// illegible at the button's own corrected size). Task VIEW-1 renamed that fact into the kit's own
// Chrome icon role -- every panel-header and panel-switcher glyph is a chrome icon, and the role
// (not this file) owns both its weight and its 16 px box.
constexpr auto kHeaderIconRole = kit::IconRole::Chrome;

// task U8, issue #131, formal amendment 1, A5: the panel-switcher glyph per editor kind. Assets
// reuses Folder (already vendored for the data-kind vocabulary) rather than a duplicate asset. An
// editor id outside this table (an unavailable-editor placeholder, or a test's own probe id) gets
// no icon at all -- KPanelSwitcher's own null-QIcon default.
[[nodiscard]] std::optional<kit::IconId> iconForEditorId(const std::string& editorId) {
    if (editorId == "bloom.assets") {
        return kit::IconId::Folder;
    }
    if (editorId == "bloom.viewer") {
        return kit::IconId::Stack;
    }
    if (editorId == "bloom.timeline") {
        return kit::IconId::Clock;
    }
    if (editorId == "bloom.properties") {
        return kit::IconId::SlidersHorizontal;
    }
    if (editorId == "bloom.nodes") {
        return kit::IconId::Graph;
    }
    return std::nullopt;
}

// task U8, issue #131, fix 7 (revised by formal amendment 2, A9): the header's one remaining
// icon-only button is a plain, bordered Size::Control square -- amendment 1's "Control minus
// header padding" formula produced an illegible 16px button once Spacing::PanelHeader grew to
// 10, and A9 explicitly reverts the button's own extent to Size::Control while leaving
// Spacing::PanelHeader governing the header row's own padding (below), not the button.
constexpr int kHeaderButtonExtent = kit::px(kit::Size::Control);

// The real clip a bordered, rounded container needs (task C1, item C5; owner: "cut rounded
// corners because the background is not clipped by the panel"). QFrame#editorArea's own QSS
// border-radius already paints THIS widget's own background/border with correctly rounded
// corners -- but the header, content, and footer are ordinary rectangular children stacked on top
// of that paint, and each one's own square corner would otherwise overwrite it. A QWidget::mask()
// bitmap region was tried and rejected: building it from an integer QPolygon
// (QPainterPath::toFillPolygon().toPolygon()) rounds the rounded-rect's vertices to whole logical
// pixels before any HiDPI scaling happens, so the clip can drift or step at 125%/150% scale, and a
// mask set on an ancestor is not guaranteed to reach every kind of child window on every platform.
// Painting the correction directly is immune to both: one small, always-on-top widget per corner
// fills exactly the wedge outside the panel's own Radius::Panel arc with Color::Background -- the
// one color a rounded panel's corner always reveals in this design language -- leaving the arc's
// interior untouched so whatever is legitimately there (the header/footer's own rounded paint, or
// content within the curve) still shows through normally.
class PanelCornerMask final : public QWidget {
  public:
    enum class Corner : std::uint8_t { TopLeft, TopRight, BottomLeft, BottomRight };

    PanelCornerMask(const Corner corner, QWidget* parent) : QWidget(parent), corner_(corner) {
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
        setFocusPolicy(Qt::NoFocus);
        const int extent = kit::radiusPx(kit::Radius::Panel, 0);
        setFixedSize(extent, extent);
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const qreal r = width();
        QPointF center;
        switch (corner_) {
        case Corner::TopLeft:
            center = QPointF(r, r);
            break;
        case Corner::TopRight:
            center = QPointF(0.0, r);
            break;
        case Corner::BottomLeft:
            center = QPointF(r, 0.0);
            break;
        case Corner::BottomRight:
            center = QPointF(0.0, 0.0);
            break;
        }
        QPainterPath square;
        square.addRect(rect());
        QPainterPath arc;
        arc.addEllipse(center, r, r);
        painter.fillPath(square.subtracted(arc), kit::color(kit::Color::Background));
    }

  private:
    Corner corner_;
};

} // namespace

EditorArea::EditorArea(const EditorRegistry& registry, std::string_view initialEditorId,
                       QString areaId, QWidget* parent)
    : QFrame(parent), editorRegistry_(registry), areaId_(std::move(areaId)) {
    if (!isValidAreaId(areaId_)) {
        areaId_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    setObjectName("editorArea");
    setFrameShape(QFrame::NoFrame);
    setFocusPolicy(Qt::ClickFocus);

    layout_ = new QVBoxLayout(this);
    layout_->setContentsMargins(0, 0, 0, 0);
    layout_->setSpacing(0);

    header_ = new QWidget(this);
    header_->setObjectName("editorHeader");
    headerCellsLayout_ = new QHBoxLayout(header_);
    headerCellsLayout_->setContentsMargins(0, 0, 0, 0);
    headerCellsLayout_->setSpacing(0);
    headerLeft_ = new QWidget(header_);
    headerLeft_->setObjectName("editorHeaderLeftCell");
    headerCellsLayout_->addWidget(headerLeft_, 1);
    headerLayout_ = new QHBoxLayout(headerLeft_);
    auto*& headerLayout = headerLayout_;
    // task U8, issue #131, formal amendment 2, A10: 10px side padding (Spacing::PanelHeader,
    // reused on all four edges now that the switcher and the header button are no longer sized
    // off the header's own margins); contents are explicitly vertically centered below rather
    // than relying on margin arithmetic to land them in the 48px row (Size::EditorHeader).
    const auto headerPadding = kit::px(kit::Spacing::PanelHeader);
    headerLayout->setContentsMargins(headerPadding, 0, headerPadding, 0);
    headerLayout->setSpacing(kit::px(kit::Spacing::XXS));
    header_->setFixedHeight(kit::px(kit::Size::HeaderRow));

    // task U8, issue #131, formal amendment 2, A7/A8: a purpose-built kit switcher, not a
    // QComboBox -- the QSS-on-QComboBox approach could not render the design (no chevron
    // rendered, the shared QComboBox rule's uppercase transform leaked in, and the field
    // stretched to fill the header row). KPanelSwitcher hugs its own content and sets its own
    // Type::UI font (natural case, never UiSmall's uppercase) internally.
    editorPicker_ = new kit::KPanelSwitcher(header_);
    editorPicker_->setObjectName("editorTypePicker");

    for (const auto& editor : registry.editors()) {
        // task U8, formal amendment 1, A5 (icon) + formal amendment 2, A7 (native item icon
        // rendering via KPanelSwitcher, ported from the old QComboBox::addItem(icon, ...) call).
        const auto iconId = iconForEditorId(editor.id);
        const QIcon icon = iconId.has_value() ? kit::icon(*iconId, kHeaderIconRole) : QIcon{};
        editorPicker_->addItem(icon, editor.displayName, QString::fromStdString(editor.id));
    }

    auto* content = new QWidget(this);
    content->setObjectName("editorContent");
    content->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    contentLayout_ = new QVBoxLayout(content);
    // Content inset inside the 1px border (task C1, item C5): the editor content itself stops one
    // hairline short of the frame's own left/right/bottom edge (the top edge is already bounded by
    // the header), so its own background never draws directly on top of the frame's border stroke.
    // The actual rounded-corner clipping is a separate concern, handled by the always-on-top
    // PanelCornerMask overlays below rather than by this inset alone.
    const int hairlineInset = static_cast<int>(kit::kHairlineWidth);
    contentLayout_->setContentsMargins(hairlineInset, 0, hairlineInset, hairlineInset);
    contentLayout_->setSpacing(0);

    // The single chokepoint every header control is built through (task U1, issue #117): the
    // glyphs moved from typed characters ("H", "V", the box, the multiplication sign) to Kinetik
    // icons here, and nowhere else. Tooltips, accessible names, objectNames, and behavior are
    // unchanged -- an icon never replaces an accessible name (ADR 0010), so every one of these
    // icon-only controls still carries both. task U8, issue #131, fix 7: every header button this
    // builds is fixed at kHeaderButtonExtent square, uniformly.
    auto makeHeaderButton = [this](const kit::IconId iconId, const QString& toolTip,
                                   const QString& objectName) {
        auto* button = new kit::KIconButton(header_);
        button->setIcon(kit::icon(iconId, kHeaderIconRole));
        const int iconExtent = kit::px(kit::iconSize(kHeaderIconRole));
        button->setIconSize(QSize(iconExtent, iconExtent));
        button->setToolTip(toolTip);
        button->setAccessibleName(button->toolTip());
        button->setObjectName(objectName);
        button->setAutoRaise(true);
        button->setFixedSize(kHeaderButtonExtent, kHeaderButtonExtent);
        return button;
    };

    // The panel options menu (task U2, issue #118, decision 4; reworked task U8, issue #131,
    // fix 4): the SAME kit-styled QMenu offering all four panel operations under the SAME signals
    // as before, but no longer owned by a dedicated header button -- fix 4 removes
    // panelContextMenuButton entirely, and a right-click anywhere on the header (caught by
    // EditorArea's own eventFilter below, since header_ is one of the widgets watchForActivation()
    // installs it on) opens this exact menu at the cursor instead.
    contextMenu_ = kit::makeMenu(this);
    contextMenu_->setObjectName("panelOptionsMenu");
    auto* splitHorizontalAction = contextMenu_->addAction("Split Horizontally");
    splitHorizontalAction->setObjectName("panelSplitHorizontalAction");
    connect(splitHorizontalAction, &QAction::triggered, this,
            [this] { emit splitRequested(this, Qt::Horizontal); });
    auto* splitVerticalAction = contextMenu_->addAction("Split Vertically");
    splitVerticalAction->setObjectName("panelSplitVerticalAction");
    connect(splitVerticalAction, &QAction::triggered, this,
            [this] { emit splitRequested(this, Qt::Vertical); });
    contextMenu_->addSeparator();
    // "Maximize" reads as "Fullscreen"/"Exit Fullscreen" (fix 6's wording), kept in lockstep with
    // the header button's own tooltip by setMaximizedAppearance() below.
    auto* menuMaximizeAction = contextMenu_->addAction("Fullscreen");
    menuMaximizeAction->setObjectName("panelMaximizeAction");
    connect(menuMaximizeAction, &QAction::triggered, this,
            [this] { emit maximizeRequested(this); });
    auto* menuCloseAction = contextMenu_->addAction("Close");
    menuCloseAction->setObjectName("panelCloseAction");
    connect(menuCloseAction, &QAction::triggered, this, [this] { emit closeRequested(this); });

    // task U8, issue #131, fix 5/6: the maximize button is the ONLY remaining header button --
    // closeAreaButton is gone (closing lives only in the menu now), and this one button both
    // toggles and restores fullscreen (icon/tooltip swap in setMaximizedAppearance()).
    maximizeButton_ = makeHeaderButton(kit::IconId::Maximize, "Fullscreen", "maximizeAreaButton");

    headerLayout->addWidget(editorPicker_);
    headerLayout->addStretch(1);
    headerLayout->addWidget(maximizeButton_);
    // task U8, formal amendment 2, A10: contents vertically centered within the 48px header row.
    headerLayout->setAlignment(editorPicker_, Qt::AlignVCenter);
    headerLayout->setAlignment(maximizeButton_, Qt::AlignVCenter);

    layout_->addWidget(header_);
    layout_->addWidget(content, 1);

    // FORMAL AMENDMENT 1: the footer slot itself is built here (empty: `layout_` has header and
    // content only so far), but whether it is ever populated is entirely rebuildEditor()'s call --

    connect(editorPicker_, &kit::KPanelSwitcher::currentIndexChanged, this,
            [this](int index) { rebuildEditor(index); });
    connect(maximizeButton_, &QToolButton::clicked, this, [this] { emit maximizeRequested(this); });

    if (initialEditorId.empty()) {
        editorPicker_->setCurrentIndex(0);
    } else {
        (void)setEditorId(initialEditorId);
    }
    if (editorWidget_ == nullptr) {
        rebuildEditor(editorPicker_->currentIndex());
    }

    // The four corner-mask overlays (task C1, item C5): created last, after every layout-managed
    // child, so Qt's default stacking order already puts them on top; raise() is only a defensive
    // guarantee against a future reordering of the constructor above.
    cornerMasks_ = {new PanelCornerMask(PanelCornerMask::Corner::TopLeft, this),
                    new PanelCornerMask(PanelCornerMask::Corner::TopRight, this),
                    new PanelCornerMask(PanelCornerMask::Corner::BottomLeft, this),
                    new PanelCornerMask(PanelCornerMask::Corner::BottomRight, this)};
    for (auto* mask : cornerMasks_) {
        mask->raise();
    }
    layoutCornerMasks();

    watchForActivation(this);
    setAreaActive(false);
}

const QString& EditorArea::areaId() const noexcept { return areaId_; }

bool EditorArea::isValidAreaId(QStringView areaId) {
    const auto text = areaId.toString();
    const QUuid parsed(text);
    return !parsed.isNull() && parsed.toString(QUuid::WithoutBraces) == text.toLower();
}

std::string EditorArea::editorId() const {
    return editorPicker_->currentData().toString().toStdString();
}

bool EditorArea::setEditorId(std::string_view editorId) {
    if (editorId.empty()) {
        return false;
    }

    const auto id = QString::fromUtf8(editorId.data(), static_cast<qsizetype>(editorId.size()));
    int index = editorPicker_->findData(id);
    if (index < 0) {
        index = addUnavailableEditor(editorId);
    }

    editorPicker_->setCurrentIndex(index);
    return true;
}

void EditorArea::setAreaActive(bool active) {
    if (active_ == active) {
        return;
    }

    active_ = active;
    setProperty("active", active);
    style()->unpolish(this);
    style()->polish(this);
    update();
}

bool EditorArea::isAreaActive() const noexcept { return active_; }

void EditorArea::setSplitEnabled(bool enabled) {
    // The split actions live in the context menu now (decision 4); the button that opens the menu
    // stays enabled either way -- Maximize and Close inside it are unaffected by this flag.
    for (auto* action : {contextMenu_->findChild<QAction*>("panelSplitHorizontalAction"),
                         contextMenu_->findChild<QAction*>("panelSplitVerticalAction")}) {
        if (action != nullptr) {
            action->setEnabled(enabled);
        }
    }
}

void EditorArea::setCloseEnabled(bool enabled) {
    // task U8, issue #131, fix 5: closing lives only in the menu now -- closeAreaButton is gone,
    // but the semantics this setter promises (an artist cannot close the last remaining area)
    // stay exactly as before, just on the one remaining path.
    if (auto* action = contextMenu_->findChild<QAction*>("panelCloseAction")) {
        action->setEnabled(enabled);
    }
}

void EditorArea::setMaximizedAppearance(bool maximized) {
    // task U8, issue #131, fix 6: the maximize header button is the only remaining header
    // button, toggling AND restoring fullscreen; wording is Fullscreen/Exit Fullscreen rather than
    // Maximize/Restore, and the context menu's own action stays in lockstep with it.
    maximizeButton_->setIcon(
        kit::icon(maximized ? kit::IconId::Restore : kit::IconId::Maximize, kHeaderIconRole));
    const QString toolTip = maximized ? "Exit Fullscreen" : "Fullscreen";
    maximizeButton_->setToolTip(toolTip);
    maximizeButton_->setAccessibleName(toolTip);
    if (auto* action = contextMenu_->findChild<QAction*>("panelMaximizeAction")) {
        action->setText(toolTip);
    }
}

QSize EditorArea::minimumSizeHint() const {
    // task WIDTH-1 (owner: "min width of something like 300px in figma pixel ... instead of
    // kicking borders around"). QFrame's default minimumSizeHint() delegates to layout_'s own
    // computed minimum, which used to grow or shrink with whatever the hosted editor's own hints
    // demanded -- a Properties selection with more KValueField cells, or a longer parameter label,
    // could nudge EditorArea's reported minimum past what a narrower QSplitter pane currently had,
    // and QSplitter answers a pane's growing minimum by moving every OTHER handle in the tree to
    // make room, not just this one. Reporting a fixed PanelMinWidth-by-chrome-height floor here,
    // computed without ever consulting editorWidget_, is what keeps a selection change from moving
    // a splitter handle: this panel always claims exactly PanelMinWidth, full stop.
    //
    // Height is the header row plus the footer's own height when this editor offered one (Viewer
    // is the only one today) -- the content region itself contributes no height floor, since
    // nothing about the vertical axis was ever the bug being fixed here.
    int height = kit::px(kit::Size::EditorHeader);
    if (footer_ != nullptr) {
        height += footer_->sizeHint().height();
    }
    return {kit::px(kit::Size::PanelMinWidth), height};
}

void EditorArea::rebuildEditor(int editorIndex) {
    if (editorHost_ != nullptr) {
        contentLayout_->removeWidget(editorHost_);
        // The old editor widget (task WIDTH-1: and its QScrollArea host, when Properties gave it
        // one below -- deleting the host cascades to the editor widget it owns) is destroyed
        // BEFORE the footer it may have handed out below: Qt severs every signal connection made
        // through it as part of its own destructor, so nothing it might otherwise still notify
        // (e.g. a footer widget's repaint-on-state-change wiring) can fire against a footer that
        // is about to be deleted out from under it.
        delete editorHost_;
        editorHost_ = nullptr;
        editorWidget_ = nullptr;
    }
    if (footer_ != nullptr) {
        layout_->removeWidget(footer_);
        delete footer_;
        footer_ = nullptr;
    }
    if (headerMenus_ != nullptr) {
        headerLayout_->removeWidget(headerMenus_);
        delete headerMenus_;
        headerMenus_ = nullptr;
    }

    if (headerRight_ != nullptr) {
        headerCellsLayout_->removeWidget(headerRight_);
        delete headerRight_;
        headerRight_ = nullptr;
    }
    headerLayout_->setStretch(headerMenus_ == nullptr ? 1 : 2, 1);
    headerLeft_->setMinimumWidth(0);
    headerLeft_->setMaximumWidth(QWIDGETSIZE_MAX);
    headerCellsLayout_->setContentsMargins(0, 0, 0, 0);

    if (editorIndex < 0) {
        return;
    }

    const auto selectedEditorId = editorPicker_->itemData(editorIndex).toString().toStdString();
    const auto descriptor = std::ranges::find_if(
        editorRegistry_.editors(), [&selectedEditorId](const EditorDescriptor& candidate) {
            return candidate.id == selectedEditorId;
        });
    if (descriptor != editorRegistry_.editors().end()) {
        editorWidget_ = descriptor->create(contentLayout_->parentWidget());
    } else {
        auto* unavailable = new kit::KLabel(QStringLiteral("Editor unavailable\n\n%1")
                                                .arg(QString::fromStdString(selectedEditorId)),
                                            contentLayout_->parentWidget());
        unavailable->setObjectName("unavailableEditorPlaceholder");
        unavailable->setTextFormat(Qt::PlainText);
        unavailable->setAlignment(Qt::AlignCenter);
        editorWidget_ = unavailable;
    }
    editorWidget_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    // task WIDTH-1: Properties is the one editor that is a FORM, not a canvas -- its own natural
    // width grows with however many KValueField cells the current selection needs and how long a
    // parameter's label happens to be, and left unwrapped that width propagates straight up
    // through contentLayout_ into EditorArea's own layout-computed minimum size, which is exactly
    // what let a selection change move a QSplitter handle. Every canvas editor (node graph,
    // timeline, viewer) already scales its own content down to whatever room it gets and stays
    // hosted directly, unwrapped. Properties instead gets a QScrollArea host: Ignored on the
    // horizontal axis so the scroll area's OWN minimum width never asks contentLayout_ for more
    // than it currently has (EditorArea::minimumSizeHint() below no longer depends on this either
    // way, but the layout that actually sizes editorHost_ during a real resize does), and
    // widgetResizable so the hosted PropertiesEditor is actually resized down to the room
    // available -- shrinking its value cells toward their own floor
    // (kit::KValueField::minimumSizeHint(), Size::ValueCellMin) and eliding its row labels before
    // ever falling back to the scroll area's own horizontal scrollbar.
    if (selectedEditorId == "bloom.properties") {
        auto* scrollArea = new QScrollArea(contentLayout_->parentWidget());
        scrollArea->setObjectName(QStringLiteral("editorContentScrollArea"));
        scrollArea->setFrameShape(QFrame::NoFrame);
        scrollArea->setWidgetResizable(true);
        scrollArea->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
        scrollArea->setWidget(editorWidget_);
        editorHost_ = scrollArea;
    } else {
        editorHost_ = editorWidget_;
    }
    contentLayout_->addWidget(editorHost_);
    watchForActivation(editorWidget_);

    if (auto* provider = dynamic_cast<EditorChromeProvider*>(editorWidget_)) {
        auto& spec = provider->editorChrome();
        if (spec.hosted)
            spec.hosted();
        footer_ = buildChromeRow(spec.footer, this, true);
        if (footer_) {
            footer_->setObjectName("editorFooter");
            layout_->addWidget(footer_);
        }
        headerMenus_ = buildChromeRow(spec.header, headerLeft_);
        if (headerMenus_) {
            headerLayout_->insertWidget(1, headerMenus_, 1, Qt::AlignVCenter);
            headerLayout_->setStretch(2, 0);
        }
        if (spec.headerCanvas) {
            headerRight_ = spec.headerCanvas;
            headerRight_->setParent(header_);
            headerLeft_->setFixedWidth(spec.splitPosition());
            headerLayout_->setStretch(2, 0);
            const int inset = static_cast<int>(kit::kHairlineWidth);
            headerCellsLayout_->setContentsMargins(inset, 0, inset, 0);
            headerCellsLayout_->addWidget(headerRight_, 1);
            watchForActivation(headerRight_);
        }
    }

    // A freshly created/reparented footer is a new child of `this`, stacked above whatever
    // siblings already existed -- including the corner-mask overlays constructed once, up front.
    // Re-raising them here (a no-op the very first time, before they exist yet) keeps the
    // rounded-corner clip on top regardless of how many times the editor picker swaps footers in
    // and out.
    for (auto* mask : cornerMasks_) {
        if (mask != nullptr) {
            mask->raise();
        }
    }
}

int EditorArea::addUnavailableEditor(std::string_view editorId) {
    const auto id = QString::fromUtf8(editorId.data(), static_cast<qsizetype>(editorId.size()));
    // task U8, formal amendment 2, A7: ported verbatim from the old QComboBox path -- no icon
    // (KPanelSwitcher's null-QIcon default), and the tooltip carries through
    // setItemToolTip() exactly as QComboBox's own Qt::ToolTipRole item data did.
    editorPicker_->addItem(QIcon{}, "Editor unavailable", id);
    const int index = editorPicker_->count() - 1;
    editorPicker_->setItemToolTip(index, QStringLiteral("Unavailable editor: %1").arg(id));
    return index;
}

bool EditorArea::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::ChildAdded) {
        auto* child = static_cast<QChildEvent*>(event)->child();
        if (child != nullptr && child->isWidgetType()) {
            watchForActivation(static_cast<QWidget*>(child));
        }
    }
    // task U8, issue #131, fix 4: a right-click anywhere on the header opens panelOptionsMenu_ at
    // the cursor -- there is no longer a dedicated button that owns it. header_ is one of the
    // widgets watchForActivation() installs this filter on, so an ignored ContextMenu event that
    // bubbles up from a header child (Qt's own propagation for an unhandled QContextMenuEvent)
    // lands here exactly like one delivered to the header directly; a right-click inside the
    // switcher's OWN open popup never reaches this filter at all, since that popup is a separate
    // top-level widget the header does not parent.
    if (watched == header_ && event->type() == QEvent::ContextMenu) {
        const auto* menuEvent = static_cast<QContextMenuEvent*>(event);
        contextMenu_->popup(menuEvent->globalPos());
        return true;
    }
    if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::FocusIn) {
        emit activationRequested(this);
    }
    return QFrame::eventFilter(watched, event);
}

void EditorArea::watchForActivation(QWidget* widget) {
    widget->installEventFilter(this);
    const auto descendants = widget->findChildren<QWidget*>();
    for (auto* descendant : descendants) {
        descendant->installEventFilter(this);
    }
}

void EditorArea::resizeEvent(QResizeEvent* event) {
    QFrame::resizeEvent(event);
    layoutCornerMasks();
}

void EditorArea::layoutCornerMasks() {
    // Repositions the four PanelCornerMask overlays (task C1, item C5) to this frame's current
    // four corners -- each is a fixed Radius::Panel square, so only its position ever needs to
    // change on resize, never its size.
    const int extent = kit::radiusPx(kit::Radius::Panel, 0);
    cornerMasks_[0]->move(0, 0);
    cornerMasks_[1]->move(width() - extent, 0);
    cornerMasks_[2]->move(0, height() - extent);
    cornerMasks_[3]->move(width() - extent, height() - extent);
}

} // namespace bloom::ui
