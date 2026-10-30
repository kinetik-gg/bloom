#include <bloom/ui/context_bar.hpp>

#include "context_bar_options.hpp"

#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/editor_area.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/icons.hpp>
#include <bloom/ui/kit/painting.hpp>
#include <bloom/ui/kit/surfaces.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/layer_alignment_actions.hpp>
#include <bloom/ui/viewer_editor.hpp>

#include <QAction>
#include <QContextMenuEvent>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QKeyEvent>
#include <QMenu>
#include <QSignalBlocker>
#include <QWidgetAction>

namespace bloom::ui {

namespace {

using commands::AlignmentTarget;
using commands::LayerArrangement;

kit::IconId toolIcon(const ViewerTool tool) {
    switch (tool) {
    case ViewerTool::Select:
        return kit::IconId::Select;
    case ViewerTool::Hand:
        return kit::IconId::Pan;
    case ViewerTool::Zoom:
        return kit::IconId::Zoom;
    case ViewerTool::Rectangle:
        return kit::IconId::Rectangle;
    case ViewerTool::Ellipse:
        return kit::IconId::Ellipse;
    case ViewerTool::Polygon:
        return kit::IconId::Polygon;
    case ViewerTool::Star:
        return kit::IconId::Star;
    case ViewerTool::Line:
        return kit::IconId::Line;
    case ViewerTool::Pen:
        return kit::IconId::Pen;
    case ViewerTool::Text:
        return kit::IconId::Text;
    case ViewerTool::Cryptomatte:
        return kit::IconId::Eyedropper;
    }
    return kit::IconId::Select;
}

QString toolObjectName(const ViewerTool tool) {
    const auto stem = viewerToolInfo(tool).objectStem;
    return QStringLiteral("contextBarTool") +
           QString::fromLatin1(stem.data(), static_cast<qsizetype>(stem.size()));
}

QWidget* group(QWidget* parent, const QString& name) {
    auto* widget = new QWidget(parent);
    widget->setObjectName(name);
    auto* layout = new QHBoxLayout(widget);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(kit::px(kit::Spacing::ChromeGap));
    return widget;
}

} // namespace

ContextBar::ContextBar(CompositionSession& session, CompositionPreviewController* preview,
                       QWidget* parent)
    : QWidget(parent), session_(session), preview_(preview),
      alignment_(new LayerAlignmentActions(session, preview, this)) {
    setObjectName(QStringLiteral("contextBar"));
    setAccessibleName(tr("Toolbar"));
    setFixedHeight(kit::px(kit::Size::ContextBarHeight));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    buildTools();
    EditorChromeRowSpec spec;
    spec.objectName = QStringLiteral("contextBarRow");
    spec.overflowButtonName = QStringLiteral("contextBarOverflowButton");
    spec.overflowMenuName = QStringLiteral("contextBarOverflowMenu");
    spec.overflowIcon = kit::IconId::Overflow;
    spec.overflowToolTip = tr("More of the toolbar");
    spec.owner = this;
    spec.addWidget(tools_);

    // Middle: the active tool's options, each group also offered whole in the overflow menu.
    const auto viewerSource = [this] { return viewer_.data(); };
    const auto committed = [this] { returnFocusToViewer(); };
    const auto overflowGroup = [this](QWidget* widget, const QString& name) {
        auto* action = new QWidgetAction(this);
        action->setText(name);
        action->setDefaultWidget(widget);
        return action;
    };
    anchorOptions_ = new AnchorModeOptions(viewerSource, this);
    auto* anchorOverflow = new AnchorModeOptions(viewerSource, nullptr);
    shapeOptions_ = new ShapeStyleOptions(session_, committed, this);
    auto* shapeOverflow = new ShapeStyleOptions(session_, committed, nullptr);
    textOptions_ = new TextStyleOptions(session_, committed, this);
    auto* textOverflow = new TextStyleOptions(session_, committed, nullptr);
    for (auto* options : {anchorOptions_, shapeOptions_, textOptions_})
        spec.addWidget(options);
    spec.setCollapse(anchorOptions_, 5, overflowGroup(anchorOverflow, tr("Anchor handle mode")));
    spec.setCollapse(shapeOptions_, 4, overflowGroup(shapeOverflow, tr("Fill and stroke")));
    spec.setCollapse(textOptions_, 4, overflowGroup(textOverflow, tr("Text")));
    anchorOverflows_ = {anchorOptions_, anchorOverflow};

    // Right: the selection's actions. Distribute goes first, then Align To, then Align.
    spec.addStretch();
    auto* align = buildAlignment(this, false);
    auto* distribute = buildAlignment(this, true);
    auto* target = buildAlignTarget(this);
    spec.addWidget(align);
    spec.addWidget(distribute);
    spec.addWidget(target);
    const auto submenu = [this](const QString& title, const std::vector<QAction*>& actions) {
        auto* menu = kit::makeMenu(title, this);
        for (auto* action : actions)
            menu->addAction(action);
        return menu->menuAction();
    };
    using A = LayerArrangement;
    spec.setCollapse(distribute, 1,
                     submenu(tr("Distribute"), {alignment_->action(A::DistributeHorizontalCentres),
                                                alignment_->action(A::DistributeVerticalCentres)}));
    spec.setCollapse(target, 2,
                     submenu(tr("Align To"), {alignment_->targetAction(AlignmentTarget::Composition),
                                              alignment_->targetAction(AlignmentTarget::Selection),
                                              alignment_->targetAction(AlignmentTarget::KeyObject)}));
    spec.setCollapse(align, 3,
                     submenu(tr("Align"), {alignment_->action(A::AlignLeft),
                                           alignment_->action(A::AlignHorizontalCentre),
                                           alignment_->action(A::AlignRight),
                                           alignment_->action(A::AlignTop),
                                           alignment_->action(A::AlignVerticalMiddle),
                                           alignment_->action(A::AlignBottom)}));

    // A Surface strip holding the row inside ChromePadding, and one Border hairline separating the
    // bar from the workspace below it (inside the bottom padding).
    auto* strip = new kit::KSurface(this, kit::Color::Surface);
    row_ = EditorArea::buildChromeRow(spec, strip);
    auto* rowLayout = new QHBoxLayout(strip);
    const int padding = kit::px(kit::Spacing::ChromePadding);
    rowLayout->setContentsMargins(padding, padding, padding,
                                  padding - kit::px(kit::Size::Hairline));
    rowLayout->setSpacing(0);
    rowLayout->addWidget(row_);
    auto* hairline = new kit::KSurface(this, kit::Color::Border);
    hairline->setObjectName(QStringLiteral("contextBarHairline"));
    hairline->setFixedHeight(kit::px(kit::Size::Hairline));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(strip);
    layout->addWidget(hairline);
    if (auto* overflow = row_->findChild<QToolButton*>(spec.overflowButtonName))
        overflow->setFocusPolicy(Qt::NoFocus);

    connect(&session_.tools(), &ToolController::toolChanged, this, [this] {
        refreshTools();
        refreshOptions();
    });
    connect(alignment_, &LayerAlignmentActions::notice, this, &ContextBar::notice);
    connect(alignment_, &LayerAlignmentActions::changed, this, &ContextBar::refreshAlignTarget);
    refreshTools();
    refreshOptions();
    refreshAlignTarget();
}

ContextBar::~ContextBar() { disconnect(viewerConnection_); }

void ContextBar::buildTools() {
    tools_ = group(this, QStringLiteral("contextBarTools"));
    auto* layout = static_cast<QHBoxLayout*>(tools_->layout());
    const auto addTool = [this, layout](const ViewerTool tool) {
        auto* button = new kit::KIconToggle(toolIcon(tool), tools_);
        button->setObjectName(toolObjectName(tool));
        button->setToolTip(viewerToolTip(tool));
        button->setAccessibleName(viewerToolInfo(tool).name);
        button->setFocusPolicy(Qt::NoFocus);
        connect(button, &QAbstractButton::clicked, this, [this, tool] {
            session_.tools().setTool(tool);
            refreshTools();
        });
        layout->addWidget(button);
        toolButtons_[tool] = button;
        return button;
    };
    const auto addGap = [layout] {
        layout->addSpacing(kit::px(kit::Spacing::ChromeGroupGap) - kit::px(kit::Spacing::ChromeGap));
    };
    addTool(ViewerTool::Select);
    addTool(ViewerTool::Hand);
    addTool(ViewerTool::Zoom);
    addGap();

    // Rectangle, Ellipse, Polygon and Star share one slot showing the last one chosen; a press and
    // hold, or a right-click, offers all four (After Effects' shape tool slot).
    shapeSlot_ = new kit::KIconToggle(toolIcon(ViewerTool::Rectangle), tools_);
    shapeSlot_->setObjectName(QStringLiteral("contextBarToolShape"));
    shapeSlot_->setFocusPolicy(Qt::NoFocus);
    shapeFlyout_ = kit::makeMenu(this);
    shapeFlyout_->setObjectName(QStringLiteral("contextBarShapeFlyout"));
    for (const auto tool : {ViewerTool::Rectangle, ViewerTool::Ellipse, ViewerTool::Polygon,
                            ViewerTool::Star}) {
        const auto info = viewerToolInfo(tool);
        auto* action = shapeFlyout_->addAction(kit::icon(toolIcon(tool), kit::IconRole::Chrome),
                                               info.name);
        action->setObjectName(toolObjectName(tool));
        if (!info.key.isEmpty())
            action->setShortcut(QKeySequence(info.key));
        // Display only: the key belongs to the app-wide tool keys, never to this menu.
        action->setShortcutContext(Qt::WidgetShortcut);
        action->setCheckable(true);
        connect(action, &QAction::triggered, this, [this, tool] {
            session_.tools().setTool(tool);
            refreshTools();
        });
    }
    shapeSlot_->setMenu(shapeFlyout_);
    shapeSlot_->setPopupMode(QToolButton::DelayedPopup);
    shapeSlot_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(shapeSlot_, &QWidget::customContextMenuRequested, this, [this] {
        shapeFlyout_->popup(shapeSlot_->mapToGlobal(QPoint(0, shapeSlot_->height())));
    });
    connect(shapeSlot_, &QAbstractButton::clicked, this, [this] {
        session_.tools().setTool(session_.tools().lastShapeTool());
        refreshTools();
    });
    shapeSlot_->setProperty("flyoutMark", true);
    layout->addWidget(shapeSlot_);
    addTool(ViewerTool::Line);
    addTool(ViewerTool::Pen);
    addTool(ViewerTool::Text);
    addGap();
    addTool(ViewerTool::Cryptomatte);
    tools_->setFixedWidth(tools_->sizeHint().width());
}

QWidget* ContextBar::buildAlignment(QWidget* parent, const bool distribute) {
    auto* widget = group(parent, distribute ? QStringLiteral("contextBarDistribute")
                                            : QStringLiteral("contextBarAlign"));
    widget->setAccessibleName(distribute ? tr("Distribute") : tr("Align"));
    auto* layout = static_cast<QHBoxLayout*>(widget->layout());
    // Align, Distribute and Align To read as three groups: a group gap, not a control gap, between.
    if (distribute)
        layout->addSpacing(kit::px(kit::Spacing::ChromeGroupGap) - kit::px(kit::Spacing::ChromeGap));
    using A = LayerArrangement;
    const auto arrangements =
        distribute ? std::vector<A>{A::DistributeHorizontalCentres, A::DistributeVerticalCentres}
                   : std::vector<A>{A::AlignLeft,   A::AlignHorizontalCentre, A::AlignRight,
                                    A::AlignTop,    A::AlignVerticalMiddle,   A::AlignBottom};
    for (const auto arrangement : arrangements) {
        auto* action = alignment_->action(arrangement);
        auto* button = new kit::KIconButton(widget);
        button->setDefaultAction(action);
        button->setObjectName(QStringLiteral("contextBar-") + action->objectName());
        button->setAccessibleName(action->text());
        button->setFocusPolicy(Qt::NoFocus);
        button->setFixedSize(kit::px(kit::Size::Control), kit::px(kit::Size::Control));
        const int box = kit::px(kit::iconSize(kit::IconRole::Chrome));
        button->setIconSize(QSize(box, box));
        layout->addWidget(button);
        // Between the horizontal and the vertical alignments.
        if (arrangement == A::AlignRight)
            layout->addSpacing(kit::px(kit::Spacing::S));
    }
    widget->setFixedWidth(widget->sizeHint().width());
    return widget;
}

QWidget* ContextBar::buildAlignTarget(QWidget* parent) {
    auto* widget = group(parent, QStringLiteral("contextBarAlignTarget"));
    auto* layout = static_cast<QHBoxLayout*>(widget->layout());
    layout->addSpacing(kit::px(kit::Spacing::ChromeGroupGap) - kit::px(kit::Spacing::ChromeGap));
    auto* label = new kit::KLabel(tr("Align To"), widget, kit::TypeRole::UiSmall);
    label->setTone(kit::Color::Muted);
    alignTarget_ = new kit::KDropdown(widget);
    alignTarget_->setObjectName(QStringLiteral("contextBarAlignTo"));
    alignTarget_->setAccessibleName(tr("Align To"));
    alignTarget_->setFocusPolicy(Qt::ClickFocus);
    alignTarget_->addItem(tr("Composition"), static_cast<int>(AlignmentTarget::Composition));
    alignTarget_->addItem(tr("Selection"), static_cast<int>(AlignmentTarget::Selection));
    alignTarget_->addItem(tr("Key Object"), static_cast<int>(AlignmentTarget::KeyObject));
    // One width whatever the key layer is called: a long key name elides, and the tooltip names it.
    alignTarget_->setWidthFloor(kit::px(kit::Size::DropdownWidthWide));
    alignTarget_->setFixedWidth(kit::px(kit::Size::DropdownWidthWide));
    layout->addWidget(label);
    layout->addWidget(alignTarget_);
    connect(alignTarget_, &kit::KDropdown::activated, this, [this](const int index) {
        session_.tools().setAlignmentTarget(
            static_cast<AlignmentTarget>(alignTarget_->itemData(index).toInt()));
        returnFocusToViewer();
    });
    return widget;
}

void ContextBar::setViewer(ViewerEditor* viewer) {
    if (viewer_ == viewer)
        return;
    disconnect(viewerConnection_);
    viewer_ = viewer;
    if (viewer != nullptr)
        viewerConnection_ = connect(viewer, &ViewerEditor::anchorModeStateChanged, this, [this] {
            for (auto* options : anchorOverflows_)
                static_cast<AnchorModeOptions*>(options)->refresh();
        });
    for (auto* options : anchorOverflows_)
        static_cast<AnchorModeOptions*>(options)->refresh();
}

kit::KIconToggle* ContextBar::toolButtonForTest(const ViewerTool tool) const {
    if (isShapeTool(tool))
        return shapeSlot_;
    const auto found = toolButtons_.find(tool);
    return found != toolButtons_.end() ? found->second : nullptr;
}

void ContextBar::refreshTools() {
    const auto tool = session_.tools().tool();
    for (const auto& [choice, button] : toolButtons_)
        button->setChecked(choice == tool);
    const auto shape = session_.tools().lastShapeTool();
    shapeSlot_->setGlyph(toolIcon(shape));
    shapeSlot_->setChecked(isShapeTool(tool));
    shapeSlot_->setToolTip(viewerToolTip(shape) +
                           tr("\nPress and hold, or right-click, for the other shapes"));
    shapeSlot_->setAccessibleName(viewerToolInfo(shape).name);
    for (auto* action : shapeFlyout_->actions())
        action->setChecked(action->objectName() == toolObjectName(tool));
}

void ContextBar::refreshOptions() {
    const auto tool = session_.tools().tool();
    const auto show = [](QWidget* options, const bool shown) {
        options->setProperty("chromeSuppressed", !shown);
    };
    show(anchorOptions_, tool == ViewerTool::Select || tool == ViewerTool::Pen);
    show(shapeOptions_, drawsShape(tool));
    show(textOptions_, tool == ViewerTool::Text);
}

void ContextBar::refreshAlignTarget() {
    if (alignTarget_ == nullptr)
        return;
    const QSignalBlocker blocker(alignTarget_);
    const auto key = alignment_->keyLayerName();
    alignTarget_->setItemText(2, key.isEmpty() ? tr("Key Object") : tr("Key Object (%1)").arg(key));
    alignTarget_->setCurrentIndex(static_cast<int>(session_.tools().alignmentTarget()));
    alignTarget_->setToolTip(
        key.isEmpty() ? tr("What the selected layers line up with")
                      : tr("What the selected layers line up with\nKey layer: %1").arg(key));
}

void ContextBar::returnFocusToViewer() {
    if (viewer_ != nullptr && viewer_->isVisible())
        viewer_->setFocus(Qt::OtherFocusReason);
}

void ContextBar::keyPressEvent(QKeyEvent* event) {
    // The bar shares the app-wide tool keys with the Viewers.
    if (event->modifiers() == Qt::NoModifier)
        if (const auto tool = toolForKey(event->key())) {
            session_.tools().setTool(*tool);
            event->accept();
            return;
        }
    QWidget::keyPressEvent(event);
}

} // namespace bloom::ui
