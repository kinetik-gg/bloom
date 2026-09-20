#pragma once

#include <bloom/core/rational_time.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/document/validation.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace bloom::commands {

enum class OperationStatus {
    Applied,
    NoChange,
    Rejected,
};

enum class OperationIssueCode {
    InvalidTarget,
    InvalidValue,
    DuplicateId,
    MissingReference,
    InvalidOrder,
    Unsupported,
    GraphCycle,
    SocketKindMismatch,
};

struct OperationIssue {
    OperationIssueCode code;
    std::string message;

    friend bool operator==(const OperationIssue&, const OperationIssue&) = default;
};

using DurableObjectId =
    std::variant<document::CompositionId, document::NodeId, document::EdgeId, document::LayerId,
                 document::LayerSlotId, document::ParameterId, document::AnimationCurveId,
                 document::KeyframeId, document::DriverBindingId, document::NodeGroupId,
                 document::AssetId, document::AssetFolderId, document::DataBlockRecordId>;

struct OperationOutput {
    std::string name;
    DurableObjectId id;

    friend bool operator==(const OperationOutput&, const OperationOutput&) = default;
};

// A finite, half-open composition-time interval [start, end) within ONE composition. The
// composition identity travels with the footprint so a consumer cannot silently apply an interval
// computed for one composition to another; a different composition is conservatively unknown.
struct AffectedTimeRange final {
    core::RationalTime start;
    core::RationalTime end;

    friend bool operator==(const AffectedTimeRange&, const AffectedTimeRange&) = default;
};

// The proven changed-time footprint of one applied operation, scoped to a single composition. A
// present footprint means the operation PROVED its rendered-output change is confined to these
// intervals; an empty interval list is a deliberate "nothing changed" and is never inferred from an
// unknown operation. std::nullopt on OperationResult instead means the conservative whole-render
// default. Intervals are normalized: sorted, non-overlapping, and non-adjacent (touching intervals
// are merged), so two footprints describing the same change compare equal.
struct AffectedTimeFootprint final {
    document::CompositionId compositionId;
    std::vector<AffectedTimeRange> intervals;

    friend bool operator==(const AffectedTimeFootprint&, const AffectedTimeFootprint&) = default;
};

// SPLIT-1. One ordered, composition/time-scoped layer-identity remap: over the half-open
// composition-time range [start, end) the output that was produced as `beforeLayerId` /
// `beforeNodeId` is now produced as `afterLayerId` / `afterNodeId`. Execute order maps the original
// head to the split tail; undoing reverses the order and swaps before/after (blind replay of the
// execute list would be wrong). The remap carries no pixels: it only says which current layer/node
// identity a retained pre-edit frame's geometry must be translated to. Only a proven
// output-equivalent split publishes one; nothing here relabels a plan or a sourceRevision.
struct LayerIdentityRemap final {
    document::CompositionId compositionId;
    core::RationalTime start;
    core::RationalTime end;
    document::LayerId beforeLayerId;
    document::LayerId afterLayerId;
    document::NodeId beforeNodeId;
    document::NodeId afterNodeId;

    friend bool operator==(const LayerIdentityRemap&, const LayerIdentityRemap&) = default;
};

// The cap past which interval bookkeeping stops being worth it and the footprint collapses to the
// conservative whole-render default. Pathological growth (many disjoint edits in one transaction)
// therefore degrades safely rather than growing unboundedly.
inline constexpr std::size_t kMaxAffectedTimeIntervals = 8;
// The cap on ordered geometry remaps in one transaction. Exceeding it clears the remaps and forces
// the conservative whole-render default, because a partially translated frame would mis-identify
// layers.
inline constexpr std::size_t kMaxLayerIdentityRemaps = 8;

// Validates each descriptor and requires every remap to name ONE composition; preserves their given
// order (order is what undo inversion depends on). Returns std::nullopt when a descriptor is
// malformed, when more than one composition is named, or when the cap is exceeded -- all of which
// the caller treats as whole-render with no remaps. An empty list normalizes to an empty list.
[[nodiscard]] std::optional<std::vector<LayerIdentityRemap>>
normalizeLayerIdentityRemaps(std::vector<LayerIdentityRemap> remaps);

// Undo inversion: reverses the ORDER and swaps before/after layer and node IDs. std::nullopt or
// empty input yields an empty list.
[[nodiscard]] std::vector<LayerIdentityRemap>
invertLayerIdentityRemaps(const std::optional<std::vector<LayerIdentityRemap>>& remaps);

// Sorts and merges overlapping/adjacent intervals in place. Returns std::nullopt when the interval
// count would exceed kMaxAffectedTimeIntervals, which the caller treats as the whole-render
// default.
[[nodiscard]] std::optional<AffectedTimeFootprint>
normalizeAffectedTimeFootprint(AffectedTimeFootprint footprint);

