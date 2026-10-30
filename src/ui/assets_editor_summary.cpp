// The Assets summary strip (#378, lane P): one fixed, two-line strip above the tree. Its visible
// lines are short facts to skim, separated by thin dots; the full sentence of each is its tooltip.
// It reads the project and the controllers' published state only; thumbnails are decoded by the
// asset controller's worker and compositions are rendered by the composition thumbnail worker.
#include "assets_editor_internal.hpp"
#include "colour_space_names.hpp"
#include <QPixmap>
#include <QResizeEvent>
#include <QTreeWidget>
#include <algorithm>
#include <array>
#include <bloom/document/project.hpp>
#include <bloom/ui/asset_controller.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/composition_thumbnail_controller.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/thumbnail.hpp>

namespace bloom::ui::assets {
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

QString chips(const QStringList& parts) {
    QStringList shown;
    for (const auto& part : parts)
        if (!part.isEmpty())
            shown.push_back(part);
    return shown.join(QStringLiteral(" · "));
}

QString dimensions(const std::uint64_t width, const std::uint64_t height) {
    return width > 0 && height > 0 ? QStringLiteral("%1 × %2").arg(width).arg(height) : QString{};
}

// "0:00:16:02 · 386 f · 24 fps": the timecode, the frame count and the rate as three facts.
QStringList timing(const std::uint64_t frames, const document::FrameRate rate) {
    return {timecodeForFrames(frames, rate), QObject::tr("%1 f").arg(frames),
            QObject::tr("%1 fps").arg(frameRateText(rate))};
}

QString counted(const std::size_t count, const QString& one, const QString& many) {
    return count == 1 ? QStringLiteral("1 %1").arg(one)
                      : QStringLiteral("%1 %2").arg(count).arg(many);
}
} // namespace

void KindCounts::add(const QTreeWidgetItem& item, const document::Project& project) {
    if (isData(&item)) {
        data += item.parent() != nullptr ? 1 : 0;
        return;
    }
    if (folderId(&item)) {
        ++folders;
        return;
    }
    if (compositionId(&item).isValid()) {
        ++compositions;
        return;
    }
    const auto* asset = project.findAsset(assetId(&item));
    if (asset == nullptr)
        return;
    switch (asset->kind) {
    case document::AssetKind::Sequence:
        ++sequences;
        break;
    case document::AssetKind::Video:
        ++videos;
        break;
    case document::AssetKind::Audio:
        ++audio;
        break;
    case document::AssetKind::Font:
        ++fonts;
        break;
    case document::AssetKind::Lut:
        ++luts;
        break;
    default:
        ++images;
        break;
    }
}

std::size_t KindCounts::items() const noexcept {
    return compositions + sequences + images + videos + audio + fonts + luts;
}

QString KindCounts::text() const {
    std::array<std::pair<std::size_t, QString>, 9> parts{{
        {compositions,
         counted(compositions, QObject::tr("composition"), QObject::tr("compositions"))},
        {sequences, counted(sequences, QObject::tr("sequence"), QObject::tr("sequences"))},
        {images, counted(images, QObject::tr("image"), QObject::tr("images"))},
        {videos, counted(videos, QObject::tr("video"), QObject::tr("videos"))},
        {audio, counted(audio, QObject::tr("audio file"), QObject::tr("audio files"))},
        {fonts, counted(fonts, QObject::tr("font"), QObject::tr("fonts"))},
        {luts, counted(luts, QObject::tr("LUT"), QObject::tr("LUTs"))},
        {folders, counted(folders, QObject::tr("folder"), QObject::tr("folders"))},
        {data, counted(data, QObject::tr("data block"), QObject::tr("data blocks"))},
    }};
    // Most numerous media first; ties keep the order above. Folders and data blocks follow.
    std::stable_sort(parts.begin(), parts.begin() + 7,
                     [](const auto& left, const auto& right) { return left.first > right.first; });
    QStringList shown;
    for (const auto& [count, label] : parts)
        if (count > 0)
            shown.push_back(label);
    return shown.join(QStringLiteral(" · "));
}

