#pragma once

#include <bloom/commands/command_stack.hpp>
#include <bloom/core/color.hpp>
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/render/display_buffer.hpp>
#include <bloom/runtime/evaluation.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>

#include <QObject>
#include <QRectF>
#include <QString>

#include <optional>
#include <string_view>
#include <variant>

namespace bloom::commands {
class Transaction;
}

namespace bloom::document {
class Composition;
struct NodeRecord;
struct ParameterRecord;
struct Vec2d;
} // namespace bloom::document

namespace bloom::ui {

using SelectionTarget =
    std::variant<std::monostate, document::LayerId, document::NodeId, document::ParameterId>;

struct CompositionSelection {
    SelectionTarget primary;
    std::optional<document::LayerId> contextualLayer;

    friend bool operator==(const CompositionSelection&, const CompositionSelection&) = default;
};

// docs/architecture/animation-and-time.md, "Direct Manipulation And Preview Overrides": gesture
// begin freezes "a non-empty mapping rectangle, composition format, proxy, pixel aspect, and
// display descriptor for the current composition". The Viewer is the only owner of screen/display
// geometry, so it computes this snapshot and hands it to
// CompositionSession::beginPositionInteraction; the session freezes it into the session-only
// PositionInteraction and never recomputes it.
struct PositionInteractionMapping final {
    // The frozen fitted composition rectangle (already accounts for proxy scaling and pixel
    // aspect -- see fitDisplayRect() in viewer_editor.cpp).
    QRectF displayRect;
    // The frozen composition format; its width/height are the "compositionWidth"/
    // "compositionHeight" of the displacement formulas.
    document::CompositionFormat compositionFormat;
    // The frozen proxy factor, if any (today always CompositionFormatResolution{} -- no proxy
    // pipeline exists yet).
    runtime::EvaluationResolution resolution;
    // The frozen pixel aspect of the displayed frame.
    core::PixelAspectRatio pixelAspect;
    // The frozen display descriptor identity (extent, pixel aspect, and packed layout) used to
    // detect format/proxy/pixel-aspect/descriptor changes.
    render::ReferenceDisplayBufferDescriptor displayDescriptor;

    friend bool operator==(const PositionInteractionMapping&,
                           const PositionInteractionMapping&) = default;
};

// Typed rejection for CompositionSession::beginPositionInteraction (docs/architecture/
// animation-and-time.md, "Direct Manipulation And Preview Overrides").
enum class PositionInteractionRejection : std::uint8_t {
    // No layer is the session's primary selection.
    NoLayerSelected,
    // The selected layer exposes no position parameter, or its constant value does not match the
    // Vec2d schema.
    NoResolvablePosition,
    // The position parameter is bound to a driver; a gesture never disconnects a driven parameter
    // silently, so it never begins on one.
    DrivenParameter,
    // The position parameter is animated but has no exact-time key at the current session time.
    // CompositionSession has no synchronous access to runtime's exact rational curve sampling
    // (src/runtime owns sampling; src/ui does not get a private evaluator), so it cannot resolve
    // an interpolated base value without guessing. Refusing to begin is the honest v1 behavior;
    // dragging is supported for animated parameters exactly when the playhead sits on a key.
    AnimatedWithoutExactKey,
    // The supplied mapping has an empty/degenerate display rectangle.
    EmptyMapping,
};

class CompositionSession final : public QObject {
    Q_OBJECT

  public:
    CompositionSession(document::Document& document, commands::CommandStack& commandStack,
                       document::CompositionId compositionId, QObject* parent = nullptr);

    [[nodiscard]] const document::Snapshot& snapshot() const noexcept;
    [[nodiscard]] document::CompositionId compositionId() const noexcept;
    [[nodiscard]] const document::Composition* composition() const noexcept;
    [[nodiscard]] const CompositionSelection& selection() const noexcept;
    [[nodiscard]] core::RationalTime currentTime() const noexcept;

