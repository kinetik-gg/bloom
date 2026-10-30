#include "timeline_mask_rows.hpp"

#include "layer_mask_support.hpp"

#include <QCoreApplication>
#include <bloom/document/layer_masks.hpp>
#include <bloom/document/project.hpp>
#include <bloom/ui/composition_session.hpp>

#include <algorithm>

namespace bloom::ui {

QString maskGroupKey() { return QCoreApplication::translate("TimelineMaskRows", "Masks"); }

void appendMaskEntries(const CompositionSession& session, const TimelineLayerEntry& layer,
                       const std::set<std::pair<document::LayerId, QString>>& collapsedGroups,
                       std::vector<TimelineLayerEntry>& rows,
                       const std::set<document::ParameterId>* reveal) {
    const auto* composition = session.composition();
    const auto* boundary = composition ? composition->graph().findLayer(layer.layerId) : nullptr;
    if (boundary == nullptr || boundary->masks.empty())
        return;
    if (reveal != nullptr &&
        std::ranges::none_of(boundary->masks, [&](const document::LayerMask& mask) {
            return std::ranges::any_of(document::layerMaskParameters(mask), [&](const auto& item) {
                return reveal->contains(item.first);
            });
        }))
        return;
    auto group = layer;
    group.rowKind = TimelineLayerEntry::Kind::Group;
    group.depth = 1;
    group.name = maskGroupKey();
    group.group = maskGroupKey();
    group.expanded = reveal != nullptr || !collapsedGroups.contains({layer.layerId, group.group});
    rows.push_back(group);
    if (!group.expanded)
        return;
    const auto parameterRow = [&](const document::LayerMask& mask, const document::ParameterId id,
                                  const std::string_view role, QString name, const int depth) {
        if (reveal != nullptr && !reveal->contains(id))
            return;
        auto entry = layer;
        entry.group = maskGroupKey();
        entry.rowKind = TimelineLayerEntry::Kind::Parameter;
        entry.depth = depth;
        entry.name = std::move(name);
        entry.role = std::string(role);
        entry.parameterId = id;
        entry.maskId = mask.id;
        entry.expanded = false;
        rows.push_back(std::move(entry));
    };
    for (const auto& mask : boundary->masks) {
        auto label =
            QString::fromStdString(mask.name) + QStringLiteral(" · ") + masks::modeText(mask.mode);
        if (mask.inverted)
            label +=
                QStringLiteral(" · ") + QCoreApplication::translate("TimelineMaskRows", "Inverted");
        parameterRow(mask, mask.path, kMaskPathRowRole, std::move(label), 2);
        const auto tr = [](const char* text) {
            return QCoreApplication::translate("TimelineMaskRows", text);
        };
        parameterRow(mask, mask.feather, kMaskFeatherRowRole, tr("Feather"), 3);
        parameterRow(mask, mask.expansion, kMaskExpansionRowRole, tr("Expansion"), 3);
        parameterRow(mask, mask.opacity, kMaskOpacityRowRole, tr("Opacity"), 3);
    }
}

} // namespace bloom::ui
