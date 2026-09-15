#pragma once
#include <QMimeData>
#include <bloom/document/ids.hpp>
#include <bloom/document/parameter.hpp>
class QWidget;
class QGraphicsView;
namespace bloom::ui {
class CompositionSession;
inline constexpr auto kAssetMimeType = "application/x-bloom-asset";
[[nodiscard]] document::AssetId assetFromMime(const QMimeData& mime,
                                              const CompositionSession& session);
void installAssetDropTarget(QWidget& widget, CompositionSession& session,
                            QGraphicsView* view = nullptr);
} // namespace bloom::ui
