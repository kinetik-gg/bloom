#include "timeline_reveal.hpp"

#include "timeline_key_time.hpp"
#include "timeline_mask_rows.hpp"
#include "timeline_property_rows.hpp"

#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/project.hpp>
#include <bloom/runtime/layer_time_projection.hpp>
#include <bloom/ui/composition_session.hpp>

#include <QObject>

#include <algorithm>

namespace bloom::ui::timeline_reveal {
namespace {

[[nodiscard]] std::string_view roleFor(const Reveal reveal) {
    switch (reveal) {
    case Reveal::Anchor:
        return document::kAnchorParameterRole;
    case Reveal::Position:
        return document::kPositionParameterRole;
    case Reveal::Scale:
        return document::kScaleParameterRole;
    case Reveal::Rotation:
        return document::kRotationParameterRole;
    case Reveal::Opacity:
        return document::kOpacityParameterRole;
    default:
        return {};
    }
}

[[nodiscard]] bool keyed(const document::Composition& composition,
                         const document::ParameterId parameter) {
    const auto* record = composition.parameters().find(parameter);
    return record != nullptr &&
           std::holds_alternative<document::AnimationCurveSource>(record->source);
}

// A constant that differs from what a new layer starts with: the node definition's own default,
// except Position, which a new layer takes at the composition centre (as in After Effects).
[[nodiscard]] bool differsFromDefault(const document::Composition& composition,
                                      const document::ParameterId parameter,
                                      const std::string& role) {
    const auto* record = composition.parameters().find(parameter);
    const auto* constant =
        record ? std::get_if<document::ConstantValueSource>(&record->source) : nullptr;
    if (constant == nullptr)
        return false;
    if (role == document::kPositionParameterRole) {
        const auto& format = composition.format();
        return constant->value != document::ParameterValue{
                                      document::Vec2d{format.width() / 2.0, format.height() / 2.0}};
    }
    for (const auto& node : composition.graph().nodes()) {
        const auto binding =
            std::ranges::find(node.parameters, parameter, &document::ParameterBinding::parameterId);
        if (binding == node.parameters.end())
            continue;
        const auto* definition =
            document::builtInNodeDefinitions().find(node.typeId, node.schemaVersion);
        if (definition == nullptr)
            return false;
        const auto declared = std::ranges::find(definition->parameters, binding->role,
                                                &document::ParameterDefinition::role);
        return declared != definition->parameters.end() &&
               constant->value != declared->defaultValue;
    }
    return false;
}

} // namespace

std::vector<RowKey> rowKeys(const CompositionSession& session, const TimelineLayerEntry& row) {
    std::vector<RowKey> result;
    const auto* composition = session.composition();
    const auto* parameter = composition ? composition->parameters().find(row.parameterId) : nullptr;
    const auto* source =
        parameter ? std::get_if<document::AnimationCurveSource>(&parameter->source) : nullptr;
    const auto* curve = source ? composition->animationCurves().find(source->curveId) : nullptr;
    if (curve == nullptr)
        return result;
    const KeyClock clock = session.keyClockForCurve(source->curveId);
    std::visit(
        [&](const auto& record) {
            using Curve = std::decay_t<decltype(record)>;
            if constexpr (document::kWholeValueAnimationCurve<Curve>) {
                for (const auto& key : record.keyframes)
                    result.push_back({{source->curveId, key.id, std::nullopt},
                                      projectedKeyTime(clock, key.time)});
            } else {
                const auto first = std::is_same_v<Curve, document::Color4AnimationCurve>
                                       ? document::AnimationComponent::Red
                                       : document::AnimationComponent::X;
                for (std::size_t index = 0; index < record.components.size(); ++index) {
                    const auto component = static_cast<document::AnimationComponent>(
                        static_cast<std::size_t>(first) + index);
                    if (row.component && *row.component != component)
                        continue;
                    for (const auto& key : record.components[index].keyframes)
                        result.push_back({{source->curveId, key.id, component},
                                          projectedKeyTime(clock, key.time)});
                }
            }
        },
        *curve);
    return result;
}

std::set<document::ParameterId> revealedParameters(const CompositionSession& session,
                                                   const document::LayerId layer,
                                                   const Reveal reveal) {
    std::set<document::ParameterId> result;
    const auto* composition = session.composition();
    if (composition == nullptr)
        return result;
    if (const auto role = roleFor(reveal); !role.empty()) {
        const auto boundary = session.boundaryNodeForLayer(layer);
        const auto* node = boundary ? composition->graph().findNode(*boundary) : nullptr;
        if (node != nullptr)
            for (const auto& binding : node->parameters)
                if (binding.role == role)
                    result.insert(binding.parameterId);
        return result;
    }
    TimelineLayerEntry seed;
    seed.layerId = layer;
    const auto sourceGroup = QObject::tr("Source");
    for (const auto& row : timelinePropertyEntries(session, {seed}, {layer})) {
        if (row.rowKind != TimelineLayerEntry::Kind::Parameter)
            continue;
        const bool maskPath = row.maskId.isValid() && row.role == kMaskPathRowRole;
        bool shown = false;
        switch (reveal) {
        case Reveal::Keyed:
            shown = keyed(*composition, row.parameterId);
            break;
        case Reveal::Modified:
            // A mask is a change made to the layer, so its path always counts as modified.
            shown = keyed(*composition, row.parameterId) || maskPath ||
                    (!row.maskId.isValid() &&
                     differsFromDefault(*composition, row.parameterId, row.role));
            break;
        case Reveal::Masks:
            shown = maskPath;
            break;
        case Reveal::Effects:
            // Bloom has no effects stack: what processes a layer is the node feeding it (the
            // Source group) and the value nodes driving it (the upstream groups).
            shown = row.group == sourceGroup || row.group.startsWith(QStringLiteral("upstream-"));
            break;
        default:
            break;
        }
        if (shown)
            result.insert(row.parameterId);
    }
    return result;
}

std::optional<std::set<document::ParameterId>>
nextReveal(const std::optional<std::set<document::ParameterId>>& current,
           const std::set<document::ParameterId>& wanted, const bool additive) {
    if (!additive) {
        if (wanted.empty() || current == wanted)
            return std::nullopt;
        return wanted;
    }
    auto next = current.value_or(std::set<document::ParameterId>{});
    if (!wanted.empty() && std::ranges::includes(next, wanted)) {
        for (const auto parameter : wanted)
            next.erase(parameter);
    } else {
        next.insert(wanted.begin(), wanted.end());
    }
    if (next.empty())
        return std::nullopt;
    return next;
}

std::optional<core::RationalTime> adjacentTime(const std::vector<core::RationalTime>& times,
                                               const core::RationalTime now, const bool forward) {
    std::optional<core::RationalTime> best;
    for (const auto time : times) {
        if (forward ? !(now < time) : !(time < now))
            continue;
        if (!best || (forward ? time < *best : *best < time))
            best = time;
    }
    return best;
}

std::optional<commands::LayerTimingProposal> playheadTiming(const CompositionSession& session,
                                                            const document::LayerId layer,
                                                            const PlayheadEdit edit) {
    const auto* composition = session.composition();
    const auto* record = composition ? composition->graph().findLayer(layer) : nullptr;
    if (record == nullptr)
        return std::nullopt;
    const auto rate = composition->format().frameRate();
    const double frame = static_cast<double>(rate.denominator()) / rate.numerator();
    const double now = session.currentTime().toSeconds();
    const double in = record->inPoint.toSeconds();
    const double out = record->endPoint(composition->duration()).toSeconds();
    commands::LayerTimingGestureInput input{.start = record->timing.startTime,
                                            .in = record->inPoint,
                                            .out = record->endPoint(composition->duration()),
                                            .speed = record->timing.speed,
                                            .rate = rate,
                                            .duration = composition->duration(),
                                            .extentStart = std::nullopt,
                                            .extentEnd = std::nullopt,
                                            .handle = 0,
                                            .alt = false,
                                            .deltaSeconds = 0.0};
    if (const auto extent = runtime::resolveLayerExtent(session.snapshot().project(), *composition,
                                                        record->timing)) {
        input.extentStart = runtime::placementTimeOfLayerTime(record->timing, {});
        input.extentEnd = runtime::placementTimeOfLayerTime(record->timing, extent->duration);
    }
    // The out point is exclusive, so a layer ending "at" the playhead keeps the playhead's frame.
    switch (edit) {
    case PlayheadEdit::MoveIn:
        input.deltaSeconds = now - in;
        break;
    case PlayheadEdit::MoveOut:
        input.deltaSeconds = now + frame - out;
        break;
    case PlayheadEdit::TrimIn:
        input.handle = -1;
        input.deltaSeconds = now - in;
        break;
    case PlayheadEdit::TrimOut:
        input.handle = 1;
        input.deltaSeconds = now + frame - out;
        break;
    }
    return commands::proposeLayerTiming(input);
}

} // namespace bloom::ui::timeline_reveal
