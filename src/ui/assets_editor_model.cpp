#include "assets_editor_internal.hpp"
#include "properties_registry_row.hpp"
#include <QHeaderView>
#include <QMetaType>
#include <QSignalBlocker>
#include <QTreeWidgetItemIterator>
#include <algorithm>
#include <bloom/core/file_uri.hpp>
#include <bloom/document/project.hpp>
#include <bloom/document/value_utility_nodes.hpp>
#include <bloom/media/provider/ffmpeg_manifest.hpp>
#include <bloom/ui/asset_controller.hpp>
#include <bloom/ui/assets_editor.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <tuple>

namespace bloom::ui {
using namespace assets;
namespace assets {
QString frameRateText(const document::FrameRate rate) {
    if (rate.denominator() == 1)
        return QString::number(rate.numerator());
    auto text = QString::number(static_cast<double>(rate.numerator()) / rate.denominator(), 'f', 3);
    while (text.endsWith(QLatin1Char('0')))
        text.chop(1);
    if (text.endsWith(QLatin1Char('.')))
        text.chop(1);
    return text;
}
std::optional<document::FrameRate> videoFrameRate(const document::AssetRecord& asset) {
    for (const auto& stream : asset.videoStreams)
        if (stream.kind == 1 && stream.framePeriod.numerator() > 0 &&
            stream.framePeriod.denominator() > 0 &&
            stream.framePeriod.numerator() <= std::numeric_limits<std::uint32_t>::max() &&
            stream.framePeriod.denominator() <= std::numeric_limits<std::uint32_t>::max())
            return document::FrameRate::create(
                static_cast<std::uint32_t>(stream.framePeriod.denominator()),
                static_cast<std::uint32_t>(stream.framePeriod.numerator()));
    return std::nullopt;
}
std::uint64_t framesAt(const core::RationalTime time, const document::FrameRate rate) {
    const auto seconds = std::max(0.0, time.toSeconds());
    return static_cast<std::uint64_t>(
        std::floor(seconds * rate.numerator() / rate.denominator() + 1e-9));
}
QString timecodeForFrames(const std::uint64_t frames, const document::FrameRate rate) {
    const auto fps = static_cast<std::uint64_t>(
        std::ceil(static_cast<double>(rate.numerator()) / rate.denominator()));
    const auto perHour = fps * 3600;
    return QStringLiteral("%1:%2:%3:%4")
        .arg(frames / perHour)
        .arg((frames / (fps * 60)) % 60, 2, 10, QLatin1Char('0'))
        .arg((frames / fps) % 60, 2, 10, QLatin1Char('0'))
        .arg(frames % fps, 2, 10, QLatin1Char('0'));
}
QString timecodeText(const core::RationalTime time, const document::FrameRate rate) {
    return timecodeForFrames(framesAt(time, rate), rate);
}
QString durationText(const std::uint64_t frames, const document::FrameRate rate) {
    return frames == 1
               ? QObject::tr("%1 (1 frame)").arg(timecodeForFrames(frames, rate))
               : QObject::tr("%1 (%2 frames)").arg(timecodeForFrames(frames, rate)).arg(frames);
}
document::FrameRate sequenceRate(const document::Composition* active) {
    return active != nullptr ? active->format().frameRate()
                             : document::CompositionFormat{}.frameRate();
}
std::uint64_t sequenceFrames(const document::AssetRecord& asset) {
    if (asset.manifest.last >= asset.manifest.first && !asset.manifest.members.empty())
        return static_cast<std::uint64_t>(static_cast<std::int64_t>(asset.manifest.last) -
                                          static_cast<std::int64_t>(asset.manifest.first)) +
               1;
    return asset.manifest.members.size();
}
QString assetKindText(const document::AssetRecord& asset) {
    const auto extension = asset.locator.path.substr(asset.locator.path.find_last_of('.') + 1);
    switch (asset.kind) {
    case document::AssetKind::Lut:
        return QObject::tr("LUT");
    case document::AssetKind::Font:
        return QObject::tr("Font");
    case document::AssetKind::Video:
        return QObject::tr("Video");
    case document::AssetKind::Sequence:
        return QObject::tr("Sequence");
    case document::AssetKind::Audio:
        return QObject::tr("Audio");
    default:
        return extension == "exr" || extension == "EXR" ? QObject::tr("EXR") : QObject::tr("Image");
    }
}
QString assetLocationText(const document::AssetRecord& asset) {
    if (asset.locator.kind != "file" || asset.locator.path.empty())
        return {};
    auto path = QString::fromStdString(asset.locator.path);
    if (asset.locator.path.find(':') != std::string::npos)
        if (const auto absolute = core::absolutePathFromFileUri(asset.locator.path))
            path = QString::fromStdU16String(absolute->generic_u16string());
    if (asset.kind == document::AssetKind::Sequence && !asset.manifest.pattern.empty()) {
        const auto slash = path.lastIndexOf(QLatin1Char('/'));
        path = path.left(slash + 1) + QString::fromStdString(asset.manifest.pattern);
    }
    return path;
}
kit::IconId assetKindIcon(const document::AssetRecord& asset) {
    switch (asset.kind) {
    case document::AssetKind::Lut:
        return kit::IconId::SlidersHorizontal;
    case document::AssetKind::Font:
        return kit::IconId::Text;
    case document::AssetKind::Sequence:
        return kit::IconId::Images;
    case document::AssetKind::Audio:
        return kit::IconId::Audio;
    case document::AssetKind::Video:
        return kit::IconId::Clip;
    default:
        return kit::IconId::Image;
    }
}
kit::Color assetLabelColor(const document::AssetRecord& asset) {
    switch (asset.kind) {
    case document::AssetKind::Sequence:
        return kit::Color::DataSequence;
    case document::AssetKind::Audio:
        return kit::Color::DataAudio;
    case document::AssetKind::Font:
        return kit::Color::DataClip;
    case document::AssetKind::Lut:
        return kit::Color::Muted;
    default:
        return kit::Color::DataImage;
    }
}
} // namespace assets

namespace {
QString itemKey(const QTreeWidgetItem* item) {
    if (!item)
        return {};
    for (const auto role : {kCompositionRole, kAssetRole, kFolderRole, kDataBlockRole}) {
        const auto value = item->data(0, role);
        if (role == kDataBlockRole && value.metaType().id() != QMetaType::ULongLong) {
            if (value.toBool())
                return QStringLiteral("data-root");
            continue;
        }
        if (const auto id = value.toULongLong(); id)
            return QString::number(role) + ':' + QString::number(id);
    }
    return {};
}

QString videoContainerTags(const document::AssetRecord& asset) {
    QStringList tags;
    for (const auto& stream : asset.videoStreams) {
        if (stream.kind == 2) {
            tags.push_back(QObject::tr("Stream %1: audio, %2 ch, %3 Hz")
                               .arg(stream.id)
                               .arg(stream.channelLayout.size())
                               .arg(stream.sampleRate));
            continue;
        }
        tags.push_back(QObject::tr("Stream %1: primaries=%2, transfer=%3, matrix=%4, range=%5")
                           .arg(stream.id)
                           .arg(stream.primaries)
                           .arg(stream.transfer)
                           .arg(stream.matrix)
                           .arg(stream.range));
        if (stream.kind == 1)
            tags.push_back(
                QObject::tr("Field order: %1; timecode: %2")
                    .arg(videoFieldOrderText(stream.fieldOrder), videoTimecodeText(stream)));
    }
    return tags.join(QStringLiteral("\n"));
}

void setColumn(QTreeWidgetItem* item, const int column, const QString& text,
               const QVariant& sort = {}) {
    item->setText(column, text);
    if (sort.isValid())
        item->setData(column, kSortRole, sort);
}

void setSize(QTreeWidgetItem* item, const std::uint64_t width, const std::uint64_t height) {
    if (width == 0 || height == 0)
        return;
    setColumn(item, SizeColumn, QStringLiteral("%1 × %2").arg(width).arg(height),
              static_cast<double>(width) * static_cast<double>(height));
}

QString audioDuration(const core::RationalTime duration) {
    return QObject::tr("%1 s").arg(std::max(0.0, duration.toSeconds()), 0, 'f', 2);
}
} // namespace

QString AssetsEditor::rowSignature() const {
    // Every record field and controller status the rows render. Unit separators keep fields apart.
    QString signature;
    const auto field = [&signature](const QString& value) {
        signature += value;
        signature += QChar(0x1f);
    };
    const auto text = [&field](const std::string_view value) {
        field(QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size())));
    };
    const auto number = [&field](const auto value) {
        field(QString::number(static_cast<qulonglong>(value)));
    };
    const auto time = [&field](const core::RationalTime value) {
        field(QString::number(value.numerator()) + '/' + QString::number(value.denominator()));
    };
    const auto& project = session_.snapshot().project();
    for (const auto& composition : project.compositions()) {
        number(composition.id().value());
        text(composition.name());
        number(composition.folder() ? composition.folder()->value() + 1 : 0);
        number(composition.format().width());
        number(composition.format().height());
        number(composition.format().frameRate().numerator());
        number(composition.format().frameRate().denominator());
        time(composition.duration());
    }
    // Sequences are timed at the active composition's rate.
    if (std::ranges::any_of(project.assets(), [](const auto& asset) {
            return asset.kind == document::AssetKind::Sequence;
        }))
        number(session_.compositionId().value());
    signature += QChar(0x1e);
    for (const auto& folder : project.assetFolders()) {
        number(folder.id.value());
        number(folder.parent ? folder.parent->value() + 1 : 0);
        text(folder.name);
    }
    signature += QChar(0x1e);
    const auto* controller = session_.assetController();
    for (const auto& asset : project.assets()) {
        number(asset.id.value());
        number(asset.folder ? asset.folder->value() + 1 : 0);
        number(asset.order);
        number(static_cast<int>(asset.kind));
        text(asset.name);
        text(asset.locator.kind);
        text(asset.locator.path);
        text(asset.manifest.pattern);
        text(asset.fontStyle);
        text(asset.interpretation.inputColorSpaceId);
        number(asset.width);
        number(asset.height);
        time(asset.duration);
        number(asset.manifest.members.size());
        for (const auto& tag : asset.tags)
            text(tag);
        for (const auto& stream : asset.videoStreams) {
            text(stream.codec);
            time(stream.framePeriod);
        }
        if (asset.kind == document::AssetKind::Video)
            field(videoContainerTags(asset));
        if (controller) {
            number(controller->missing(asset.id) ? 1 : 0);
            number(controller->dataOnly(asset.id) ? 1 : 0);
            number(controller->inputColorSpaceAssumed(asset.id) ? 1 : 0);
            field(controller->problem(asset.id));
            field(controller->audioDiagnostic(asset.id));
            field(controller->inputColorSpaceDisplay(asset.id));
            field(controller->inputColorSpaceWarning(asset.id));
        }
        signature += QChar(0x1e);
    }
    for (const auto& block : project.typedDataBlocks()) {
        const auto* id = std::get_if<document::DataBlockRecordId>(&block.id);
        if (id == nullptr || document::isMediaDataBlockKind(block.kind))
            continue;
        number(id->value());
        text(block.typeId);
        text(document::dataBlockKindName(block.kind));
        text(block.provenance.createdAt);
        const auto digest = block.provenance.contentDigest.toLowercaseHex();
        text(std::string_view(digest.data(), digest.size()));
    }
    return signature;
}

