#pragma once

#include <bloom/document/asset.hpp>
#include <bloom/host/font_catalogue.hpp>

#include <QString>

#include <optional>

namespace bloom::ui {

class CompositionSession;

// The font catalogue a text font control offers, shared by Properties and the context bar so the
// process scans the installed fonts once. The scan runs off the UI thread; callers poll().
[[nodiscard]] host::FontCatalogueCache& fontCatalogueCache();

// A face's stable reference ("family|style|digest"): what a font control stores as item data.
[[nodiscard]] QString fontReference(const platform::FontFace& face);
// The Font asset a text font parameter points at once the face is chosen (EnsureFontAsset).
[[nodiscard]] document::AssetRecord fontAssetRecord(const platform::FontFace& face);
// The Font asset for a reference the catalogue offers; nullopt for an empty reference, or while the
// catalogue has not listed that face.
[[nodiscard]] std::optional<document::AssetRecord> fontAssetForReference(const QString& reference);
// The reference of the face a text font parameter's stored value (an asset id, or a reference)
// names in the session's project.
[[nodiscard]] QString storedFontReference(const CompositionSession& session, const QString& stored);

} // namespace bloom::ui
