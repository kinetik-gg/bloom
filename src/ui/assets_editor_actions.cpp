#include "asset_input_picker_guard.hpp"
#include "assets_editor_internal.hpp"
#include "project_change_check.hpp"
#include "project_item_delete.hpp"
#include "reveal_in_file_manager.hpp"
#include <QHBoxLayout>
#include <QPointer>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>
#include <algorithm>
#include <bloom/commands/asset_operations.hpp>
#include <bloom/commands/composition_settings_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/document/project.hpp>
#include <bloom/ui/asset_controller.hpp>
#include <bloom/ui/assets_editor.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/dialog.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <filesystem>
#include <memory>
#include <set>
#include <tuple>

namespace bloom::ui {
namespace {
// The Edit Tags rows. A scroll area's own minimum is two scroll bars' worth, which a layout
// honours even under a single tag; one tag row is the real minimum.
class TagRowsArea final : public QScrollArea {
  public:
    using QScrollArea::QScrollArea;
    [[nodiscard]] QSize minimumSizeHint() const override {
        return {0, kit::px(kit::Size::Control)};
    }
};
} // namespace
std::optional<document::AssetFolderId> AssetsEditor::selectedFolder() const {
    return assets::targetFolder(tree_->currentItem() && tree_->currentItem()->isSelected()
                                    ? tree_->currentItem()
                                    : nullptr);
}
std::vector<document::AssetId> AssetsEditor::selectedAssets() const {
    std::vector<document::AssetId> ids;
    for (QTreeWidgetItemIterator it(tree_, QTreeWidgetItemIterator::Selected); *it; ++it)
        if (const auto id = assets::assetId(*it); id.isValid())
            ids.push_back(id);
    return ids;
}
void AssetsEditor::createFolder() {
    const auto parent = selectedFolder();
    std::set<std::string> names;
    for (const auto& folder : session_.snapshot().project().assetFolders())
        if (folder.parent == parent)
            names.insert(folder.name);
    auto name = tr("New Folder").toStdString();
    for (std::size_t suffix = 2; names.contains(name); ++suffix)
        name = tr("New Folder %1").arg(suffix).toStdString();
    search_->clear();
    if (parent)
        collapsedFolders_.remove(parent->value());
    commands::Transaction transaction("New Folder", session_.snapshot().revision());
    transaction.emplace<commands::CreateAssetFolder>(name, parent);
    const auto result = session_.executeTransaction(std::move(transaction));
    const auto id = result.outputId<document::AssetFolderId>(commands::kCreateAssetFolderOutput);
    if (!id)
        return;
    for (QTreeWidgetItemIterator it(tree_); *it; ++it)
        if (assets::folderId(*it) == id) {
            tree_->clearSelection();
            tree_->setCurrentItem(*it);
            beginRename(*it);
            break;
        }
}
void AssetsEditor::editTags(document::AssetId id) {
    const auto* asset = session_.snapshot().project().findAsset(id);
    if (!asset)
        return;
    std::vector<document::AssetId> ids;
    for (const auto* item : tree_->selectedItems()) {
        const auto selected = assets::assetId(item);
        if (selected.isValid())
            ids.push_back(selected);
    }
    if (std::ranges::find(ids, id) == ids.end())
        ids = {id};
    const auto tags = asset->tags;
    auto check = std::make_shared<ProjectChangeCheck>(session_);
    auto* dialog = new kit::KDialog(tr("Edit Tags"), QStringLiteral("assetsTagsDialog"), *this);
    auto* layout = dialog->contentLayout();
    auto* label =
        new kit::KLabel(ids.size() > 1 ? tr("Replace tags on %1 selected assets").arg(ids.size())
                                       : tr("One tag per field"),
                        dialog);
    layout->addWidget(label);
    auto* scroll = new TagRowsArea(dialog);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    // The rows' own height, scrolling only past a diagnostic-sized list: no empty band under a
    // single tag.
    scroll->setSizeAdjustPolicy(QAbstractScrollArea::AdjustToContents);
    scroll->setMaximumHeight(kit::px(kit::Size::DiagnosticHeight));
    // The tag rows sit on the dialog's own surface, not in a darker well.
    scroll->viewport()->setAutoFillBackground(false);
    auto* fields = new QWidget(scroll);
    auto* rows = new QVBoxLayout(fields);
    rows->setContentsMargins(0, 0, 0, 0);
    rows->setSpacing(kit::px(kit::Spacing::XS));
    rows->addStretch();
    scroll->setWidget(fields);
    fields->setAutoFillBackground(false);
    layout->addWidget(scroll);
    auto* add = new kit::KButton(tr("Add Tag"), dialog);
    add->setObjectName(QStringLiteral("assetsAddTagButton"));
    layout->addWidget(add, 0, Qt::AlignLeft);
    auto editors = std::make_shared<std::vector<QPointer<kit::KLineEdit>>>();
    const auto addTag = [dialog, scroll, fields, rows, add, editors](const QString& text) {
        if (editors->size() >= 64)
            return;
        auto* row = new QWidget(fields);
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(kit::px(kit::Spacing::XS));
        auto* edit = new kit::KLineEdit(text, row);
        edit->setObjectName(QStringLiteral("assetsTagField"));
        edit->setAccessibleName(tr("Tag"));
        edit->setPlaceholderText(tr("Tag"));
        auto* remove = new kit::KIconButton(row);
        remove->setIcon(kit::icon(kit::IconId::Close, kit::IconRole::Chrome));
        remove->setToolTip(tr("Remove Tag"));
        remove->setAccessibleName(tr("Remove Tag"));
        rowLayout->addWidget(edit, 1);
        rowLayout->addWidget(remove);
        rows->insertWidget(rows->count() - 1, row);
        scroll->updateGeometry();
        editors->emplace_back(edit);
        connect(remove, &kit::KIconButton::clicked, dialog, [editors, add, row, edit, scroll] {
            std::erase(*editors, QPointer<kit::KLineEdit>(edit));
            row->hide();
            row->deleteLater();
            scroll->updateGeometry();
            add->setEnabled(true);
        });
        add->setEnabled(editors->size() < 64);
        edit->setFocus();
    };
    for (const auto& tag : tags)
        addTag(QString::fromStdString(tag));
    if (tags.empty())
        addTag({});
    connect(add, &kit::KButton::clicked, dialog, [addTag] { addTag({}); });
    dialog->addButton(kit::KDialog::Role::Accept, tr("Apply"))
        ->setObjectName(QStringLiteral("assetsApplyTagsButton"));
    dialog->addButton(kit::KDialog::Role::Reject, tr("Cancel"));
    // Accept-time (and live) check: tags read from a project that has since changed are not
    // written over it.
    dialog->setValidator([check]() -> std::optional<kit::KRefusal> {
        if (check->projectChanged())
            return kit::KRefusal{nullptr, tr("The project changed while editing tags. Cancel and "
                                             "open Edit Tags again.")};
        return std::nullopt;
    });
    connect(&session_, &CompositionSession::snapshotChanged, dialog, &kit::KDialog::revalidate);
    if (!editors->empty())
        dialog->setInitialFocus(editors->front());
    QPointer<CompositionSession> session(&session_);
    dialog->open([session, check, ids = std::move(ids), editors](const kit::KDialog::Role role) {
        if (role != kit::KDialog::Role::Accept || !session || check->projectChanged())
            return;
        std::vector<std::string> edited;
        for (const auto& field : *editors) {
            const auto tag = field ? field->text().trimmed() : QString{};
            if (!tag.isEmpty())
                edited.push_back(tag.toStdString());
        }
        commands::Transaction transaction("Edit Asset Tags", check->snapshot().revision());
        transaction.emplace<commands::SetAssetTags>(ids, std::move(edited));
        (void)session->executeTransaction(std::move(transaction));
    });
}
void AssetsEditor::editInputColor(const document::AssetId id) {
    const auto* asset = session_.snapshot().project().findAsset(id);
    auto* controller = session_.assetController();
    if (!asset || !controller || !controller->acceptsEdits())
        return;
    const auto initialInterpretation = asset->interpretation;
    const auto initialColorIntent = runtime::captureEvaluationColorIntent(session_.colorIntent());
    if (!initialColorIntent) {
        emit session_.commandRejected(tr("The project colour configuration is unavailable."));
        return;
    }
    // Issue #381 (Interpret Footage) will replace this dialog; it stays a minimal kit port.
    auto check = std::make_shared<ProjectChangeCheck>(session_);
    auto* dialog =
        new kit::KDialog(tr("Input Colour — %1").arg(QString::fromStdString(asset->name)),
                         QStringLiteral("assetsInputColorDialog"), *this);
    auto* explanation = new kit::KLabel(
        tr("Choose how this asset's pixels are interpreted. A source node can override this "
           "choice in Properties."),
        dialog);
    explanation->setWordWrap(true);
    dialog->contentLayout()->addWidget(explanation);
    auto* selector = new kit::KDropdown(dialog);
    selector->setObjectName(QStringLiteral("assetsInputColorSelector"));
    selector->setSearchable(true);
    selector->setWidthFloor(kit::px(kit::Size::DropdownWidth) * 2);
    dialog->contentLayout()->addWidget(selector);
    auto* status = new kit::KLabel(dialog);
    status->setObjectName(QStringLiteral("assetsInputColorStatus"));
    status->setWordWrap(true);
    status->setTone(kit::Color::Muted);
    dialog->contentLayout()->addWidget(status);
    dialog->contentLayout()->addStretch(1);
    dialog->buttonBox()->setObjectName(QStringLiteral("assetsInputColorButtons"));
    dialog->addButton(kit::KDialog::Role::Accept, tr("Apply"));
    dialog->addButton(kit::KDialog::Role::Reject, tr("Cancel"));
    const auto initial = [&] {
        if (!asset->interpretation.inputColorSpaceId.empty())
            return QStringLiteral("id:") +
                   QString::fromStdString(asset->interpretation.inputColorSpaceId);
        switch (asset->interpretation.colorSpace) {
        case document::AssetColorSpace::Raw:
            return QStringLiteral("raw");
        case document::AssetColorSpace::Srgb:
            return QStringLiteral("srgb");
        case document::AssetColorSpace::Linear:
            return QStringLiteral("linear");
        case document::AssetColorSpace::Auto:
            return QStringLiteral("auto");
        }
        return QStringLiteral("auto");
    }();
    QPointer<AssetController> guardedController(controller);
    const auto refreshChoices = [selector, status, initial, guardedController] {
        if (!guardedController)
            return;
        const QString selected = selector->count() ? selector->currentData().toString() : initial;
        const QSignalBlocker blocked(selector);
        selector->clearItems();
        selector->addItem(tr("Automatic (from media tags)"), QStringLiteral("auto"));
        selector->addItem(tr("Raw data (no colour conversion)"), QStringLiteral("raw"));
        selector->addItem(tr("Legacy sRGB"), QStringLiteral("srgb"));
        selector->addItem(tr("Legacy Linear"), QStringLiteral("linear"));
        if (const auto* spaces = guardedController->inputColorSpaces()) {
            auto ordered = *spaces;
            std::ranges::sort(ordered, [](const auto& left, const auto& right) {
                return std::tie(left.family, left.id) < std::tie(right.family, right.id);
            });
            for (const auto& space : ordered) {
                const auto name = QString::fromStdString(space.id);
                const auto family = QString::fromStdString(space.family);
                selector->addItem(family.isEmpty() ? name : family + QStringLiteral(" / ") + name,
                                  QStringLiteral("id:") + name);
            }
            status->setText(tr("OCIO colour spaces from the selected project configuration."));
        } else if (!guardedController->inputColorCatalogDiagnostic().isEmpty()) {
            status->setText(guardedController->inputColorCatalogDiagnostic());
        } else {
            status->setText(tr("Preparing OCIO colour spaces…"));
        }
        if (selector->findData(selected) < 0 && selected.startsWith(QStringLiteral("id:"))) {
            selector->addItem(tr("Current space unavailable: %1").arg(selected.mid(3)), selected);
        }
        const auto index = selector->findData(selected);
        selector->setCurrentIndex(index < 0 ? 0 : index);
    };
    connect(controller, &AssetController::changed, dialog, refreshChoices);
    controller->requestInputColorSpaces();
    refreshChoices();
    QPointer<CompositionSession> session(&session_);
    // The shared accept-time check, with this dialog's narrower rule: only this asset's
    // interpretation and the project's colour configuration need to be what the dialog showed.
    const auto stale = [session, check, id, initialInterpretation,
                        initialColorIntent = *initialColorIntent] {
        return !session || check->documentRebound() ||
               !detail::assetInputPickerTargetMatches(check->snapshot().project().id(), id,
                                                      initialInterpretation,
                                                      session->snapshot().project()) ||
               session->colorIntent() != initialColorIntent;
    };
    dialog->setValidator([stale]() -> std::optional<kit::KRefusal> {
        if (stale())
            return kit::KRefusal{nullptr, tr("This asset or its colour configuration changed "
                                             "while choosing input colour. Cancel and open the "
                                             "picker again.")};
        return std::nullopt;
    });
    connect(&session_, &CompositionSession::snapshotChanged, dialog, &kit::KDialog::revalidate);
    dialog->setInitialFocus(selector);
    QPointer<kit::KDropdown> choiceField(selector);
    dialog->open([session, stale, choiceField, initial, id,
                  guardedController](const kit::KDialog::Role role) {
        if (role != kit::KDialog::Role::Accept || !session || !choiceField || stale())
            return;
        const auto choice = choiceField->currentData().toString();
        if (choice == initial)
            return;
        document::AssetColorSpace legacy = document::AssetColorSpace::Auto;
        std::string inputId;
        if (choice == QStringLiteral("raw"))
            legacy = document::AssetColorSpace::Raw;
        else if (choice == QStringLiteral("srgb"))
            legacy = document::AssetColorSpace::Srgb;
        else if (choice == QStringLiteral("linear"))
            legacy = document::AssetColorSpace::Linear;
        else if (choice.startsWith(QStringLiteral("id:"))) {
            inputId = choice.mid(3).toStdString();
            const auto* spaces =
                guardedController ? guardedController->inputColorSpaces() : nullptr;
            if (!spaces || std::ranges::none_of(
                               *spaces, [&](const auto& space) { return space.id == inputId; })) {
                emit session->commandRejected(tr("That OCIO colour space is unavailable. Reopen "
                                                 "the picker after the configuration is ready."));
                return;
            }
        } else if (choice != QStringLiteral("auto"))
            return;
        commands::Transaction transaction("Set Asset Input Colour", session->snapshot().revision());
        transaction.emplace<commands::SetAssetInputColor>(id, legacy, std::move(inputId));
        (void)session->executeTransaction(std::move(transaction));
    });
}
void AssetsEditor::removeSelected() {
    ProjectItemSelection selection;
    for (QTreeWidgetItemIterator it(tree_, QTreeWidgetItemIterator::Selected); *it; ++it) {
        // A selected folder already accounts for what it holds.
        bool insideSelectedFolder = false;
        for (auto* parent = (*it)->parent(); parent; parent = parent->parent())
            insideSelectedFolder = insideSelectedFolder || parent->isSelected();
        if (insideSelectedFolder)
            continue;
        if (const auto asset = assets::assetId(*it); asset.isValid())
            selection.assets.push_back(asset);
        else if (const auto composition = assets::compositionId(*it); composition.isValid())
            selection.compositions.push_back(composition);
        else if (const auto folder = assets::folderId(*it))
            selection.folders.push_back(*folder);
    }
    // The same path, and the same in-use warning, as Composition > Delete Composition.
    requestDeleteProjectItems(session_, *this, std::move(selection));
}
void AssetsEditor::revealInFileManager(const document::AssetId id) {
    const auto* controller = session_.assetController();
    const auto path = controller ? controller->sourcePath(id) : std::filesystem::path{};
    // Selects the file where the platform's file manager can, else opens its folder (#380).
    ui::revealInFileManager(path, this);
}
void AssetsEditor::newCompositionFromSelection() {
    const auto snapshot = session_.snapshot();
    const auto& project = snapshot.project();
    std::vector<document::AssetId> footage;
    for (const auto id : selectedAssets())
        if (const auto* asset = project.findAsset(id); asset &&
                                                       asset->kind != document::AssetKind::Lut &&
                                                       asset->kind != document::AssetKind::Font)
            footage.push_back(id);
    if (footage.empty())
        return;
    // Sized from the first visual footage item, like After Effects; the active composition (or
    // the defaults) supplies what the footage does not state.
    commands::CompositionSettings base;
    base.name = tr("Composition %1").arg(project.compositions().size() + 1).toStdString();
    base.duration = core::RationalTime::fromInteger(10);
    if (const auto* current = session_.composition())
        base = commands::compositionSettingsOf(*current);
    const auto* first = project.findAsset(footage.front());
    for (const auto id : footage)
        if (const auto* asset = project.findAsset(id); asset->kind != document::AssetKind::Audio) {
            first = asset;
            break;
        }
    auto settings = commands::compositionSettingsForAsset(*first, base);
    settings.timecode = {};
    commands::Transaction transaction("New Composition from Selection", snapshot.revision());
    transaction.emplace<commands::AddCompositionFromAssets>(
        std::move(settings), footage, assets::targetFolder(tree_->currentItem()));
    const auto result = session_.executeTransaction(std::move(transaction));
    if (const auto id = result.outputId<document::CompositionId>(commands::kAddCompositionOutput))
        openComposition(*id);
}
} // namespace bloom::ui