void AssetsEditor::refreshRows() {
    if (rowSignature() != rowSignature_)
        rebuild();
}

void AssetsEditor::rebuild() {
    rowSignature_ = rowSignature();
    ++rowRebuilds_;
    rebuilding_ = true;
    QSet<QString> selected;
    for (const auto* item : tree_->selectedItems())
        selected.insert(itemKey(item));
    const auto current = itemKey(tree_->currentItem());
    const QSignalBlocker blocker(tree_);
    // Rows are sorted once, after they are all in place.
    tree_->setSortingEnabled(false);
    tree_->clear();
    const auto rowHeight = QSize(0, kit::px(kit::Size::ListRow));
    const auto finish = [rowHeight](QTreeWidgetItem* item, const kit::IconId icon,
                                    const kit::Color color) {
        item->setIcon(NameColumn, kit::icon(icon, kit::IconRole::Chrome, color));
        item->setSizeHint(NameColumn, rowHeight);
        item->setToolTip(TypeColumn, item->toolTip(NameColumn));
    };
    const auto& project = session_.snapshot().project();
    std::map<document::AssetFolderId, QTreeWidgetItem*> folders;
    for (const auto& folder : project.assetFolders())
        folders.emplace(folder.id, new AssetItem);
    const auto parentFor = [&](const std::optional<document::AssetFolderId>& folder) {
        const auto found = folder ? folders.find(*folder) : folders.end();
        return found == folders.end() ? tree_->invisibleRootItem() : found->second;
    };
    for (const auto& folder : project.assetFolders()) {
        auto* item = folders.at(folder.id);
        parentFor(folder.parent)->addChild(item);
        item->setText(NameColumn, QString::fromStdString(folder.name));
        setColumn(item, TypeColumn, tr("Folder"));
        item->setData(0, kFolderRole, QVariant::fromValue<qulonglong>(folder.id.value()));
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable |
                       Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled);
        item->setToolTip(NameColumn, tr("Folder %1").arg(QString::fromStdString(folder.name)));
        finish(item, kit::IconId::Folder, kit::Color::Label3);
    }
    for (const auto& composition : project.compositions()) {
        auto* item = new AssetItem(parentFor(composition.folder()));
        const auto format = composition.format();
        item->setText(NameColumn, QString::fromStdString(composition.name()));
        setColumn(item, TypeColumn, tr("Composition"));
        setSize(item, format.width(), format.height());
        setColumn(
            item, DurationColumn,
            durationText(framesAt(composition.duration(), format.frameRate()), format.frameRate()),
            composition.duration().toSeconds());
        setColumn(item, FrameRateColumn, frameRateText(format.frameRate()),
                  static_cast<double>(format.frameRate().numerator()) /
                      format.frameRate().denominator());
        item->setData(0, kCompositionRole,
                      QVariant::fromValue<qulonglong>(composition.id().value()));
        item->setToolTip(NameColumn, tr("Composition %1").arg(composition.id().value()));
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable |
                       Qt::ItemIsDragEnabled);
        finish(item, kit::IconId::Composition, kit::Color::DataComposition);
    }
    const auto* controller = session_.assetController();
    for (const auto& asset : project.assets()) {
        auto* item = new AssetItem(parentFor(asset.folder));
        const bool lut = asset.kind == document::AssetKind::Lut;
        const bool font = asset.kind == document::AssetKind::Font;
        const bool video = asset.kind == document::AssetKind::Video;
        const bool audio = asset.kind == document::AssetKind::Audio;
        const bool sequence = asset.kind == document::AssetKind::Sequence;
        item->setText(NameColumn, QString::fromStdString(asset.name));
        setColumn(item, TypeColumn, assetKindText(asset));
        if (!lut && !font && !audio)
            setSize(item, asset.width, asset.height);
        if (video) {
            const auto rate = videoFrameRate(asset);
            setColumn(item, DurationColumn,
                      rate ? durationText(framesAt(asset.duration, *rate), *rate)
                           : audioDuration(asset.duration),
                      asset.duration.toSeconds());
            if (rate)
                setColumn(item, FrameRateColumn, frameRateText(*rate),
                          static_cast<double>(rate->numerator()) / rate->denominator());
        } else if (audio)
            setColumn(item, DurationColumn, audioDuration(asset.duration),
                      asset.duration.toSeconds());
        else if (sequence) {
            const auto rate = sequenceRate(session_.composition());
            const auto frames = sequenceFrames(asset);
            setColumn(item, DurationColumn, durationText(frames, rate),
                      static_cast<double>(frames) * rate.denominator() / rate.numerator());
        }
        QStringList tags;
        for (const auto& tag : asset.tags)
            tags.push_back(QString::fromStdString(tag));
        item->setData(0, kTagsRole, tags);
        setColumn(item, TagsColumn, tags.join(QStringLiteral(", ")));
        setColumn(item, LocationColumn, assetLocationText(asset));
        item->setToolTip(LocationColumn, item->text(LocationColumn));
        item->setData(0, kAssetRole, QVariant::fromValue<qulonglong>(asset.id.value()));
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable |
                       Qt::ItemIsDragEnabled);
        const bool missing = controller && controller->missing(asset.id);
        // A present source that cannot be shown as it is states its own fix; it is never a
        // Relink prompt. A data-only image (no colour layer) is valid and carries no mark.
        const auto problem = controller ? controller->problem(asset.id) : QString{};
        const auto audioError =
            (audio || video) && controller ? controller->audioDiagnostic(asset.id) : QString{};
        QString tooltip;
        if (!audioError.isEmpty())
            tooltip = tr("Audio preview unavailable: %1").arg(audioError);
        else if (missing)
            tooltip = lut     ? tr("Missing LUT — Replace Footage in Assets")
                      : font  ? tr("Missing font — Replace Footage in Assets")
                      : audio ? tr("Missing audio — Replace Footage in Assets")
                      : video ? tr("Missing video — Replace Footage in Assets")
                              : tr("Missing image — Replace Footage in Assets");
        else if (!problem.isEmpty())
            tooltip = problem;
        else
            tooltip = QString::fromStdString(asset.locator.path);
        if (controller && controller->dataOnly(asset.id))
            tooltip += QStringLiteral("\n") + tr("Data channels only — use its Channels output");
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
            tooltip += QStringLiteral("\n") + tr("Input colour space: ") +
                       QString::fromStdString(asset.interpretation.inputColorSpaceId);
        if (video && std::ranges::any_of(asset.videoStreams, [](const auto& stream) {
                return stream.codec == "prores";
            }))
            tooltip +=
                QStringLiteral("\n") + QString::fromUtf8(media::provider::kProResPreviewNote);
        item->setToolTip(NameColumn, tooltip);
        // The Name glyph is the kind in its label colour, or the row's state: a warning for a
        // missing or unshowable source, an Info note for a colour assumption (untagged video).
        const bool flagged = missing || !problem.isEmpty();
        const bool colourNote =
            !flagged && video && controller && controller->inputColorSpaceAssumed(asset.id);
        item->setData(0, kStateRole,
                      flagged      ? QStringLiteral("missing")
                      : colourNote ? QStringLiteral("colourNote")
                                   : QString{});
        finish(item,
               flagged      ? kit::IconId::Warning
               : colourNote ? kit::IconId::Info
                            : assetKindIcon(asset),
               flagged      ? kit::Color::Warn
               : colourNote ? kit::Color::Muted
                            : assetLabelColor(asset));
    }
    const bool hasDataBlocks =
        std::ranges::any_of(project.typedDataBlocks(), [](const auto& block) {
            return !document::isMediaDataBlockKind(block.kind);
        });
    if (hasDataBlocks) {
        auto* dataRoot = new AssetItem(tree_);
        dataRoot->setText(NameColumn, tr("Data"));
        setColumn(dataRoot, TypeColumn, tr("Inspector"));
        dataRoot->setData(0, kDataBlockRole, true);
        dataRoot->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        finish(dataRoot, kit::IconId::Stack, kit::Color::Muted);
        for (const auto& block : project.typedDataBlocks()) {
            if (document::isMediaDataBlockKind(block.kind))
                continue;
            const auto* id = std::get_if<document::DataBlockRecordId>(&block.id);
            if (id == nullptr)
                continue;
            auto* item = new AssetItem(dataRoot);
            const auto kindName = document::dataBlockKindName(block.kind);
            item->setText(
                NameColumn,
                QStringLiteral("%1 · %2").arg(
                    QString::fromStdString(block.typeId),
                    QString::fromUtf8(kindName.data(), static_cast<qsizetype>(kindName.size()))));
            setColumn(item, TypeColumn, tr("Data block"));
            item->setData(0, kDataBlockRole, QVariant::fromValue<qulonglong>(id->value()));
            const auto digest = block.provenance.contentDigest.toLowercaseHex();
            item->setToolTip(
                NameColumn,
                tr("%1 · created %2 · digest %3")
                    .arg(QString::fromStdString(block.typeId),
                         QString::fromStdString(block.provenance.createdAt),
                         QString::fromStdString(std::string(digest.data(), digest.size()))));
            item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            finish(item, kit::IconId::Info, kit::Color::Muted);
        }
    }
    tree_->setSortingEnabled(true);
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
    rebuilding_ = false;
    selectionChanged();
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
    const auto blockOf = [](const QTreeWidgetItem* item) {
        const auto value = item->data(0, kDataBlockRole);
        return value.metaType().id() == QMetaType::ULongLong
                   ? std::optional(document::DataBlockRecordId::fromRaw(value.toULongLong()))
                   : std::nullopt;
    };
    for (QTreeWidgetItemIterator it(tree_); *it; ++it) {
        if ((*it)->isHidden())
            continue;
        const auto block = blockOf(*it);
        const bool chosen = selectedBlock != nullptr ? block == *selectedBlock
                            : !readBlocks.empty()
                                ? block && readBlocks.contains(*block)
                                : compositionId(*it) == session_.compositionId() &&
                                      session_.compositionId().isValid();
        if (!chosen)
            continue;
        tree_->setCurrentItem(*it);
        (*it)->setSelected(true);
        if (selectedBlock != nullptr)
            break;
    }
    if (!rebuilding_)
        selectionChanged();
}

