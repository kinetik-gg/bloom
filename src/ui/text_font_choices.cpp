#include "text_font_choices.hpp"

#include <bloom/document/project.hpp>
#include <bloom/ui/composition_session.hpp>

#include <QUrl>

#include <filesystem>

namespace bloom::ui {

QString fontReference(const platform::FontFace& face) {
    const auto digest = face.contentDigest.toLowercaseHex();
    return QString::fromStdString(face.family) + QLatin1Char('|') +
           QString::fromStdString(face.style) + QLatin1Char('|') +
           QString::fromLatin1(digest.data(), static_cast<qsizetype>(digest.size()));
}

document::AssetRecord fontAssetRecord(const platform::FontFace& face) {
    document::AssetRecord asset;
    asset.kind = document::AssetKind::Font;
    asset.contentDigest = face.contentDigest;
    asset.fontFamily = face.family;
    asset.fontStyle = face.style;
    asset.fontIndex = face.faceIndex;
    if (face.source == platform::FontSource::Embedded) {
        asset.locator = {"font", "builtin", "embedded/" + std::to_string(face.faceIndex),
                         "font:embedded:" + face.family + "|" + face.style};
    } else {
        const auto absolute = std::filesystem::absolute(face.path).lexically_normal();
        const auto path = absolute.generic_string();
        asset.locator = {
            "font", "system", path,
            QUrl::fromLocalFile(QString::fromStdString(path)).toEncoded().toStdString()};
    }
    return asset;
}

host::FontCatalogueCache& fontCatalogueCache() {
    static host::FontCatalogueCache cache;
    static bool requested = false;
    if (!requested) {
        cache.refresh();
        requested = true;
    }
    return cache;
}

std::optional<document::AssetRecord> fontAssetForReference(const QString& reference) {
    if (reference.isEmpty())
        return std::nullopt;
    auto& cache = fontCatalogueCache();
    (void)cache.poll();
    const auto snapshot = cache.snapshot();
    if (!snapshot)
        return std::nullopt;
    for (const auto& face : snapshot->faces)
        if (fontReference(face) == reference)
            return fontAssetRecord(face);
    return std::nullopt;
}

QString storedFontReference(const CompositionSession& session, const QString& stored) {
    bool numeric = false;
    const auto assetNumber = stored.toULongLong(&numeric);
    if (!numeric)
        return stored;
    const auto* asset =
        session.snapshot().project().findAsset(document::AssetId::fromRaw(assetNumber));
    if (asset == nullptr || asset->kind != document::AssetKind::Font)
        return stored;
    const auto digest = asset->contentDigest.toLowercaseHex();
    return QString::fromStdString(asset->fontFamily) + QLatin1Char('|') +
           QString::fromStdString(asset->fontStyle) + QLatin1Char('|') +
           QString::fromLatin1(digest.data(), static_cast<qsizetype>(digest.size()));
}

} // namespace bloom::ui
