#include "assets_editor_internal.hpp"
#include <QHBoxLayout>
#include <QMetaType>
#include <QSignalBlocker>
#include <QTreeWidgetItemIterator>
#include <algorithm>
#include <bloom/document/value_utility_nodes.hpp>
#include <bloom/media/provider/ffmpeg_manifest.hpp>
#include <bloom/ui/asset_controller.hpp>
#include <bloom/ui/assets_editor.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/row.hpp>
#include <map>
#include <set>
#include <tuple>

namespace bloom::ui {
namespace {
class AssetRow final : public kit::KRow {
  public:
    using kit::KRow::KRow;
    void setTagChips(QWidget* trailing, QWidget* full, QWidget* compact, int warningWidth) {
        trailing_ = trailing;
        full_ = full;
        compact_ = compact;
        warningWidth_ = warningWidth;
        updateChips();
    }

  protected:
    void resizeEvent(QResizeEvent* event) override {
        updateChips();
        kit::KRow::resizeEvent(event);
    }

  private:
    void updateChips() {
        if (!trailing_)
            return;
        const bool expanded =
            width() >= kit::px(kit::Size::DropdownWidth) * 3 + kit::px(kit::Size::ToggleCell);
        full_->setVisible(expanded);
        compact_->setVisible(!expanded);
        trailing_->setFixedWidth(
            (expanded ? kit::px(kit::Size::DropdownWidth) : compact_->sizeHint().width()) +
            warningWidth_);
    }
    QWidget* trailing_ = nullptr;
    QWidget* full_ = nullptr;
    QWidget* compact_ = nullptr;
    int warningWidth_ = 0;
};
QString itemKey(const QTreeWidgetItem* item) {
    if (!item)
        return {};
    for (const auto role : {assets::kCompositionRole, assets::kAssetRole, assets::kFolderRole,
                            assets::kDataBlockRole}) {
        const auto value = item->data(0, role);
        if (role == assets::kDataBlockRole && value.metaType().id() != QMetaType::ULongLong) {
            if (value.toBool())
                return QStringLiteral("data-root");
            continue;
        }
        const auto id = value.toULongLong();
        if (id)
            return QString::number(role) + ':' + QString::number(id);
    }
    return QStringLiteral("compositions");
}

QString videoContainerTags(const document::AssetRecord& asset) {
    QStringList tags;
    for (const auto& stream : asset.videoStreams)
        tags.push_back(QObject::tr("Stream %1: primaries=%2, transfer=%3, matrix=%4, range=%5")
                           .arg(stream.id)
                           .arg(stream.primaries)
                           .arg(stream.transfer)
                           .arg(stream.matrix)
                           .arg(stream.range));
    return tags.join(QStringLiteral("\n"));
}
} // namespace
void AssetsEditor::refreshDisclosure(QTreeWidgetItem* item) {
    if (!item || (!assets::folderId(item) && !item->data(0, assets::kCompositionRootRole).toBool()))
        return;
    if (auto* row = qobject_cast<kit::KRow*>(tree_->itemWidget(item, 0))) {
        row->setName(item->text(0),
                     item->isExpanded() ? kit::IconId::CaretDown : kit::IconId::CaretRight);
        row->disclosureButton()->setAccessibleName(item->isExpanded()
                                                       ? tr("Collapse %1").arg(item->text(0))
                                                       : tr("Expand %1").arg(item->text(0)));
    }
}
void AssetsEditor::rebuild() {
    rebuilding_ = true;
    QSet<QString> selected;
    for (const auto* item : tree_->selectedItems())
        selected.insert(itemKey(item));
    const auto current = itemKey(tree_->currentItem());
    const QSignalBlocker blocker(tree_);
    tree_->clear();
    const auto addRow = [this](QTreeWidgetItem* item, kit::IconId icon, bool branch,
                               const QStringList& tags = {}, bool missing = false) {
        auto* row = new AssetRow(tree_);
        row->setObjectName(QStringLiteral("assetsRow"));
        row->setName(item->text(0), icon);
        row->setToolTip(item->toolTip(0));
        auto* kind = new kit::KLabel(row);
        kind->setElidedText(item->text(1));
        QWidget* trailing = nullptr;
        int trailingWidth = 0;
        QWidget* fullTags = nullptr;
        QWidget* compactTags = nullptr;
        if (!tags.empty() || missing) {
            trailing = new QWidget(row);
            auto* layout = new QHBoxLayout(trailing);
            layout->setContentsMargins(0, 0, 0, 0);
            layout->setSpacing(kit::px(kit::Spacing::XS));
            if (!tags.empty()) {
                fullTags = new QWidget(trailing);
                auto* fullLayout = new QHBoxLayout(fullTags);
                fullLayout->setContentsMargins(0, 0, 0, 0);
                fullLayout->setSpacing(kit::px(kit::Spacing::XS));
                layout->addWidget(fullTags, 1);
                auto* compact = new kit::KButton(QStringLiteral("+%1").arg(tags.size()), trailing);
                compact->setObjectName(QStringLiteral("assetsCompactTagsChip"));
                compact->setControlSize(kit::KButton::ControlSize::Compact);
                compact->setMinimumWidth(0);
                compact->setToolTip(tags.join(QStringLiteral(", ")));
                compact->setAccessibleName(tags.size() == 1 ? tr("Edit tag")
                                                            : tr("Edit %1 tags").arg(tags.size()));
                const auto assetId = assets::assetId(item);
                connect(compact, &kit::KButton::clicked, this,
                        [this, assetId] { editTags(assetId); });
                layout->addWidget(compact);
                compactTags = compact;
                auto* tag = new kit::KButton(tags.front(), fullTags);
                tag->setObjectName(QStringLiteral("assetsTagChip"));
                tag->setControlSize(kit::KButton::ControlSize::Compact);
                tag->setAccessibleName(tr("Filter by tag %1").arg(tags.front()));
                tag->setToolTip(tags.join(QStringLiteral(", ")));
                tag->setMinimumWidth(0);
                fullLayout->addWidget(tag, 1);
                connect(tag, &kit::KButton::clicked, this,
                        [this, value = tags.front()] { search_->setText("tag:" + value); });
                if (tags.size() > 1) {
                    auto* more =
                        new kit::KButton(QStringLiteral("+%1").arg(tags.size() - 1), fullTags);
                    more->setObjectName(QStringLiteral("assetsMoreTagsChip"));
                    more->setControlSize(kit::KButton::ControlSize::Compact);
                    more->setToolTip(tags.join(QStringLiteral(", ")));
                    more->setAccessibleName(tr("Edit all tags"));
                    fullLayout->addWidget(more);
                    const auto id = assets::assetId(item);
                    connect(more, &kit::KButton::clicked, this, [this, id] { editTags(id); });
                }
                trailingWidth = kit::px(kit::Size::DropdownWidth);
            }
            if (missing) {
                auto* warning = new kit::KIconButton(trailing);
                warning->setObjectName(QStringLiteral("assetsMissingGlyph"));
                warning->setFixedSize(kit::px(kit::Size::ToggleCell),
                                      kit::px(kit::Size::ToggleCell));
                warning->setIcon(
                    kit::icon(kit::IconId::Warning, kit::IconRole::Chrome, kit::Color::Warn));
                warning->setToolTip(item->toolTip(0));
                warning->setAccessibleName(item->toolTip(0));
                layout->addWidget(warning);
                trailingWidth += kit::px(kit::Size::ToggleCell);
            }
        }
        row->setCells({}, nullptr, {kind}, trailing);
        if (trailing)
            trailing->setFixedWidth(trailingWidth);
        if (fullTags)
            row->setTagChips(trailing, fullTags, compactTags,
                             missing ? kit::px(kit::Size::ToggleCell) : 0);
        if (branch) {
            connect(row->disclosureButton(), &kit::KIconButton::clicked, this,
                    [item] { item->setExpanded(!item->isExpanded()); });
        } else {
            row->disclosureButton()->setAttribute(Qt::WA_TransparentForMouseEvents);
        }
        kind->setAttribute(Qt::WA_TransparentForMouseEvents);
        item->setSizeHint(0, QSize(0, kit::px(kit::Size::ListRow)));
        item->setFirstColumnSpanned(true);
        tree_->setItemWidget(item, 0, row);
        if (branch)
            refreshDisclosure(item);
    };
    auto* compositions = new QTreeWidgetItem(tree_);
    compositions->setText(0, tr("Compositions"));
    compositions->setData(0, assets::kCompositionRootRole, true);
    compositions->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    compositions->setExpanded(!compositionsCollapsed_);
    addRow(compositions, kit::IconId::CaretDown, true);
    for (const auto& composition : session_.snapshot().project().compositions()) {
        auto* item = new QTreeWidgetItem(compositions);
        item->setText(0, QString::fromStdString(composition.name()));
        item->setText(1, tr("Composition"));
        item->setData(0, assets::kCompositionRole,
                      QVariant::fromValue<qulonglong>(composition.id().value()));
        item->setToolTip(0, tr("Composition %1").arg(composition.id().value()));
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable |
                       Qt::ItemIsDragEnabled);
        addRow(item, kit::IconId::Composition, false);
    }
    const auto& project = session_.snapshot().project();
    std::map<document::AssetFolderId, QTreeWidgetItem*> folders;
    for (const auto& folder : project.assetFolders())
        folders.emplace(folder.id, new QTreeWidgetItem);
    for (const auto& folder : project.assetFolders()) {
        auto* item = folders.at(folder.id);
        if (folder.parent)
            folders.at(*folder.parent)->addChild(item);
        else
            tree_->addTopLevelItem(item);
        item->setText(0, QString::fromStdString(folder.name));
        item->setText(1, tr("Folder"));
        item->setData(0, assets::kFolderRole, QVariant::fromValue<qulonglong>(folder.id.value()));
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable |
                       Qt::ItemIsDropEnabled);
    }
    for (const auto& folder : project.assetFolders()) {
        auto* item = folders.at(folder.id);
        item->setExpanded(!collapsedFolders_.contains(folder.id.value()));
        addRow(item, kit::IconId::CaretDown, true);
    }
    std::vector<const document::AssetRecord*> records;
    for (const auto& asset : project.assets())
        records.push_back(&asset);
    std::ranges::sort(records, [](const auto* left, const auto* right) {
        return std::tie(left->folder, left->order, left->id) <
               std::tie(right->folder, right->order, right->id);
    });
    for (const auto* record : records) {
        const auto& asset = *record;
        auto* item = asset.folder ? new QTreeWidgetItem(folders.at(*asset.folder))
                                  : new QTreeWidgetItem(tree_);
        const bool font = asset.kind == document::AssetKind::Font;
        const bool video = asset.kind == document::AssetKind::Video;
        const auto seconds = static_cast<qulonglong>(std::max(0.0, asset.duration.toSeconds()));
        const auto duration = QStringLiteral("%1:%2:%3")
                                  .arg(seconds / 3600, 2, 10, QLatin1Char('0'))
                                  .arg((seconds / 60) % 60, 2, 10, QLatin1Char('0'))
                                  .arg(seconds % 60, 2, 10, QLatin1Char('0'));
        const bool audio = asset.kind == document::AssetKind::Audio;
        const bool sequence = asset.kind == document::AssetKind::Sequence;
        const auto extension = asset.locator.path.substr(asset.locator.path.find_last_of('.') + 1);
        const bool exr = extension == "exr" || extension == "EXR";
        item->setText(0, QString::fromStdString(asset.name));
        item->setText(1, font    ? tr("Font · %1").arg(QString::fromStdString(asset.fontStyle))
                         : video ? tr("Video · %1").arg(duration)
                         : sequence
                             ? (exr ? tr("EXR · %1 frames").arg(asset.manifest.members.size())
                                    : tr("Sequence [%1]").arg(asset.manifest.members.size()))
                         : audio ? tr("Audio · %1 s").arg(asset.duration.toSeconds(), 0, 'f', 2)
                         : exr   ? tr("EXR")
                                 : tr("Image"));
        item->setData(0, assets::kAssetRole, QVariant::fromValue<qulonglong>(asset.id.value()));
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable |
                       Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled);
        QStringList tags;
        for (const auto& tag : asset.tags)
            tags.push_back(QString::fromStdString(tag));
        item->setData(0, assets::kTagsRole, tags);
        const auto* controller = session_.assetController();
        const bool missing = controller && controller->missing(asset.id);
        auto tooltip = missing ? (font    ? tr("Missing font — Relink in Assets")
                                  : audio ? tr("Missing audio — Relink in Assets")
                                  : video ? tr("Missing video — Relink in Assets")
                                          : tr("Missing image — Relink in Assets"))
                               : QString::fromStdString(asset.locator.path);
        if (video) {
            const auto containerTags = videoContainerTags(asset);
            if (!containerTags.isEmpty())
                tooltip += QStringLiteral("\n") + tr("Container colour tags:\n") + containerTags;
        }
        if (controller) {
            auto input = controller->inputColorSpaceDisplay(asset.id);
            if (input.isEmpty() && !asset.interpretation.inputColorSpaceId.empty())
                input = QString::fromStdString(asset.interpretation.inputColorSpaceId);
            if (!input.isEmpty())
                tooltip += QStringLiteral("\n") + tr("Input colour space: ") + input;
            const auto warning = controller->inputColorSpaceWarning(asset.id);
            if (!warning.isEmpty())
                tooltip += QStringLiteral("\n") + tr("Colour note: ") + warning;
        } else if (!asset.interpretation.inputColorSpaceId.empty())
            tooltip += QStringLiteral("\nInput colour space: ") +
                       QString::fromStdString(asset.interpretation.inputColorSpaceId);
        item->setToolTip(0, tooltip);
        if (video && std::ranges::any_of(asset.videoStreams, [](const auto& stream) {
                return stream.codec == "prores";
            }))
            item->setToolTip(0, item->toolTip(0) + QStringLiteral("\n") +
                                    QString::fromUtf8(media::provider::kProResPreviewNote));
        addRow(item,
               font       ? kit::IconId::Text
               : sequence ? kit::IconId::Images
               : audio    ? kit::IconId::Audio
                          : kit::IconId::Image,
               false, tags, missing);
    }
    const bool hasDataBlocks =
        std::ranges::any_of(project.typedDataBlocks(), [](const auto& block) {
            return !document::isMediaDataBlockKind(block.kind);
        });
    if (hasDataBlocks) {
        auto* dataRoot = new QTreeWidgetItem(tree_);
        dataRoot->setText(0, tr("Data"));
        dataRoot->setText(1, tr("Inspector"));
        dataRoot->setData(0, assets::kDataBlockRole, true);
        dataRoot->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        dataRoot->setExpanded(true);
        addRow(dataRoot, kit::IconId::Stack, true);
        for (const auto& block : project.typedDataBlocks()) {
            if (document::isMediaDataBlockKind(block.kind))
                continue;
            const auto* id = std::get_if<document::DataBlockRecordId>(&block.id);
            if (id == nullptr)
                continue;
            auto* item = new QTreeWidgetItem(dataRoot);
            const auto kindName = document::dataBlockKindName(block.kind);
            item->setText(0, QStringLiteral("%1 · %2").arg(
                                 QString::fromStdString(block.typeId),
                                 QString::fromUtf8(kindName.data(),
                                                   static_cast<qsizetype>(kindName.size()))));
            item->setText(1, QString::fromStdString(block.provenance.createdAt));
            item->setData(0, assets::kDataBlockRole, QVariant::fromValue<qulonglong>(id->value()));
            const auto digest = block.provenance.contentDigest.toLowercaseHex();
            item->setToolTip(
                0, tr("%1 · digest %2")
                       .arg(QString::fromStdString(block.typeId),
                            QString::fromStdString(std::string(digest.data(), digest.size()))));
            item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            addRow(item, kit::IconId::Info, false);
        }
    }
    applyFilter(search_->text());
    if (selected.empty())
        updateSelection();
    else {
        for (QTreeWidgetItemIterator it(tree_); *it; ++it) {
            const auto key = itemKey(*it);
            if (key == current)
                tree_->setCurrentItem(*it, 0, QItemSelectionModel::NoUpdate);
            (*it)->setSelected(selected.contains(key) && !(*it)->isHidden());
        }
    }
    refreshRowSelection();
    rebuilding_ = false;
}
void AssetsEditor::updateSelection() {
    const QSignalBlocker blocker(tree_);
    tree_->clearSelection();
    tree_->setCurrentItem(nullptr);
    const auto* selectedBlock =
        std::get_if<document::DataBlockRecordId>(&session_.selection().primary);
    std::set<document::DataBlockRecordId> readBlocks;
    if (selectedBlock == nullptr) {
        for (const auto& block : session_.snapshot().project().typedDataBlocks()) {
            const auto* id = std::get_if<document::DataBlockRecordId>(&block.id);
            if (id == nullptr)
                continue;
            const auto readers = session_.dataBlockReaders(*id);
            if (std::ranges::any_of(session_.selectedNodes(),
                                    [&](const auto node) { return readers.contains(node); }))
                readBlocks.insert(*id);
        }
    }
    for (QTreeWidgetItemIterator it(tree_); *it; ++it)
        if (!(*it)->isHidden() &&
            ((selectedBlock != nullptr &&
              (*it)->data(0, assets::kDataBlockRole).metaType().id() == QMetaType::ULongLong &&
              (*it)->data(0, assets::kDataBlockRole).toULongLong() == selectedBlock->value()) ||
             (selectedBlock == nullptr &&
              (*it)->data(0, assets::kDataBlockRole).metaType().id() == QMetaType::ULongLong &&
              readBlocks.contains(document::DataBlockRecordId::fromRaw(
                  (*it)->data(0, assets::kDataBlockRole).toULongLong()))) ||
             (selectedBlock == nullptr && readBlocks.empty() &&
              assets::compositionId(*it) == session_.compositionId() &&
              session_.compositionId().isValid()))) {
            tree_->setCurrentItem(*it);
            (*it)->setSelected(true);
            if (selectedBlock != nullptr)
                break;
        }
    refreshRowSelection();
}
void AssetsEditor::refreshRowSelection() {
    int index = 0;
    for (QTreeWidgetItemIterator it(tree_); *it; ++it)
        if (auto* row = qobject_cast<kit::KRow*>(tree_->itemWidget(*it, 0)))
            row->setRowState(index++, (*it)->isSelected());
    if (!rebuilding_) {
        const auto* current = tree_->currentItem();
        if (current != nullptr) {
            const auto value = current->data(0, assets::kDataBlockRole);
            if (value.metaType().id() == QMetaType::ULongLong)
                session_.selectDataBlock(document::DataBlockRecordId::fromRaw(value.toULongLong()));
        }
    }
}
void AssetsEditor::applyFilter(const QString& text) {
    const auto query = text.trimmed();
    const bool tagsOnly = query.startsWith(QStringLiteral("tag:"), Qt::CaseInsensitive);
    const auto term = tagsOnly ? query.mid(4).trimmed() : query;
    std::vector<QTreeWidgetItem*> items;
    for (QTreeWidgetItemIterator it(tree_); *it; ++it)
        items.push_back(*it);
    const QSignalBlocker blocker(tree_);
    const bool hasDataBlocks =
        std::ranges::any_of(session_.snapshot().project().typedDataBlocks(), [](const auto& block) {
            return !document::isMediaDataBlockKind(block.kind);
        });
    for (auto it = items.rbegin(); it != items.rend(); ++it) {
        auto* item = *it;
        const auto tags = item->data(0, assets::kTagsRole).toStringList();
        const bool isData = item->data(0, assets::kDataBlockRole).isValid();
        const bool isComposition = assets::compositionId(item).isValid() ||
                                   item->data(0, assets::kCompositionRootRole).toBool();
        const bool isMedia = assets::assetId(item).isValid() || assets::folderId(item).has_value();
        const bool categoryMatches = filterMode_ == -1
                                         ? isMedia || isComposition || (isData && hasDataBlocks)
                                     : filterMode_ == 0 ? isMedia
                                     : filterMode_ == 1 ? isData
                                                        : isComposition;
        const bool textMatches =
            query.isEmpty() || (!tagsOnly && item->text(0).contains(term, Qt::CaseInsensitive));
        const bool tagMatches = std::ranges::any_of(
            tags, [&](const auto& tag) { return tag.contains(term, Qt::CaseInsensitive); });
        const bool matches = categoryMatches && (textMatches || tagMatches);
        bool childMatches = false;
        for (int index = 0; index < item->childCount(); ++index)
            childMatches = childMatches || !item->child(index)->isHidden();
        item->setHidden(!matches && !childMatches);
        if (item->isHidden())
            item->setSelected(false);
        if (assets::folderId(item) || item->data(0, assets::kCompositionRootRole).toBool() ||
            item->data(0, assets::kDataBlockRole).toBool()) {
            const auto folder = assets::folderId(item);
            const bool collapsed =
                folder ? collapsedFolders_.contains(folder->value()) : compositionsCollapsed_;
            item->setExpanded(query.isEmpty() ? !collapsed : childMatches);
            refreshDisclosure(item);
        }
    }
    refreshRowSelection();
}
} // namespace bloom::ui
