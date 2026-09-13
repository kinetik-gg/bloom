#include <bloom/ui/assets_editor.hpp>

#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/project.hpp>
#include <bloom/ui/composition_session.hpp>

#include <QAction>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <QVariant>

#include <cstdint>
#include <optional>

namespace bloom::ui {
namespace {

constexpr int kCompositionIdRole = Qt::UserRole + 1;

[[nodiscard]] document::CompositionId compositionIdForItem(const QTreeWidgetItem* item) {
    if (item == nullptr) {
        return {};
    }
    return document::CompositionId::fromRaw(item->data(0, kCompositionIdRole).toULongLong());
}

[[nodiscard]] document::CompositionId lowestCompositionId(const document::Project& project) {
    const auto compositions = project.compositions();
    if (compositions.empty()) {
        return {};
    }
    auto lowest = compositions.front().id();
    for (const auto& composition : compositions) {
        if (composition.id().value() < lowest.value()) {
            lowest = composition.id();
        }
    }
    return lowest;
}

void setDisabledReason(QWidget* widget, const QString& reason) {
    widget->setEnabled(false);
    widget->setToolTip(reason);
}

void setDisabledReason(QAction* action, const QString& reason) {
    action->setEnabled(false);
    action->setToolTip(reason);
}

} // namespace

AssetsEditor::AssetsEditor(CompositionSession& session, QWidget* parent)
    : QWidget(parent), session_(session) {
    setObjectName(QStringLiteral("assetsEditor"));
    setAccessibleName(tr("Project assets editor"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(7);

    search_ = new QLineEdit(this);
    search_->setObjectName(QStringLiteral("assetsSearchField"));
    search_->setAccessibleName(tr("Search assets"));
    search_->setPlaceholderText(tr("Search assets…"));
    layout->addWidget(search_);

    tree_ = new QTreeWidget(this);
    tree_->setObjectName(QStringLiteral("assetsTree"));
    tree_->setAccessibleName(tr("Assets"));
    tree_->setHeaderLabels({tr("Name"), tr("Kind")});
    tree_->setRootIsDecorated(false);
    tree_->setUniformRowHeights(true);
    tree_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    tree_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    tree_->header()->setStretchLastSection(false);
    tree_->setColumnWidth(0, 220);
    layout->addWidget(tree_, 1);

    connect(search_, &QLineEdit::textChanged, this, &AssetsEditor::applyFilter);
    connect(tree_, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem* item, const int column) {
                if (column == 0) {
                    openComposition(compositionIdForItem(item));
                }
            });
    connect(tree_, &QTreeWidget::itemChanged, this,
            [this](QTreeWidgetItem* item, const int column) { commitRename(item, column); });
    connect(tree_, &QTreeWidget::customContextMenuRequested, this,
            [this](const QPoint position) { showContextMenu(position); });

    connect(&session_, &CompositionSession::snapshotChanged, this, &AssetsEditor::rebuild);
    connect(&session_, &CompositionSession::compositionChanged, this,
            &AssetsEditor::updateSelection);

    buildHeaderMenus();
    buildFooter();
    rebuild();
}

void AssetsEditor::rebuild() {
    rebuilding_ = true;
    const QSignalBlocker blocker(tree_);
    tree_->clear();
    for (const auto& composition : session_.snapshot().project().compositions()) {
        auto* item = new QTreeWidgetItem(tree_);
        item->setText(0, QString::fromStdString(composition.name()));
        item->setText(1, tr("Composition"));
        item->setData(0, kCompositionIdRole,
                      QVariant::fromValue<qulonglong>(composition.id().value()));
        item->setToolTip(0, tr("Composition %1").arg(composition.id().value()));
        item->setFlags(item->flags() | Qt::ItemIsEditable);
    }
    applyFilter(search_->text());
    updateSelection();
    rebuilding_ = false;
}

void AssetsEditor::updateSelection() {
    if (tree_ == nullptr) {
        return;
    }
    const QSignalBlocker blocker(tree_);
    tree_->clearSelection();
    for (int index = 0; index < tree_->topLevelItemCount(); ++index) {
        auto* item = tree_->topLevelItem(index);
        if (!item->isHidden() && compositionIdForItem(item) == session_.compositionId()) {
            tree_->setCurrentItem(item);
            item->setSelected(true);
            return;
        }
    }
    tree_->setCurrentItem(nullptr);
}

void AssetsEditor::applyFilter(const QString& text) {
    if (tree_ == nullptr) {
        return;
    }
    const QString query = text.trimmed();
    for (int index = 0; index < tree_->topLevelItemCount(); ++index) {
        auto* item = tree_->topLevelItem(index);
        item->setHidden(!query.isEmpty() && !item->text(0).contains(query, Qt::CaseInsensitive));
    }
    if (!rebuilding_) {
        updateSelection();
    }
}

void AssetsEditor::openComposition(const document::CompositionId id) {
    if (id.isValid()) {
        (void)session_.setComposition(id);
    }
}

void AssetsEditor::commitRename(QTreeWidgetItem* item, const int column) {
    if (rebuilding_ || item == nullptr || column != 0) {
        return;
    }
    const auto id = compositionIdForItem(item);
    if (!id.isValid()) {
        return;
    }
    commands::Transaction transaction("Rename Composition", session_.snapshot().revision());
    transaction.emplace<commands::SetCompositionName>(id, item->text(0).toStdString());
    const auto result = session_.executeTransaction(std::move(transaction));
    if (!result.succeeded()) {
        rebuild();
    }
}

void AssetsEditor::showContextMenu(const QPoint position) {
    auto* item = tree_->itemAt(position);
    if (item == nullptr) {
        return;
    }
    tree_->setCurrentItem(item);
    const auto id = compositionIdForItem(item);

    QMenu menu(this);
    auto* openAction = menu.addAction(tr("Open"));
    openAction->setObjectName(QStringLiteral("assetsOpenAction"));
    connect(openAction, &QAction::triggered, this, [this, id] { openComposition(id); });
    auto* renameAction = menu.addAction(tr("Rename"));
    renameAction->setObjectName(QStringLiteral("assetsRenameAction"));
    connect(renameAction, &QAction::triggered, this, [this, item] { tree_->editItem(item, 0); });
    auto* duplicateAction = menu.addAction(tr("Duplicate"));
    duplicateAction->setObjectName(QStringLiteral("assetsDuplicateAction"));
    connect(duplicateAction, &QAction::triggered, this, [this, id] { duplicateComposition(id); });
    auto* deleteAction = menu.addAction(tr("Delete"));
    deleteAction->setObjectName(QStringLiteral("assetsDeleteAction"));
    connect(deleteAction, &QAction::triggered, this, [this, id] { deleteComposition(id); });
    menu.exec(tree_->viewport()->mapToGlobal(position));
}

void AssetsEditor::duplicateComposition(const document::CompositionId id) {
    if (!id.isValid()) {
        return;
    }
    commands::Transaction transaction("Duplicate Composition", session_.snapshot().revision());
    transaction.emplace<commands::DuplicateComposition>(id);
    const auto result = session_.executeTransaction(std::move(transaction));
    const auto copyId =
        result.outputId<document::CompositionId>(commands::kDuplicateCompositionOutput);
    if (result.succeeded() && copyId.has_value()) {
        openComposition(*copyId);
    }
}

void AssetsEditor::deleteComposition(const document::CompositionId id) {
    if (!id.isValid()) {
        return;
    }
    const bool wasActive = session_.compositionId() == id;
    commands::Transaction transaction("Delete Composition", session_.snapshot().revision());
    transaction.emplace<commands::DeleteComposition>(id);
    const auto result = session_.executeTransaction(std::move(transaction));
    if (!result.succeeded() || !wasActive) {
        return;
    }
    openComposition(lowestCompositionId(session_.snapshot().project()));
}

void AssetsEditor::showNewCompositionDialog() {
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("newCompositionDialog"));
    dialog.setWindowTitle(tr("New Composition"));

    auto* form = new QFormLayout(&dialog);
    auto* name = new QLineEdit(&dialog);
    name->setObjectName(QStringLiteral("assetsNameField"));
    name->setText(
        tr("Composition %1").arg(session_.snapshot().project().compositions().size() + 1));
    form->addRow(tr("Name"), name);

    const auto* const current = session_.composition();
    const auto currentFormat =
        current == nullptr ? document::CompositionFormat{} : current->format();
    auto* width = new QSpinBox(&dialog);
    width->setObjectName(QStringLiteral("assetsWidthField"));
    width->setRange(1, static_cast<int>(document::CompositionFormat::kMaximumDimension));
    width->setValue(static_cast<int>(currentFormat.width()));
    form->addRow(tr("Width"), width);
    auto* height = new QSpinBox(&dialog);
    height->setObjectName(QStringLiteral("assetsHeightField"));
    height->setRange(1, static_cast<int>(document::CompositionFormat::kMaximumDimension));
    height->setValue(static_cast<int>(currentFormat.height()));
    form->addRow(tr("Height"), height);
    auto* frameRate = new QSpinBox(&dialog);
    frameRate->setObjectName(QStringLiteral("assetsFrameRateField"));
    frameRate->setRange(1, 1000);
    frameRate->setValue(static_cast<int>(currentFormat.frameRate().numerator() /
                                         currentFormat.frameRate().denominator()));
    form->addRow(tr("Frame rate (fps)"), frameRate);
    auto* duration = new QSpinBox(&dialog);
    duration->setObjectName(QStringLiteral("assetsDurationField"));
    duration->setRange(1, 1'000'000);
    duration->setValue(current == nullptr ? 10
                                          : static_cast<int>(current->duration().numerator() /
                                                             current->duration().denominator()));
    form->addRow(tr("Duration (frames)"), duration);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->setObjectName(QStringLiteral("assetsDialogButtons"));
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    const auto rate =
        document::FrameRate::create(static_cast<std::uint32_t>(frameRate->value()), 1);
    const auto format =
        rate.has_value()
            ? document::CompositionFormat::create(static_cast<std::uint32_t>(width->value()),
                                                  static_cast<std::uint32_t>(height->value()),
                                                  currentFormat.pixelAspect(), *rate)
            : std::nullopt;
    if (!format.has_value()) {
        return;
    }
    commands::Transaction transaction("Add Composition", session_.snapshot().revision());
    transaction.emplace<commands::AddComposition>(
        name->text().toStdString(), *format, *rate,
        core::RationalTime::fromInteger(duration->value()));
    const auto result = session_.executeTransaction(std::move(transaction));
    const auto id = result.outputId<document::CompositionId>(commands::kAddCompositionOutput);
    if (result.succeeded() && id.has_value()) {
        openComposition(*id);
    }
}

void AssetsEditor::buildHeaderMenus() {
    auto* bar = new QMenuBar(this);
    bar->setObjectName(QStringLiteral("assetsHeaderMenuBar"));
    bar->setNativeMenuBar(false);

    auto* view = bar->addMenu(tr("View"));
    view->setObjectName(QStringLiteral("assetsViewMenu"));
    auto* expand = view->addAction(tr("Expand All"));
    expand->setObjectName(QStringLiteral("assetsExpandAllAction"));
    connect(expand, &QAction::triggered, tree_, &QTreeWidget::expandAll);
    auto* collapse = view->addAction(tr("Collapse All"));
    collapse->setObjectName(QStringLiteral("assetsCollapseAllAction"));
    connect(collapse, &QAction::triggered, tree_, &QTreeWidget::collapseAll);

    auto* add = bar->addMenu(tr("Add"));
    add->setObjectName(QStringLiteral("assetsAddMenu"));
    auto* newComposition = add->addAction(tr("New Composition…"));
    newComposition->setObjectName(QStringLiteral("assetsNewCompositionAction"));
    connect(newComposition, &QAction::triggered, this, &AssetsEditor::showNewCompositionDialog);
    auto* newFolder = add->addAction(tr("New Folder"));
    newFolder->setObjectName(QStringLiteral("assetsNewFolderAction"));
    setDisabledReason(newFolder, tr("Folders arrive with asset organisation"));

    auto* select = bar->addMenu(tr("Select"));
    select->setObjectName(QStringLiteral("assetsSelectMenu"));
    auto* selectAll = select->addAction(tr("All"));
    selectAll->setObjectName(QStringLiteral("assetsSelectAllAction"));
    connect(selectAll, &QAction::triggered, tree_, &QTreeWidget::selectAll);
    auto* selectNone = select->addAction(tr("None"));
    selectNone->setObjectName(QStringLiteral("assetsSelectNoneAction"));
    connect(selectNone, &QAction::triggered, tree_, &QTreeWidget::clearSelection);

    headerMenuWidget_ = bar;
}

void AssetsEditor::buildFooter() {
    footerWidget_ = new QWidget(this);
    footerWidget_->setObjectName(QStringLiteral("assetsFooter"));
    auto* layout = new QHBoxLayout(footerWidget_);
    layout->setContentsMargins(8, 0, 8, 0);
    layout->setSpacing(8);

    auto* newComposition = new QPushButton(tr("New Composition"), footerWidget_);
    newComposition->setObjectName(QStringLiteral("assetsNewCompositionButton"));
    connect(newComposition, &QPushButton::clicked, this, &AssetsEditor::showNewCompositionDialog);
    layout->addWidget(newComposition);

    auto* newFolder = new QPushButton(tr("New Folder"), footerWidget_);
    newFolder->setObjectName(QStringLiteral("assetsNewFolderButton"));
    setDisabledReason(newFolder, tr("Folders arrive with asset organisation"));
    layout->addWidget(newFolder);

    auto* import = new QPushButton(tr("Import"), footerWidget_);
    import->setObjectName(QStringLiteral("assetsImportButton"));
    setDisabledReason(import, tr("Image and sequence import arrives with the media pipeline"));
    layout->addWidget(import);

    layout->addStretch(1);
    auto* remove = new QPushButton(tr("Delete"), footerWidget_);
    remove->setObjectName(QStringLiteral("assetsDeleteButton"));
    connect(remove, &QPushButton::clicked, this,
            [this] { deleteComposition(compositionIdForItem(tree_->currentItem())); });
    layout->addWidget(remove);
}

QWidget* AssetsEditor::takeHeaderMenuWidget() {
    if (headerMenuWidgetTaken_) {
        return nullptr;
    }
    headerMenuWidgetTaken_ = true;
    return headerMenuWidget_;
}

QWidget* AssetsEditor::takeFooterWidget() {
    if (footerWidgetTaken_) {
        return nullptr;
    }
    footerWidgetTaken_ = true;
    return footerWidget_;
}

} // namespace bloom::ui