// Unions `next` into `accumulated`, both scoped to one composition. Returns std::nullopt when the
// two footprints name different compositions or the merged count exceeds the cap, either of which
// the caller treats as whole-render. A null `accumulated` adopts `next` normalized.
[[nodiscard]] std::optional<AffectedTimeFootprint>
mergeAffectedTimeFootprints(const std::optional<AffectedTimeFootprint>& accumulated,
                            const AffectedTimeFootprint& next);

struct OperationResult {
    OperationStatus status = OperationStatus::Applied;
    std::vector<OperationIssue> issues;
    std::vector<OperationOutput> outputs;
    // std::nullopt is the conservative default: this operation did not prove a finite footprint, so
    // its applied impact is treated as the whole render. An operation that can prove a finite
    // changed interval sets this to a normalized footprint. NoChange and Rejected operations never
    // advertise one.
    std::optional<AffectedTimeFootprint> affectedTimes;
    // SPLIT-1. A proven output-equivalent split additionally publishes ordered geometry remaps. It
    // is meaningful only alongside a finite (possibly deliberately empty) affectedTimes, and is
    // never published by NoChange, Rejected, stale, or failed results.
    std::optional<std::vector<LayerIdentityRemap>> layerIdentityRemaps;

    [[nodiscard]] static OperationResult applied(std::vector<OperationOutput> outputs = {});
    [[nodiscard]] static OperationResult noChange(std::vector<OperationOutput> outputs = {});
    [[nodiscard]] static OperationResult rejected(OperationIssueCode code, std::string message);
};

enum class CommandAction {
    Execute,
    Undo,
    Redo,
};

enum class CommandStatus {
    Succeeded,
    NoChange,
    Rejected,
    ValidationFailed,
    StaleRevision,
    ForeignDocument,
    DraftBaseMismatch,
    RevisionOverflow,
    NothingToUndo,
    NothingToRedo,
};

struct OperationFailure {
    std::size_t operationIndex = 0;
    std::string operationType;
    OperationIssue issue;

    friend bool operator==(const OperationFailure&, const OperationFailure&) = default;
};

struct CommandOutput {
    std::size_t operationIndex = 0;
    std::string operationType;
    OperationOutput output;

    friend bool operator==(const CommandOutput&, const CommandOutput&) = default;
};

struct CommandResult {
    CommandAction action = CommandAction::Execute;
    CommandStatus status = CommandStatus::NoChange;
    document::Revision beforeRevision;
    document::Revision afterRevision;
    std::optional<document::Revision> expectedRevision;
    std::string label;
    std::vector<OperationFailure> operationFailures;
    std::vector<CommandOutput> outputs;
    document::ValidationResult validation;
    // True when the applied transaction could change rendered pixels. Conservative default: a
    // rejected result, a no-change result, an unclassified operation, and every path that does not
    // aggregate operation effects stay true. Only a successful transaction whose every Applied
    // operation opted out (Operation::renderAffecting() == false) publishes false.
    bool renderAffecting = true;
    // The union of the applied operations' proven finite footprints, when every render-affecting
    // applied operation proved one for the same composition. std::nullopt is the conservative
    // whole-render default and is what an unclassified, mixed-composition, or no-pixel-change
    // transaction publishes; a present footprint is meaningful only when renderAffecting is true.
    // Rejected, failed, and stale results never carry applicable reuse evidence.
    std::optional<AffectedTimeFootprint> affectedTimes;
    // SPLIT-1. The transaction's ordered geometry remaps, present only when EVERY applied
    // render-affecting operation proved a finite footprint and every remap names the same single
    // composition within the cap. Undo publishes the inverted (reversed, before/after-swapped)
    // list; redo publishes the stored forward list.
    std::optional<std::vector<LayerIdentityRemap>> layerIdentityRemaps;

    [[nodiscard]] bool succeeded() const noexcept {
        return status == CommandStatus::Succeeded || status == CommandStatus::NoChange;
    }
    [[nodiscard]] bool changed() const noexcept { return status == CommandStatus::Succeeded; }

    template <core::TypedId IdType>
    [[nodiscard]] std::optional<IdType> outputId(std::string_view name,
                                                 std::size_t operationIndex = 0) const noexcept {
        for (const auto& item : outputs) {
            if (item.operationIndex == operationIndex && item.output.name == name) {
                if (const auto* id = std::get_if<IdType>(&item.output.id)) {
                    return *id;
                }
            }
        }
        return std::nullopt;
    }
};

} // namespace bloom::commands
