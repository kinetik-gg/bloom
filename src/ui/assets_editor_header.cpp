// The Assets selection header (#378): a thumbnail and the facts of the selected item, as at the top
// of the After Effects Project panel. It reads the project and the asset controller's published
// state only; thumbnails are decoded by the controller's worker.
#include "assets_editor_internal.hpp"
#include "colour_space_names.hpp"
#include <QHBoxLayout>
#include <QPixmap>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <array>
#include <bloom/document/project.hpp>
#include <bloom/ui/asset_controller.hpp>
#include <bloom/ui/assets_editor.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/composition_thumbnail_controller.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/thumbnail.hpp>

namespace bloom::ui {
using namespace assets;
namespace {
QString usageText(const Usage usage) {
    if (usage.uses == 0)
        return QObject::tr("Not used");
    const auto times =
        usage.uses == 1 ? QObject::tr("Used once") : QObject::tr("Used %1 times").arg(usage.uses);
    return usage.compositions == 1
               ? QObject::tr("%1 in 1 composition").arg(times)
               : QObject::tr("%1 in %2 compositions").arg(times).arg(usage.compositions);
}
} // namespace

void AssetsEditor::buildSelectionHeader() {
    selectionHeader_ = new QWidget(this);
    selectionHeader_->setObjectName(QStringLiteral("assetsSelectionHeader"));
    auto* row = new QHBoxLayout(selectionHeader_);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(kit::px(kit::Spacing::S));
    thumbnail_ = new kit::KThumbnail(selectionHeader_);
    thumbnail_->setObjectName(QStringLiteral("assetsSelectionThumbnail"));
    thumbnail_->setAccessibleName(tr("Selection thumbnail"));
    row->addWidget(thumbnail_, 0, Qt::AlignTop);
    auto* facts = new QVBoxLayout;
    facts->setContentsMargins(0, 0, 0, 0);
    facts->setSpacing(0);
    // Every line is one Control-height label, so the tree below never moves with the selection.
    const auto line = [&](const char* name, const kit::TypeRole role) {
        auto* label = new kit::KLabel(QString{}, selectionHeader_, role);
        label->setObjectName(QString::fromLatin1(name));
        label->setFixedHeight(kit::px(kit::Size::Control));
        facts->addWidget(label);
        return label;
    };
    selectionName_ = line("assetsSelectionName", kit::TypeRole::Title);
    for (const auto* name :
         {"assetsSelectionKind", "assetsSelectionColour", "assetsSelectionUsage"})
        selectionFacts_.push_back(line(name, kit::TypeRole::UiSmall));
    row->addLayout(facts, 1);
}

void AssetsEditor::refreshSelectionHeader() {
    if (selectionHeader_ == nullptr || tree_ == nullptr)
        return;
    const auto items = tree_->selectedItems();
    const auto& project = session_.snapshot().project();
    const auto* controller = session_.assetController();
    QString name;
    QString kind, timing, colour, usage;
    QPixmap picture;
    auto glyph = kit::IconId::Folder;
    auto tint = kit::Color::Muted;
    const auto* item = items.size() == 1 ? items.front() : nullptr;
    if (items.size() > 1) {
        name = tr("%1 items selected").arg(items.size());
        glyph = kit::IconId::Stack;
    } else if (const auto* asset = project.findAsset(assetId(item))) {
        name = QString::fromStdString(asset->name);
        glyph = assetKindIcon(*asset);
        tint = assetLabelColor(*asset);
        kind = assetKindText(*asset);
        if (asset->width > 0 && asset->height > 0 && asset->kind != document::AssetKind::Audio)
            kind += QStringLiteral(" · %1 × %2").arg(asset->width).arg(asset->height);
        if (asset->kind == document::AssetKind::Font && !asset->fontFamily.empty())
            kind += QStringLiteral(" · %1 %2")
                        .arg(QString::fromStdString(asset->fontFamily),
                             QString::fromStdString(asset->fontStyle));
        switch (asset->kind) {
        case document::AssetKind::Video:
            if (const auto rate = videoFrameRate(*asset))
                timing = tr("%1, %2 fps")
                             .arg(durationText(framesAt(asset->duration, *rate), *rate),
                                  frameRateText(*rate));
            else
                timing = tr("%1 s").arg(asset->duration.toSeconds(), 0, 'f', 2);
            break;
        case document::AssetKind::Audio:
            timing = tr("%1 s").arg(asset->duration.toSeconds(), 0, 'f', 2);
            if (asset->rate > 0)
                timing += tr(" · %1 Hz · %2 ch").arg(asset->rate).arg(asset->channels);
            break;
        case document::AssetKind::Sequence: {
            const auto rate = sequenceRate(session_.composition());
            timing = tr("%1, %2 fps")
                         .arg(durationText(sequenceFrames(*asset), rate), frameRateText(rate));
            break;
        }
        default:
            break;
        }
        const bool pictures = asset->kind == document::AssetKind::Image ||
                              asset->kind == document::AssetKind::Sequence ||
                              asset->kind == document::AssetKind::Video;
        if (controller && controller->missing(asset->id))
            colour = tr("Missing — Replace Footage to relink it");
        else if (const auto problem = controller ? controller->problem(asset->id) : QString{};
                 !problem.isEmpty())
            colour = problem;
        else if (controller && controller->dataOnly(asset->id))
            colour = tr("Data channels only — use its Channels output");
        else if (pictures) {
            // The plain name of the space, then the alpha, then the colour note once (#380).
            auto input = controller ? controller->inputColorSpaceName(asset->id) : QString{};
            if (input.isEmpty())
                input = controller ? controller->inputColorSpaceDisplay(asset->id) : QString{};
            else if (controller->inputColorSpaceAutomatic(asset->id) &&
                     !controller->inputColorSpaceAssumed(asset->id))
                input = tr("%1 (from the file)").arg(input);
            if (input.isEmpty())
                input = asset->interpretation.inputColorSpaceId.empty()
                            ? tr("Automatic")
                            : colourSpaceDisplayName(asset->interpretation.inputColorSpaceId);
            colour = tr("Colour: %1").arg(input);
            colour += asset->interpretation.alphaAssociation ==
                              document::AssetAlphaAssociation::Premultiplied
                          ? tr(" · Premultiplied alpha")
                          : tr(" · Straight alpha");
            if (const auto warning =
                    controller ? controller->inputColorSpaceWarning(asset->id) : QString{};
                !warning.isEmpty())
                colour += QStringLiteral(" · ") + warning;
        }
        usage = usageText(assetUsage(project, asset->id));
        if (controller && !controller->missing(asset->id))
            if (const auto image = controller->thumbnail(asset->id); !image.isNull())
                picture = QPixmap::fromImage(image);
        if (controller &&
            (controller->missing(asset->id) || !controller->problem(asset->id).isEmpty())) {
            glyph = kit::IconId::Warning;
            tint = kit::Color::Warn;
        }
    } else if (const auto* composition = project.findComposition(compositionId(item))) {
        const auto format = composition->format();
        name = QString::fromStdString(composition->name());
        glyph = kit::IconId::Composition;
        tint = kit::Color::DataComposition;
        kind = tr("Composition · %1 × %2").arg(format.width()).arg(format.height());
        timing = tr("%1, %2 fps")
                     .arg(durationText(framesAt(composition->duration(), format.frameRate()),
                                       format.frameRate()),
                          frameRateText(format.frameRate()));
        usage = usageText(compositionUsage(project, composition->id()));
        // A rendered frame of the composition, prepared on a worker (#380); the kind glyph
        // stands in until the first one arrives.
        if (auto* thumbnails = session_.compositionThumbnails())
            if (const auto image = thumbnails->thumbnail(composition->id()); !image.isNull())
                picture = QPixmap::fromImage(image);
    } else if (const auto folder = folderId(item)) {
        const auto* record = project.findAssetFolder(*folder);
        name = record ? QString::fromStdString(record->name) : QString{};
        tint = kit::Color::Label3;
        kind = item->childCount() == 1 ? tr("Folder · 1 item")
                                       : tr("Folder · %1 items").arg(item->childCount());
    } else if (item != nullptr && isData(item)) {
        name = item->text(NameColumn);
        glyph = item->parent() ? kit::IconId::Info : kit::IconId::Stack;
        kind = item->text(TypeColumn);
    } else {
        name = tr("No selection");
        kind = tr("Select an item to see its details");
        glyph = kit::IconId::Folder;
    }
    if (picture.isNull())
        picture = kit::iconPixmap(glyph, kit::Size::IconControl, tint);
    thumbnail_->setPixmap(picture);
    thumbnail_->setToolTip(name);
    selectionName_->setElidedText(name);
    selectionName_->setToolTip(name);
    // Kind, size, duration and rate share the first line, as in the After Effects header.
    const std::array<QString, 3> lines{timing.isEmpty() ? kind
                                       : kind.isEmpty() ? timing
                                                        : kind + QStringLiteral(" · ") + timing,
                                       colour, usage};
    for (std::size_t index = 0; index < lines.size(); ++index) {
        selectionFacts_[index]->setElidedText(lines[index]);
        selectionFacts_[index]->setToolTip(lines[index]);
    }
}
} // namespace bloom::ui