    [[nodiscard]] bool setComposition(document::CompositionId compositionId);
    [[nodiscard]] bool setCurrentTime(core::RationalTime time);
    void clearSelection();
    void selectLayer(document::LayerId layerId);
    void selectNode(document::NodeId nodeId);
    void selectParameter(document::ParameterId parameterId);

    [[nodiscard]] const document::NodeRecord* selectedNode() const noexcept;
    [[nodiscard]] std::optional<document::LayerId> layerForNode(document::NodeId nodeId) const;
    [[nodiscard]] std::optional<document::NodeId>
    boundaryNodeForLayer(document::LayerId layerId) const noexcept;
    [[nodiscard]] std::optional<document::NodeId>
    directSourceNodeForLayer(document::LayerId layerId) const noexcept;
    [[nodiscard]] const document::ParameterRecord*
    parameterForSelection(std::string_view role) const noexcept;
    [[nodiscard]] std::optional<double> constantValue(document::ParameterId parameterId) const;
    [[nodiscard]] std::optional<document::Vec2d>
    constantVec2Value(document::ParameterId parameterId) const;
    [[nodiscard]] std::optional<core::Color4d>
    constantColorValue(document::ParameterId parameterId) const;

    [[nodiscard]] bool addSolidLayer(const QString& name, core::Color4d color);
    [[nodiscard]] bool addTextLayer(const QString& name, const QString& text);
    [[nodiscard]] bool setSelectedPosition(double x, double y);
    [[nodiscard]] bool setSelectedOpacity(double opacity);
    [[nodiscard]] bool
    moveLayerBefore(document::LayerSlotId slotId,
                    std::optional<document::LayerSlotId> beforeSlotId = std::nullopt);

    // Direct viewer manipulation of the selected layer's position (docs/architecture/
    // animation-and-time.md, "Direct Manipulation And Preview Overrides"; issue #82). Session-only,
    // never persisted or undoable by itself: exactly one command transaction lands on
    // commitPositionInteraction(). The Viewer owns the whole gesture -- press, move, release,
    // Escape, and invalidation -- and is the only caller.
    [[nodiscard]] bool positionInteractionActive() const noexcept;
    // Sourced fresh by CompositionPreviewController's request path on every Interactive request
    // build while an interaction is armed; never cached across requests.
    [[nodiscard]] std::optional<runtime::SnapshotParameterOverride>
    positionInteractionOverride() const;
    // Validates the selected layer, a resolvable position parameter, and a non-empty mapping;
    // freezes `mapping` and the base value/revision/time. Returns the typed rejection, or
    // std::nullopt on success.
    [[nodiscard]] std::optional<PositionInteractionRejection>
    beginPositionInteraction(PositionInteractionMapping mapping);
    // Recomputes the override from the frozen base value plus the TOTAL gesture displacement
    // (never a chain of already-rounded intermediates). No-op if no interaction is active.
    void updatePositionInteraction(double screenDx, double screenDy);
    // Clears interaction state; creates no command. No-op if no interaction is active.
    void cancelPositionInteraction();
    // Called by the Viewer on resize/DPI/format/proxy/pixel-aspect/display-descriptor changes it
    // detects; today identical to cancelPositionInteraction() (docs/architecture/
    // animation-and-time.md: both "clear[ ] the override and create[ ] no command").
    void invalidatePositionInteraction();
    // Executes exactly one transaction through the same command surface as setSelectedPosition()
    // (SetParameterSource for a constant source, SetKeyframeAtTime at the frozen time for an
    // animated source), targeted at the frozen parameter/time rather than live selection. A stale
    // base revision or a zero-displacement move commits nothing. Always clears interaction state.
    [[nodiscard]] bool commitPositionInteraction();

    [[nodiscard]] bool canUndo() const noexcept;
    [[nodiscard]] bool canRedo() const noexcept;
    [[nodiscard]] QString undoLabel() const;
    [[nodiscard]] QString redoLabel() const;
    [[nodiscard]] bool undo();
    [[nodiscard]] bool redo();

