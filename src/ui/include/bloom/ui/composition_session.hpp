#pragma once

#include <bloom/commands/command_stack.hpp>
#include <bloom/core/blend_mode.hpp>
#include <bloom/core/color.hpp>
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/render/display_buffer.hpp>
#include <bloom/runtime/evaluation.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>

#include <QObject>
#include <QRectF>
#include <QString>

#include <bloom/commands/result.hpp>

#include <optional>
#include <set>
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

// A selected keyframe (issue #84; docs/architecture/animation-and-time.md: "Keyframe selection
// stores the stable KeyframeId; row index and screen position are presentation details"). curveId
// is included because it, together with keyframeId, is exactly what DeleteKeyframe/
// UpdateScalarKeyframe/UpdateVec2Keyframe are keyed by (src/commands/include/bloom/commands/
// animation_operations.hpp) -- no command needs the owning ParameterId. The owning ParameterId (and
// from it, the contextual layer) is instead derived on demand from curveId wherever something
// genuinely needs it (CompositionSession::selectKeyframe() does this once, to populate
// contextualLayer, the same way selectParameter() already derives it rather than caching it), so it
// is deliberately NOT stored here.
struct KeyframeSelection final {
    document::AnimationCurveId curveId;
    document::KeyframeId keyframeId;

    friend bool operator==(const KeyframeSelection&, const KeyframeSelection&) = default;
};

using SelectionTarget = std::variant<std::monostate, document::LayerId, document::NodeId,
                                     document::ParameterId, KeyframeSelection>;

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
    // The frozen composition display rectangle (already accounts for proxy scaling and pixel
    // aspect). Before task U3 (issue #119) this was always fitDisplayRect()'s fit-to-window
    // rectangle; ViewerEditor now derives it from the Viewer's own active zoom/pan ViewTransform at
    // gesture begin (viewTransformedDisplayRect() in viewer_editor.cpp) -- fitDisplayRect() exactly
    // when the transform is in Fit mode, or the actively zoomed/panned rectangle otherwise. The
    // freeze contract here is unchanged: this struct still doesn't know or care which geometry
    // source produced the rectangle, only that it was non-empty and is now frozen.
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
//
// issue #86 (task E1) removed the former AnimatedWithoutExactKey rejection: the contract never
// forbade beginning a gesture on an animated base without an exact key at the current time -- D1's
// refusal was an implementation gap (CompositionSession had no synchronous access to runtime's
// exact rational curve sampling). sampleParameterValue() now closes that gap by compiling the
// parameter's curve (runtime::compileAnimationCurve()) and sampling it (runtime::
// sampleAnimationCurve()) synchronously, so an animated position with no exact key at the current
// time begins from the exact interpolated base instead of refusing. No other code referenced the
// removed enumerator (verified: composition_session.cpp/.hpp and its own tests were the only
// occurrences), so it was deleted rather than kept as a documented-unreachable value.
enum class PositionInteractionRejection : std::uint8_t {
    // No layer is the session's primary selection.
    NoLayerSelected,
    // The selected layer exposes no position parameter, its constant value does not match the
    // Vec2d schema, or its animated curve fails to resolve/sample at the current session time.
    NoResolvablePosition,
    // The position parameter is bound to a driver; a gesture never disconnects a driven parameter
    // silently, so it never begins on one.
    DrivenParameter,
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
    [[nodiscard]] const std::set<document::NodeId>& selectedNodes() const noexcept {
        return selectedNodes_;
    }
    void selectNodes(std::set<document::NodeId> nodes, document::NodeId primary);
    void toggleNodeSelection(document::NodeId nodeId);
    void selectParameter(document::ParameterId parameterId);
    // Selecting a keyframe REPLACES the primary selection like every other select* method (one
    // primary/contextual selection truth -- docs/roadmap.md's Batch-4 gate). A missing curve/key
    // reports unavailable and leaves the selection untouched, mirroring selectLayer/selectNode/
    // selectParameter's own not-found handling.
    void selectKeyframe(document::AnimationCurveId curveId, document::KeyframeId keyframeId);

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
    // The String-valued counterpart of the three readers above (the text content schema). Returns
    // nullopt for a missing, non-constant, or wrong-typed parameter, exactly like they do.
    [[nodiscard]] std::optional<QString>
    constantStringValue(document::ParameterId parameterId) const;
    // The blend mode a layer is authored with, or nullopt when the layer has no resolvable Layer
    // Output blend-mode parameter (no such layer, a non-constant source, or an integer the closed
    // mapping does not name). Every surface that shows the mode reads it through this one method, so
    // the timeline row, the Properties row, and the node card cannot disagree about what a layer is
    // set to.
    [[nodiscard]] std::optional<core::BlendMode>
    blendModeForLayer(document::LayerId layerId) const noexcept;

