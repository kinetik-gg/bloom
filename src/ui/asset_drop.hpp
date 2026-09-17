#pragma once
#include <QMimeData>
#include <bloom/document/ids.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/project.hpp>
class QWidget;
class QGraphicsView;
namespace bloom::ui {
class CompositionSession;
inline constexpr auto kCompositionMimeType = "application/x-bloom-composition";
inline constexpr auto kAssetMimeType = "application/x-bloom-asset";
[[nodiscard]] QByteArray assetMimePayload(const CompositionSession& session,
                                          document::AssetId asset);
[[nodiscard]] document::AssetId assetFromMime(const QMimeData& mime,
                                              const CompositionSession& session);
[[nodiscard]] document::CompositionId compositionFromMime(const QMimeData& mime,
                                                          const CompositionSession& session);
[[nodiscard]] document::CompositionId compositionSourceId(const document::Composition& composition,
                                                          const document::NodeRecord& node);
void installAssetDropTarget(QWidget& widget, CompositionSession& session,
                            QGraphicsView* view = nullptr);
} // namespace bloom::ui