SummaryStrip::SummaryStrip(CompositionSession& session, const QTreeWidget& tree, QWidget* parent)
    : QWidget(parent), session_(session), tree_(&tree) {
    setObjectName(QStringLiteral("assetsSelectionHeader"));
    setFixedHeight(kit::px(kit::Size::SummaryStrip));
    thumbnail_ = new kit::KThumbnail(this, kit::KThumbnail::Cell::Strip);
    thumbnail_->setObjectName(QStringLiteral("assetsSelectionThumbnail"));
    thumbnail_->setAccessibleName(tr("Selection thumbnail"));
    const auto line = [this](const char* name, const kit::TypeRole role, const kit::Color tone) {
        auto* label = new kit::KLabel(QString{}, this, role);
        label->setObjectName(QString::fromLatin1(name));
        label->setTone(tone);
        return label;
    };
    // Size and ink rank the facts: the name in Ui Foreground, the facts in UiSmall Muted, and the
    // usage, least needed, in UiSmall Faint.
    name_ = line("assetsSelectionName", kit::TypeRole::Ui, kit::Color::Foreground);
    usage_ = line("assetsSelectionUsage", kit::TypeRole::UiSmall, kit::Color::Faint);
    usage_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    kind_ = line("assetsSelectionKind", kit::TypeRole::UiSmall, kit::Color::Muted);
    colour_ = line("assetsSelectionColour", kit::TypeRole::UiSmall, kit::Color::Muted);
}

void SummaryStrip::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    place();
}

void SummaryStrip::place() {
    const int line = kit::px(kit::Size::Control);
    const int gap = kit::px(kit::Spacing::S);
    const int left = thumbnail_->isHidden() ? 0 : thumbnail_->width() + gap;
    const int width = std::max(0, this->width() - left);
    thumbnail_->move(0, (height() - thumbnail_->height()) / 2);
    // With nothing selected the one summary line is centred in the strip's fixed height.
    const bool single = name_->isHidden();
    const int top = single ? (height() - line) / 2 : 0;
    const auto natural = [](const kit::KLabel& label, const QString& text) {
        return text.isEmpty() ? 0 : label.fontMetrics().horizontalAdvance(text) + 1;
    };
    // Line 1: the name takes what the trailing usage leaves (the usage never takes over half).
    const int usage = std::min(natural(*usage_, usageText_), width / 2);
    usage_->setGeometry(this->width() - usage, top, usage, line);
    name_->setGeometry(left, top, std::max(0, width - usage - (usage > 0 ? gap : 0)), line);
    // Line 2: the kind facts first, at their own width; the colour facts take the rest.
    const int kindTop = single ? top : line;
    const int kind = std::min(natural(*kind_, kindText_), width);
    kind_->setGeometry(left, kindTop, kind, line);
    const int colourLeft = left + kind;
    colour_->setGeometry(colourLeft, kindTop, std::max(0, this->width() - colourLeft), line);
}

