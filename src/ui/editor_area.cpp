#include <bloom/ui/editor_area.hpp>

#include <bloom/ui/editor_registry.hpp>

#include <QChildEvent>
#include <QComboBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
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

    auto* header = new QWidget(this);
    header->setObjectName("editorHeader");
    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(6, 4, 4, 4);
    headerLayout->setSpacing(4);

    editorPicker_ = new QComboBox(header);
    editorPicker_->setObjectName("editorTypePicker");
    editorPicker_->setSizeAdjustPolicy(QComboBox::AdjustToContents);

    for (const auto& editor : registry.editors()) {
        editorPicker_->addItem(editor.displayName, QString::fromStdString(editor.id));
    }

    auto* content = new QWidget(this);
    content->setObjectName("editorContent");
    content->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    contentLayout_ = new QVBoxLayout(content);
    contentLayout_->setContentsMargins(0, 0, 0, 0);
    contentLayout_->setSpacing(0);

    auto makeHeaderButton = [header](QString text, QString toolTip, QString objectName) {
        auto* button = new QToolButton(header);
        button->setText(std::move(text));
        button->setToolTip(std::move(toolTip));
        button->setAccessibleName(button->toolTip());
        button->setObjectName(std::move(objectName));
        button->setAutoRaise(true);
        return button;
    };

    splitLeftRightButton_ = makeHeaderButton("H", "Split area left/right", "splitLeftRightButton");
    splitTopBottomButton_ = makeHeaderButton("V", "Split area top/bottom", "splitTopBottomButton");
    maximizeButton_ = makeHeaderButton("□", "Maximize area", "maximizeAreaButton");
    closeButton_ = makeHeaderButton("×", "Close area", "closeAreaButton");

    headerLayout->addWidget(editorPicker_);
    headerLayout->addStretch(1);
    headerLayout->addWidget(splitLeftRightButton_);
    headerLayout->addWidget(splitTopBottomButton_);
    headerLayout->addWidget(maximizeButton_);
    headerLayout->addWidget(closeButton_);

    layout->addWidget(header);
    layout->addWidget(content, 1);

    connect(editorPicker_, &QComboBox::currentIndexChanged, this,
            [this](int index) { rebuildEditor(index); });
    connect(splitLeftRightButton_, &QToolButton::clicked, this,
            [this] { emit splitRequested(this, Qt::Horizontal); });
    connect(splitTopBottomButton_, &QToolButton::clicked, this,
            [this] { emit splitRequested(this, Qt::Vertical); });
    connect(closeButton_, &QToolButton::clicked, this, [this] { emit closeRequested(this); });
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
    splitLeftRightButton_->setEnabled(enabled);
    splitTopBottomButton_->setEnabled(enabled);
}

void EditorArea::setCloseEnabled(bool enabled) { closeButton_->setEnabled(enabled); }

void EditorArea::setMaximizedAppearance(bool maximized) {
    maximizeButton_->setText(maximized ? "▣" : "□");
    maximizeButton_->setToolTip(maximized ? "Restore area" : "Maximize area");
    maximizeButton_->setAccessibleName(maximizeButton_->toolTip());
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

} // namespace bloom::ui
