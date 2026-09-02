#include <bloom/ui/editor_area.hpp>

#include <bloom/ui/editor_registry.hpp>
#include <bloom/ui/kit/icons.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <QAction>
#include <QChildEvent>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QEvent>
#include <QHBoxLayout>
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
#include <utility>

namespace bloom::ui {
namespace {

// The panel header is a dense chrome row, so its icon-only controls take the smallest icon box.
constexpr auto kHeaderIconSize = kit::Size::IconSmall;

// task U8, issue #131, fix 7: "the header QToolButtons must also be square: fixed size
// Size::Control minus header padding, uniform." The header row reserves 4 design pixels of
// vertical padding above and below its controls (headerLayout's own setContentsMargins(6, 4, 4,
// 4) below), so a header button's square extent is the Control token less that one padding unit.
constexpr int kHeaderVerticalPadding = 4;
constexpr int kHeaderButtonExtent = kit::px(kit::Size::Control) - kHeaderVerticalPadding;

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
    headerLayout->setContentsMargins(6, kHeaderVerticalPadding, 4, kHeaderVerticalPadding);
    headerLayout->setSpacing(4);

    editorPicker_ = new QComboBox(header_);
    editorPicker_->setObjectName("editorTypePicker");
    editorPicker_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    // The panel header's title styling (task U2, issue #118, decision 4): UiSmall already IS
    // "uppercase, +0.07em tracking" (kit::font()'s own recipe for the role), so giving the
    // switcher this role is the whole job -- no separate title label, no per-character transform.
    // Stays a QComboBox rather than becoming a KDropdown (decision 4's own named alternative):
    // this control also carries the "unavailable editor" placeholder path below via
    // QComboBox::findData()/setItemData(..., Qt::ToolTipRole), neither of which KDropdown exposes,
    // and switching its type would silently break every existing "editorTypePicker" QComboBox*
    // test contract outside this task's sanctioned change.
    editorPicker_->setFont(kit::font(kit::TypeRole::UiSmall));

    for (const auto& editor : registry.editors()) {
        editorPicker_->addItem(editor.displayName, QString::fromStdString(editor.id));
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

    layout->addWidget(header_);
    layout->addWidget(content, 1);

    connect(editorPicker_, &QComboBox::currentIndexChanged, this,
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
    editorPicker_->addItem("Editor unavailable", id);
    const int index = editorPicker_->count() - 1;
    editorPicker_->setItemData(index, QStringLiteral("Unavailable editor: %1").arg(id),
                               Qt::ToolTipRole);
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
    // Radius::Small rounded corners over the Background gutter (task U2, issue #118, decision 4;
    // shrunk to the smallest panel radius by task U8, issue #131, fix 2): a real clip rather than
    // only the stylesheet's own border-radius, so the header's Surface background and whatever the
    // active editor draws never overhang the panel's rounded corners -- the QFrame's own CSS
    // border-radius (kinetikStyleSheet()'s QFrame#editorArea rule) only ever paints the frame's
    // OWN background/border, never its children.
    QPainterPath path;
    const int radius = kit::radiusPx(kit::Radius::Small, 0);
    path.addRoundedRect(rect(), radius, radius);
    setMask(QRegion(path.toFillPolygon().toPolygon()));
}

} // namespace bloom::ui