void SummaryStrip::refresh(const QList<QTreeWidgetItem*>& selection) {
    const auto& project = session_.snapshot().project();
    const auto* controller = session_.assetController();
    QString name, usage;
    QString kindShort, kindLong, colourShort, colourLong;
    auto colourTone = kit::Color::Muted;
    auto kindTone = kit::Color::Muted;
    QPixmap picture;
    auto icon = kit::IconId::Folder;
    auto tint = kit::Color::Muted;
    bool summary = false;
    const auto* item = selection.size() == 1 ? selection.front() : nullptr;
    if (selection.size() > 1) {
        KindCounts counts;
        for (const auto* chosen : selection)
            counts.add(*chosen, project);
        name = tr("%1 items selected").arg(selection.size());
        kindShort = kindLong = counts.text();
        icon = kit::IconId::Stack;
    } else if (const auto* asset = project.findAsset(assetId(item))) {
        name = QString::fromStdString(asset->name);
        icon = assetKindIcon(*asset);
        const auto kind = assetKindText(*asset);
        const auto size = asset->kind == document::AssetKind::Audio
                              ? QString{}
                              : dimensions(asset->width, asset->height);
        QStringList facts{kind, size};
        kindLong = chips({kind, size});
        if (asset->kind == document::AssetKind::Font && !asset->fontFamily.empty()) {
            const auto face =
                QStringLiteral("%1 %2").arg(QString::fromStdString(asset->fontFamily),
                                            QString::fromStdString(asset->fontStyle));
            facts.push_back(face);
            kindLong = chips({kindLong, face});
        }
        QString timingLong;
        switch (asset->kind) {
        case document::AssetKind::Video:
            if (const auto rate = videoFrameRate(*asset)) {
                const auto frames = framesAt(asset->duration, *rate);
                facts += timing(frames, *rate);
                timingLong =
                    tr("%1, %2 fps").arg(durationText(frames, *rate), frameRateText(*rate));
            } else {
                timingLong = tr("%1 s").arg(asset->duration.toSeconds(), 0, 'f', 2);
                facts.push_back(timingLong);
            }
            break;
        case document::AssetKind::Audio:
            timingLong = tr("%1 s").arg(asset->duration.toSeconds(), 0, 'f', 2);
            facts.push_back(timingLong);
            if (asset->rate > 0) {
                facts.push_back(tr("%1 Hz").arg(asset->rate));
                facts.push_back(tr("%1 ch").arg(asset->channels));
                timingLong += tr(" · %1 Hz · %2 ch").arg(asset->rate).arg(asset->channels);
            }
            break;
        case document::AssetKind::Sequence: {
            const auto rate = sequenceRate(session_.composition());
            facts += timing(sequenceFrames(*asset), rate);
            timingLong = tr("%1, %2 fps")
                             .arg(durationText(sequenceFrames(*asset), rate), frameRateText(rate));
            break;
        }
        default:
            break;
        }
        kindShort = chips(facts);
        if (!timingLong.isEmpty())
            kindLong = chips({kindLong, timingLong});
        const bool pictures = asset->kind == document::AssetKind::Image ||
                              asset->kind == document::AssetKind::Sequence ||
                              asset->kind == document::AssetKind::Video;
        const bool missing = controller && controller->missing(asset->id);
        const auto problem = controller ? controller->problem(asset->id) : QString{};
        if (missing) {
            colourShort = colourLong = tr("Missing — Replace Footage to relink it");
            colourTone = kit::Color::Warn;
        } else if (!problem.isEmpty()) {
            colourShort = colourLong = problem;
            colourTone = kit::Color::Warn;
        } else if (controller && controller->dataOnly(asset->id)) {
            colourShort = colourLong = tr("Data channels only — use its Channels output");
        } else if (pictures) {
            // The plain name of the space, then the alpha, then the colour note once (#380).
            auto input = controller ? controller->inputColorSpaceName(asset->id) : QString{};
            auto space = input;
            if (input.isEmpty())
                space = input =
                    controller ? controller->inputColorSpaceDisplay(asset->id) : QString{};
            else if (controller->inputColorSpaceAutomatic(asset->id) &&
                     !controller->inputColorSpaceAssumed(asset->id))
                input = tr("%1 (from the file)").arg(input);
            if (input.isEmpty())
                space = input =
                    asset->interpretation.inputColorSpaceId.empty()
                        ? tr("Automatic")
                        : colourSpaceDisplayName(asset->interpretation.inputColorSpaceId);
            const bool premultiplied = asset->interpretation.alphaAssociation ==
                                       document::AssetAlphaAssociation::Premultiplied;
            colourLong = tr("Color: %1").arg(input) +
                         (premultiplied ? tr(" · Premultiplied alpha") : tr(" · Straight alpha"));
            QStringList colourFacts{space, premultiplied ? tr("Premultiplied") : tr("Straight")};
            if (const auto warning =
                    controller ? controller->inputColorSpaceWarning(asset->id) : QString{};
                !warning.isEmpty()) {
                // One warning, in Warn; its whole sentence is the tooltip.
                colourLong += QStringLiteral(" · ") + warning;
                colourFacts.push_back(warning);
                colourTone = kit::Color::Warn;
            }
            colourShort = chips(colourFacts);
        }
        usage = usageText(assetUsage(project, asset->id));
        if (controller && !missing)
            if (const auto image = controller->thumbnail(asset->id); !image.isNull())
                picture = QPixmap::fromImage(image);
        if (missing || !problem.isEmpty()) {
            icon = kit::IconId::Warning;
            tint = kit::Color::Warn;
        }
    } else if (const auto* composition = project.findComposition(compositionId(item))) {
        const auto format = composition->format();
        const auto frames = framesAt(composition->duration(), format.frameRate());
        name = QString::fromStdString(composition->name());
        icon = kit::IconId::Composition;
        const auto size = dimensions(format.width(), format.height());
        QStringList facts{tr("Composition"), size};
        facts += timing(frames, format.frameRate());
        kindShort = chips(facts);
        kindLong = tr("Composition · %1 · %2, %3 fps")
                       .arg(size, durationText(frames, format.frameRate()),
                            frameRateText(format.frameRate()));
        usage = usageText(compositionUsage(project, composition->id()));
        // A rendered frame of the composition, prepared on a worker (#380); the film strip stands
        // in until the first one arrives.
        if (auto* thumbnails = session_.compositionThumbnails())
            if (const auto image = thumbnails->thumbnail(composition->id()); !image.isNull())
                picture = QPixmap::fromImage(image);
    } else if (const auto folder = folderId(item)) {
        const auto* record = project.findAssetFolder(*folder);
        name = record ? QString::fromStdString(record->name) : QString{};
        icon = item->isExpanded() ? kit::IconId::FolderOpen : kit::IconId::Folder;
        kindShort = kindLong = item->childCount() == 1
                                   ? tr("Folder · 1 item")
                                   : tr("Folder · %1 items").arg(item->childCount());
    } else if (item != nullptr && isData(item)) {
        name = item->text(NameColumn);
        icon = item->parent() ? kit::IconId::Info : kit::IconId::Stack;
        kindShort = kindLong = item->text(TypeColumn);
    } else {
        // Nothing selected: what the project holds, in one Faint line.
        summary = true;
        KindCounts counts;
        if (const auto* tree = tree_)
            for (int index = 0; index < tree->topLevelItemCount(); ++index) {
                std::vector<const QTreeWidgetItem*> pending{tree->topLevelItem(index)};
                while (!pending.empty()) {
                    const auto* next = pending.back();
                    pending.pop_back();
                    counts.add(*next, project);
                    for (int child = 0; child < next->childCount(); ++child)
                        pending.push_back(next->child(child));
                }
            }
        const auto items = counts.items();
        kindShort = items == 0 && counts.folders == 0
                        ? tr("No items yet — import files or drop them here")
                        : chips({counted(items, tr("item"), tr("items")), counts.text()});
        kindLong = kindShort;
        kindTone = kit::Color::Faint;
    }
    if (picture.isNull())
        picture = kit::iconPixmap(icon, kit::Size::IconLarge, tint);
    thumbnail_->setVisible(!summary);
    thumbnail_->setPixmap(picture);
    thumbnail_->setToolTip(name);
    name_->setVisible(!summary);
    usage_->setVisible(!summary);
    colour_->setVisible(!summary);
    nameText_ = name;
    usageText_ = usage;
    kindText_ = kindShort;
    colourText_ = colourShort.isEmpty() || kindShort.isEmpty()
                      ? colourShort
                      : QStringLiteral(" · ") + colourShort;
    kind_->setTone(kindTone);
    colour_->setTone(colourTone);
    name_->setElidedText(nameText_);
    usage_->setElidedText(usageText_);
    kind_->setElidedText(kindText_);
    colour_->setElidedText(colourText_);
    // The visible facts are short; each tooltip is the whole sentence.
    kind_->setToolTip(kindLong);
    colour_->setToolTip(colourLong);
    place();
}

} // namespace bloom::ui::assets