    [[nodiscard]] bool addSolidLayer(const QString& name, core::Color4d color);
    // `size` is the em size in pixels and `color` a straight reference-linear-sRGB authoring value;
    // both default to the registered text schema's own defaults.
    [[nodiscard]] bool addTextLayer(const QString& name, const QString& text,
                                    double size = document::kDefaultTextSizePixels,
                                    core::Color4d color = core::Color4d{1.0, 1.0, 1.0, 1.0});
    [[nodiscard]] bool setSelectedPosition(double x, double y);
    // The rest of the Layer Output transform. Anchor and scale are authored exactly as position is
    // (full-resolution composition pixels for the anchor, a unitless factor for the scale),
    // rotation in degrees clockwise on screen, and each is one undoable command that writes a
    // constant or a keyframe at the session time depending on the parameter's source.
    [[nodiscard]] bool setSelectedAnchor(double x, double y);
    [[nodiscard]] bool setSelectedScale(double x, double y);
    [[nodiscard]] bool setSelectedRotation(double degrees);
    [[nodiscard]] bool setSelectedOpacity(double opacity);
    // Task P3 (issue #120 follow-up, owner review 2026-09-12): the properties panel's editable
    // RGBA cells write through this, exactly mirroring setSelectedOpacity()'s shape -- resolve the
    // selection's document::kSolidColorParameterRole parameter, then a single
    // commands::SetParameterSource carrying the whole core::Color4d as its ConstantValueSource
    // (the same generic command Position/Opacity already use for their own constant branch), one
    // transaction per call. Unlike Position, there is no animated branch:
    // CreateAnimationForParameter accepts only the animatable transform and opacity schemas
    // (src/commands/animation_operations.cpp) and commands::SetKeyframeAtTime has no core::Color4d
    // overload, so a solid color parameter can never actually become an AnimationCurveSource
    // through the existing command surface -- a non-constant source (driven, or the
    // otherwise-unreachable animated case) is refused the same way setSelectionScalarParameter()'s
    // driven branch is. See this task's raw report.
    [[nodiscard]] bool setSelectedSolidColor(core::Color4d color);
    // Task S3: the three text parameters, written through exactly the paths their solid/opacity
    // counterparts already use -- one commands::SetParameterSource per call, one transaction, one
    // undo step. Content goes through its own method rather than setSelectionScalarParameter()
    // because the value is a string; size goes straight through that shared scalar helper; color
    // shares setSelectionColorParameter() with setSelectedSolidColor(), which is why a text color
    // and a solid color cannot drift apart. Each is a no-op returning false (with the usual
    // commandRejected() message) when the selection exposes no such parameter, and a committing
    // no-op returning true when the value is already what was asked for.
    [[nodiscard]] bool setSelectedTextContent(const QString& content);
    [[nodiscard]] bool setSelectedTextSize(double size);
    [[nodiscard]] bool setSelectedTextColor(core::Color4d color);
    // The blend mode, by explicit LayerId. This is the primitive the timeline row needs: a row knows
    // which layer it draws and must not have to move the selection to change that layer's blending.
    // One commands::SetParameterSource carrying the mode's stored integer, one transaction, one undo
    // step -- exactly the shape setSelectedSolidColor() uses, and for the same reason: the schema is
    // constant-only, so there is no keyframe branch. A mode already equal to the one asked for
    // commits nothing and returns true.
    [[nodiscard]] bool setLayerBlendMode(document::LayerId layerId, core::BlendMode mode);
    // The selection-driven form, for the Properties row and the Layer node card, which author
    // whatever the contextual layer is. Resolves the selection's layer and delegates to
    // setLayerBlendMode(), so there is exactly one write path.
    [[nodiscard]] bool setSelectedBlendMode(core::BlendMode mode);
    [[nodiscard]] bool
    moveLayerBefore(document::LayerSlotId slotId,
                    std::optional<document::LayerSlotId> beforeSlotId = std::nullopt);

