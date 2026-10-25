#pragma once
#include <QMimeData>
#include <bloom/document/ids.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/project.hpp>
#include <variant>
#include <vector>
class QWidget;
class QGraphicsView;
namespace bloom::ui {
class CompositionSession;
inline constexpr auto kCompositionMimeType = "application/x-bloom-composition";
inline constexpr auto kAssetMimeType = "application/x-bloom-asset";
// Every asset and composition an Assets drag carries, in selection order, for layer drops (#380).
inline constexpr auto kLayerItemsMimeType = "application/x-bloom-layer-items";
[[nodiscard]] QByteArray assetMimePayload(const CompositionSession& session,
                                          document::AssetId asset);
[[nodiscard]] document::AssetId assetFromMime(const QMimeData& mime,
                                              const CompositionSession& session);
[[nodiscard]] document::CompositionId compositionFromMime(const QMimeData& mime,
                                                          const CompositionSession& session);
// Valid only for this session and revision, like assetMimePayload.
[[nodiscard]] QByteArray layerItemsMimePayload(
    const CompositionSession& session,
    const std::vector<std::variant<document::AssetId, document::CompositionId>>& items);
[[nodiscard]] std::vector<std::variant<document::AssetId, document::CompositionId>>
layerItemsFromMime(const QMimeData& mime, const CompositionSession& session);
[[nodiscard]] document::CompositionId compositionSourceId(const document::Composition& composition,
                                                          const document::NodeRecord& node);
// With `view` (the Nodes canvas): an asset or composition becomes a source node at the drop point
// and files are imported. Without one the widget is a layer drop target (layer_drop.hpp).
void installAssetDropTarget(QWidget& widget, CompositionSession& session,
                            QGraphicsView* view = nullptr);
} // namespace bloom::ui