    // Projection rebinding (task U1, issue #72): atomically swaps which document/command-stack
    // this session projects, for use after ProjectHost replaces the live ProjectSession content
    // (New/Open). Implements the UI-owned half of docs/architecture/project-session.md's "Session
    // Publication" install list: clears selection/interaction state, resets current time to zero,
    // selects `compositionId` (the caller's already-computed lowest valid CompositionId, or an
    // invalid/default id when no composition exists -- see ProjectHost), and emits every existing
    // changed signal so observers (preview controller, editors) see one coherent transition. The
    // OLD document/command-stack are left completely untouched by this call.
    void rebind(document::Document& document, commands::CommandStack& commandStack,
                document::CompositionId compositionId);

  signals:
    void snapshotChanged();
    void compositionChanged();
    void currentTimeChanged();
    void selectionChanged();
    void historyChanged();
    void commandRejected(const QString& message);
    // Emitted on begin/update/cancel/commit of a position interaction (docs/architecture/
    // animation-and-time.md, "Direct Manipulation And Preview Overrides").
    // CompositionPreviewController consumes it to (re)build a preview request carrying the fresh
    // override.
    void positionInteractionChanged();

  private:
    [[nodiscard]] bool execute(commands::Transaction&& transaction);
    [[nodiscard]] bool handleResult(const commands::CommandResult& result);
    [[nodiscard]] const document::NodeRecord*
    nodeForSelection(const CompositionSelection& selection) const noexcept;
    [[nodiscard]] const document::ParameterRecord*
    parameterForNode(const document::NodeRecord& node, std::string_view role) const noexcept;
    [[nodiscard]] bool setSelectionScalarParameter(std::string_view role, double value,
                                                   const QString& commandLabel);
    // The one command-selection decision for writing a position value (constant source ->
    // SetParameterSource; animation source -> SetKeyframeAtTime at `time`; driver source ->
    // rejected), executed as exactly one transaction. setSelectedPosition() calls this with its
    // live-derived parameter/time; commitPositionInteraction() calls it with the FROZEN
    // parameter/time -- a single copy of the branch so the two callers cannot silently drift if the
    // command surface ever changes.
    [[nodiscard]] bool executePositionCommand(document::ParameterId parameterId,
                                              core::RationalTime time, document::Vec2d value,
                                              const QString& commandLabel);
    [[nodiscard]] bool selectionExists(const CompositionSelection& selection) const noexcept;
    void normalizeSelection();
    void reportUnavailable(const QString& message);
    // Cancels an active position interaction whose frozen base revision no longer matches
    // snapshot_ (docs/architecture/animation-and-time.md: "snapshot changes that break the frozen
    // revision also cancel"). Called after handleResult() adopts a new snapshot.
    void invalidatePositionInteractionOnStaleRevision();

    // Session-only, never persisted (docs/architecture/animation-and-time.md, "Direct
    // Manipulation And Preview Overrides"). Named after exactly what the contract freezes.
    struct PositionInteraction final {
        document::Revision baseRevision;
        document::ParameterId parameterId;
        document::LayerId layerId;
        core::RationalTime time;
        document::Vec2d baseValue;
        document::Vec2d currentOverride;
        PositionInteractionMapping mapping;
    };

    // Pointers, not references (task U1, issue #72): rebind() must be able to atomically retarget
    // which document/command-stack this session projects after ProjectHost replaces the live
    // ProjectSession content. A reference member cannot be reseated; both are set at construction
    // and by rebind(), and are never null while this object is alive.
    document::Document* document_;
    commands::CommandStack* commandStack_;
    document::Snapshot snapshot_;
    document::CompositionId compositionId_;
    core::RationalTime currentTime_ = core::RationalTime::fromInteger(0);
    CompositionSelection selection_;
    std::optional<PositionInteraction> positionInteraction_;
};

} // namespace bloom::ui