    // Keyframe delete/move gestures (issue #84; docs/architecture/animation-and-time.md). Command
    // construction lives here, not in the widget -- the same "one place" precedent as
    // executePositionCommand(). Both are a no-op false with no transaction and the selection intact
    // when nothing is selected or the command layer refuses (e.g. DeleteKeyframe's final-key
    // refusal, UpdateScalarKeyframe/UpdateVec2Keyframe's duplicate-time refusal).
    [[nodiscard]] bool deleteSelectedKeyframe();
    // Reads the selected key's EXISTING value/interpolation from the current snapshot and passes
    // them unchanged with newTime (scalar vs Vec2d branch resolved once, here). A newTime exactly
    // equal to the key's current exact time commits nothing and returns true (docs/architecture/
    // animation-and-time.md's zero-move precedent, mirrored from commitPositionInteraction()).
    [[nodiscard]] bool moveSelectedKeyframe(core::RationalTime newTime);
    // Timeline insert gesture (issue #86, task E1): double-clicking a keyframe lane's row
    // BACKGROUND (never an existing key -- that hit-testing is the widget's job, same tolerance as
    // the click-select/drag gestures) inserts a new key at the exact frame-snapped `time`, valued
    // at the curve's own exactly sampled value at that time (sampleParameterValue(), via decision
    // 1's compileAnimationCurve() + the existing sampleAnimationCurve()). Uses
    // InsertScalarKeyframe/ InsertVec2Keyframe -- their default outgoing interpolation is Linear,
    // matching exactly what SetKeyframeAtTime already does for a newly inserted key
    // (src/commands/animation_operations.cpp) rather than inventing new policy. An occupied exact
    // time is the commands layer's existing refusal (InvalidOrder): no transaction, selection
    // untouched. On success the new key becomes the selected keyframe (K1's one-truth
    // selectKeyframe() swap), in exactly one transaction.
    [[nodiscard]] bool insertKeyframeAtTime(document::AnimationCurveId curveId,
                                            core::RationalTime time);

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

    // Node authoring (task N3): the ONE public submission seam for node-operation transactions
    // built by the node editor (MoveNodes, ConnectPorts, DuplicateNodes, ...). Executes exactly
    // one transaction through the same command stack and publication path as every other session
    // edit (snapshot/selection normalization/history/rejection signals are identical), and returns
    // the full command result so the caller can read the ids the transaction created. Refusals
    // surface through commandRejected() exactly as they do for setSelectedPosition().
    [[nodiscard]] commands::CommandResult
    executeNodeTransaction(commands::Transaction&& transaction);

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
    // The Vec2d counterpart of setSelectionScalarParameter(), shared by every Vec2d transform row.
    // It routes through executePositionCommand() deliberately: the constant/keyframe/driven
    // decision is identical for position, anchor, and scale, so one write path serves all three.
    [[nodiscard]] bool setSelectionVec2Parameter(std::string_view role, double x, double y,
                                                 const QString& commandLabel);
    // The one command-selection decision for writing a Color4d-valued parameter, shared by
    // setSelectedSolidColor() and setSelectedTextColor(). A driven source is refused exactly as the
    // scalar helper refuses one; there is no animated branch, because no command in the surface can
    // put a color parameter on a curve (CreateAnimationForParameter accepts only the animatable
    // transform and opacity schemas, and SetKeyframeAtTime has no Color4d overload).
    [[nodiscard]] bool setSelectionColorParameter(std::string_view role, core::Color4d color,
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
    // The one session-level exact-sampling helper (issue #86, task E1; docs/architecture/
    // animation-and-time.md): for an animation-sourced parameter, compiles its curve
    // (runtime::compileAnimationCurve(), decision 1's extracted pure conversion) and samples it at
    // `time` via the existing runtime::sampleAnimationCurve() -- the SAME exact rational sampler
    // src/runtime uses everywhere else. Returns std::nullopt for a constant/driven source (those
    // callers keep their existing paths -- constantValue()/constantVec2Value(), the driven refusal
    // branches) or if the curve fails to resolve/sample. Both beginPositionInteraction() (the
    // relaxed animated-base rule) and insertKeyframeAtTime() (the timeline insert gesture) call
    // this one place rather than each re-deriving the curve/sample logic.
    [[nodiscard]] std::optional<std::variant<double, document::Vec2d>>
    sampleParameterValue(const document::ParameterRecord& parameter, core::RationalTime time) const;
    // Not noexcept (see keyframeSelectionExists()): the KeyframeSelection branch delegates to it.
    [[nodiscard]] bool selectionExists(const CompositionSelection& selection) const;
    // Not noexcept: std::visit over the AnimationCurveStore's curve-kind variant cannot be proven
    // exception-free by clang-tidy's bugprone-exception-escape (the variant is never valueless in
    // practice, but std::visit's contract still permits bad_variant_access).
    [[nodiscard]] bool keyframeSelectionExists(const KeyframeSelection& selection) const;
    [[nodiscard]] std::optional<document::ParameterId>
    parameterForCurve(document::AnimationCurveId curveId) const noexcept;
    [[nodiscard]] std::optional<document::LayerId>
    contextualLayerForParameter(document::ParameterId parameterId) const;
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
    std::set<document::NodeId> selectedNodes_;
    std::optional<PositionInteraction> positionInteraction_;
};

} // namespace bloom::ui
