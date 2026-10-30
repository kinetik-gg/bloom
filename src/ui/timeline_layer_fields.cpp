#include "timeline_layer_fields.hpp"

#include "composition_editor_support.hpp"

#include <bloom/commands/layer_operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/layer_source.hpp>
#include <bloom/document/project.hpp>
#include <bloom/ui/composition_session.hpp>

namespace bloom::ui::layer_fields {
namespace {

const document::LayerOutputBoundary* layerRecord(const CompositionSession& session,
                                                 const document::LayerId layer) {
    const auto* composition = session.composition();
    return composition ? composition->graph().findLayer(layer) : nullptr;
}

} // namespace

bool shy(const CompositionSession& session, const document::LayerId layer) {
    const auto* record = layerRecord(session, layer);
    return record != nullptr && record->shy;
}

bool setShy(CompositionSession& session, const std::vector<document::LayerId>& layers,
            const bool value) {
    // Shy is presentation (document 1.30): one undoable step that never invalidates a render.
    commands::Transaction transaction("Set Layer Shy", session.snapshot().revision());
    bool any = false;
    for (const auto layer : layers) {
        const auto* record = layerRecord(session, layer);
        if (record == nullptr || record->shy == value)
            continue;
        transaction.emplace<commands::SetLayerShy>(session.compositionId(), layer, value);
        any = true;
    }
    return any && session.executeTransaction(std::move(transaction)).changed();
}

QString sourceName(const CompositionSession& session, const document::LayerId layer) {
    const auto* composition = session.composition();
    const auto* record = layerRecord(session, layer);
    if (composition == nullptr || record == nullptr)
        return {};
    const auto name =
        document::layerSourceName(session.snapshot().project(), *composition, *record);
    return name ? QString::fromStdString(*name) : QString{};
}

bool nameFollowsSource(const CompositionSession& session, const document::LayerId layer) {
    const auto* record = layerRecord(session, layer);
    return record != nullptr && record->nameFollowsSource;
}

QString displayName(const CompositionSession& session, const document::LayerId layer) {
    const auto* composition = session.composition();
    if (composition == nullptr)
        return {};
    // The document keeps a following layer's name equal to its source's (renaming or replacing
    // the source re-applies it in the same step); the source's own name is read directly so the
    // table never shows a stale name for a following layer.
    if (nameFollowsSource(session, layer))
        if (auto name = sourceName(session, layer); !name.isEmpty())
            return name;
    return layerName(*composition, layer);
}

} // namespace bloom::ui::layer_fields
