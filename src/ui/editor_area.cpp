#include <bloom/ui/editor_area.hpp>

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
#include <QPainterPath>
#include <QRegion>
#include <QResizeEvent>
#include <QSize>
#include <QSizePolicy>
#include <QString>
#include <QStyle>
#include <QToolButton>
#include <QUuid>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <optional>
#include <utility>

namespace bloom::ui {
namespace {

// task U8, issue #131, formal amendment 2, A9: the header's one remaining icon-only button
// takes a full IconMedium glyph now (the earlier IconSmall/dense-chrome sizing read as
// illegible at the button's own corrected size).
constexpr auto kHeaderIconSize = kit::Size::IconMedium;

// task U8, issue #131, formal amendment 1, A5: the panel-switcher glyph per editor kind. Media
// reuses Folder (already vendored for the data-kind vocabulary) rather than a duplicate asset. An
// editor id outside this table (an unavailable-editor placeholder, or a test's own probe id) gets
// no icon at all -- KPanelSwitcher's own null-QIcon default.
[[nodiscard]] std::optional<kit::IconId> iconForEditorId(const std::string& editorId) {
    if (editorId == "bloom.media") {
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

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    header_ = new QWidget(this);
    header_->setObjectName("editorHeader");
    auto* headerLayout = new QHBoxLayout(header_);
    // task U8, issue #131, formal amendment 2, A10: 10px side padding (Spacing::PanelHeader,
    // reused on all four edges now that the switcher and the header button are no longer sized
    // off the header's own margins); contents are explicitly vertically centered below rather
    // than relying on margin arithmetic to land them in the 48px row (Size::EditorHeader).
    const auto headerPadding = kit::px(kit::Spacing::PanelHeader);
    headerLayout->setContentsMargins(headerPadding, 0, headerPadding, 0);
    headerLayout->setSpacing(4);

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
        const QIcon icon = iconId.has_value() ? kit::icon(*iconId, kit::Size::IconMedium) : QIcon{};
        editorPicker_->addItem(icon, editor.displayName, QString::fromStdString(editor.id));
    }

    auto* content = new QWidget(this);
    content->setObjectName("editorContent");
    content->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    contentLayout_ = new QVBoxLayout(content);
    contentLayout_->setContentsMargins(0, 0, 0, 0);
    contentLayout_->setSpacing(0);

    // The single chokepoint every header control is built through (task U1, issue #117): the
    // glyphs moved from typed characters ("H", "V", the box, the multiplication sign) to Kinetik
    // icons here, and nowhere else. Tooltips, accessible names, objectNames, and behavior are
    // unchanged -- an icon never replaces an accessible name (ADR 0010), so every one of these
    // icon-only controls still carries both. task U8, issue #131, fix 7: every header button this
    // builds is fixed at kHeaderButtonExtent square, uniformly.
    auto makeHeaderButton = [this](const kit::IconId iconId, const QString& toolTip,
                                   const QString& objectName) {
        auto* button = new QToolButton(header_);
        button->setIcon(kit::icon(iconId, kHeaderIconSize));
        button->setIconSize(QSize(kit::px(kHeaderIconSize), kit::px(kHeaderIconSize)));
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
    contextMenu_ = new QMenu(this);
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

    layout->addWidget(header_);
    layout->addWidget(content, 1);

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
        kit::icon(maximized ? kit::IconId::Restore : kit::IconId::Maximize, kHeaderIconSize));
    const QString toolTip = maximized ? "Exit Fullscreen" : "Fullscreen";
    maximizeButton_->setToolTip(toolTip);
    maximizeButton_->setAccessibleName(toolTip);
    if (auto* action = contextMenu_->findChild<QAction*>("panelMaximizeAction")) {
        action->setText(toolTip);
    }
}

void EditorArea::rebuildEditor(int editorIndex) {
    if (editorWidget_ != nullptr) {
        contentLayout_->removeWidget(editorWidget_);
        delete editorWidget_;
        editorWidget_ = nullptr;
    }

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
        auto* unavailable = new QLabel(QStringLiteral("Editor unavailable\n\n%1")
                                           .arg(QString::fromStdString(selectedEditorId)),
                                       contentLayout_->parentWidget());
        unavailable->setObjectName("unavailableEditorPlaceholder");
        unavailable->setTextFormat(Qt::PlainText);
        unavailable->setAlignment(Qt::AlignCenter);
        editorWidget_ = unavailable;
    }
    editorWidget_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    contentLayout_->addWidget(editorWidget_);
    watchForActivation(editorWidget_);
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
    updateRoundedMask();
}

void EditorArea::updateRoundedMask() {
    // Rounded corners over the Background gutter (task U2, issue #118, decision 4; shrunk to
    // Radius::Small by task U8, issue #131, fix 2; moved to its own Radius::Panel = 4 by formal
    // amendment 1, A3): a real clip rather than only the stylesheet's own border-radius, so the
    // header's Surface background and whatever the active editor draws never overhang the
    // panel's rounded corners -- the QFrame's own CSS border-radius (kinetikStyleSheet()'s
    // QFrame#editorArea rule) only ever paints the frame's OWN background/border, never its
    // children.
    QPainterPath path;
    const int radius = kit::radiusPx(kit::Radius::Panel, 0);
    path.addRoundedRect(rect(), radius, radius);
    setMask(QRegion(path.toFillPolygon().toPolygon()));
}

} // namespace bloom::ui