void AssetsEditor::selectionChanged() {
    if (rebuilding_)
        return;
    const auto* current = tree_->currentItem();
    if (current != nullptr && current->isSelected()) {
        const auto value = current->data(0, kDataBlockRole);
        if (value.metaType().id() == QMetaType::ULongLong)
            session_.selectDataBlock(document::DataBlockRecordId::fromRaw(value.toULongLong()));
    }
    // File > Import and the footer import land in the selected folder (#378).
    if (auto* controller = session_.assetController())
        controller->setImportFolder(selectedFolder());
    refreshSelectionHeader();
}

void AssetsEditor::applyFilter(const QString& text) {
    const auto query = text.trimmed();
    const bool tagsOnly = query.startsWith(QStringLiteral("tag:"), Qt::CaseInsensitive);
    const auto term = tagsOnly ? query.mid(4).trimmed() : query;
    std::vector<QTreeWidgetItem*> items;
    for (QTreeWidgetItemIterator it(tree_); *it; ++it)
        items.push_back(*it);
    const QSignalBlocker blocker(tree_);
    const bool wasRebuilding = rebuilding_;
    rebuilding_ = true;
    // Children before parents, so a folder knows whether anything inside it matched.
    for (auto it = items.rbegin(); it != items.rend(); ++it) {
        auto* item = *it;
        const auto tags = item->data(0, kTagsRole).toStringList();
        const bool textMatches =
            query.isEmpty() ||
            (!tagsOnly && item->text(NameColumn).contains(term, Qt::CaseInsensitive));
        const bool tagMatches = std::ranges::any_of(
            tags, [&](const auto& tag) { return tag.contains(term, Qt::CaseInsensitive); });
        bool childMatches = false;
        for (int index = 0; index < item->childCount(); ++index)
            childMatches = childMatches || !item->child(index)->isHidden();
        item->setHidden(!(textMatches || tagMatches) && !childMatches);
        if (item->isHidden())
            item->setSelected(false);
        if (item->childCount() > 0 || folderId(item)) {
            const auto folder = folderId(item);
            const bool collapsed =
                folder ? collapsedFolders_.contains(folder->value()) : dataCollapsed_;
            item->setExpanded(query.isEmpty() ? !collapsed : childMatches);
        }
    }
    rebuilding_ = wasRebuilding;
}
} // namespace bloom::ui
