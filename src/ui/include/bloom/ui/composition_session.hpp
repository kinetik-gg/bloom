#pragma once
#include <bloom/commands/operations.hpp>
#include <bloom/document/shape.hpp>

#include <bloom/commands/animation_operations.hpp>
#include <bloom/commands/command_stack.hpp>
#include <bloom/core/blend_mode.hpp>
#include <bloom/core/color.hpp>
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/animation.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/render/display_buffer.hpp>
#include <bloom/runtime/evaluation.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/ui/kit/color.hpp>
#include <bloom/ui/viewer_overlays.hpp>

#include <QObject>
#include <QRectF>
#include <QString>
#include <QTransform>

#include <bloom/commands/result.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

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
class AssetController;

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
    std::optional<document::AnimationComponent> component{};

    friend bool operator==(const KeyframeSelection&, const KeyframeSelection&) = default;
};

using SelectionTarget = std::variant<std::monostate, document::LayerId, document::NodeId,
                                     document::ParameterId, KeyframeSelection>;

struct CompositionSelection {
    SelectionTarget primary;
    std::optional<document::LayerId> contextualLayer;
    std::vector<KeyframeSelection> keyframes{};

    friend bool operator==(const CompositionSelection&, const CompositionSelection&) = default;
};

struct TransformGesture final {
    enum class Kind : std::uint8_t { Move, Scale, Rotate, Anchor };
    Kind kind = Kind::Move;
    int handle = 0;
    QPointF origin{};
    // Frozen evaluated local bounds and world polygon, from the current preview frame.
    std::optional<runtime::EvaluatedOperationBounds> bounds{};
};

struct TransformModifiers final {
    bool shift = false;
    bool alt = false;
};

enum class TransformInteractionRejection : std::uint8_t {
    NoLayerSelected,
    LockedLayer,
    NoResolvableTransform,
    DrivenParameter,
    EmptyMapping,
    SingularTransform,
};

// One parameter's value, whatever kind it is: the three animatable value kinds a curve can carry
// and a constant can hold. Named once so sampleParameterValue() and the effective*Value() readers
// cannot drift apart about what a "parameter value" is in this layer.
// Task DRIVE-1 added the three kinds that cannot interpolate -- a String, an Integer (every
// enum-backed one included) and a Boolean. They have no curve and never will, but they DO have a
// value: the constant the store holds, or, once a driver binding is on them, whatever the graph
// produces. "What is this parameter's value" is one question for all seven kinds, so it has one
// answer type rather than four readers and three special cases.
using ParameterSample = std::variant<double, document::Vec2d, document::Vec3d, core::Color4d,
                                     std::string, std::int64_t, bool>;

// Task DRIVE-1. How far an upstream walk follows, and one node it reached.
//
// DriverLinksOnly is the walk the TIMELINE makes from a layer: a layer's twirl-down already lists
// the layer's own parameters and its direct source's, so what it is missing is exactly the value
// nodes those parameters are DRIVEN by, and nothing about the image chain behind them. A node with
// no parameters at all -- a Reroute's pass-through, the one socket a driver cannot land on -- still
// follows its input edge, because otherwise a reroute would hide the node behind it.
//
// DriverLinksAndInputEdges is the walk PROPERTIES makes from the selected node: everything that
// node depends on, whether the dependency is a wire carrying pixels or a driver carrying a number.
// Either walk stops at a Layer Stack and at the Composition Output, because everything upstream of
// those two is the whole composition rather than this node's own inputs.
enum class UpstreamTraversal : std::uint8_t { DriverLinksOnly, DriverLinksAndInputEdges };

struct UpstreamNode final {
    document::NodeId id;
    // Edges away from the nearest seed. Breadth-first, so this never decreases along the result.
    int depth = 0;
    friend bool operator==(const UpstreamNode&, const UpstreamNode&) = default;
};

// What a keyframe diamond shows for one parameter at the session's current time (task S5, item 0;
// docs/architecture/animation-and-time.md, "The Keyframe Gesture"). The three authored states are
// exactly the three AE diamond shapes -- empty, outlined, filled -- and Unsupported is the fourth,
// honest case: the selection exposes no such parameter, or its schema declares no animation at all,
// so there is no diamond to click.
enum class KeyframeDiamondState : std::uint8_t {
    Unsupported,
    Constant,
    AnimatedWithoutKey,
    AnimatedWithKey,
};

enum class KeyframeParameterState : std::uint8_t {
    Unsupported,
    None,
    Some,
    All,
};

// Which object a value write authors (task FIX2). Empty means the CURRENT SELECTION -- what the
// Properties panel and the timeline row mean when they write. A node id names THAT node's own
// parameter instead, which is what a node CARD means: a card authors the card it is drawn on, and
// selecting itself first (as it used to, so a selection-driven write would find it) collapsed every
// other selected card and moved the panel off whatever the artist was looking at. Both forms travel
// the same function bodies below, so the two surfaces cannot drift on units, domains, keyframe
// branching, or refusal wording.
using AuthoringTarget = std::optional<document::NodeId>;

struct SessionColorConverterState;

class CompositionSession final : public QObject {
    Q_OBJECT

  public:
    // Every Color4d-valued schema (document::isColor4AnimatableSchemaKey()) plus the colour operand
    // schema declares reference linear sRGB. An unrecognized schema key fails closed.
    [[nodiscard]] kit::KColorConverter
    colorConverter(std::string_view schemaKey = document::kSolidColorParameterSchemaKey);

    void setAssetController(AssetController* controller) noexcept { assetController_ = controller; }
    [[nodiscard]] AssetController* assetController() const noexcept { return assetController_; }

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
    void selectLayer(document::LayerId layerId, bool extend = false);
    [[nodiscard]] const document::LayerStack* timelineMerge() const noexcept;
    void selectNode(document::NodeId nodeId);
    [[nodiscard]] document::WorkArea workArea() const noexcept;
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
    void selectKeyframe(document::AnimationCurveId curveId, document::KeyframeId keyframeId,
                        bool extend = false);
    void selectKeyframe(document::AnimationCurveId curveId, document::AnimationComponent component,
                        document::KeyframeId keyframeId, bool extend = false);
    void selectKeyframes(const std::vector<KeyframeSelection>& keys);
    [[nodiscard]] std::vector<commands::KeyframePaste> selectedKeyframeData() const;
    [[nodiscard]] bool moveKeyframes(std::vector<commands::KeyframeMove> keys,
                                     document::Revision revision);
    // A graph-editor key drag moves keys in TIME and in VALUE at once. Both halves belong to one
    // transaction and therefore one undo entry, because they are one gesture: an artist who drags a
    // key diagonally and then undoes expects the key back where it started, not half-way. An empty
    // list omits its operation rather than staging an empty batch, and a domain refusal on either
    // half commits nothing at all.
    [[nodiscard]] bool moveKeyframesAndValues(std::vector<commands::KeyframeMove> moves,
                                              std::vector<commands::KeyframeValueEdit> values,
                                              document::Revision revision);
    // The ease-handle drag. One transaction; the command layer forces the shaped segment's left key
    // to Ease In-Out and refuses the final key's outgoing and the first key's incoming handle.
    [[nodiscard]] bool setKeyframeHandles(std::vector<commands::KeyframeHandleEdit> edits,
                                          document::Revision revision);
    // "Reset Handles" over the current selection: every selected key's handles return to their
    // defaults, which ARE the canonical Ease In-Out shape -- so the reset also settles the affected
    // segments on that mode. A key that owns no segment at all (the only key of its curve) is
    // skipped rather than refused.
    [[nodiscard]] bool resetSelectedKeyframeHandles();
    [[nodiscard]] bool pasteKeyframes(const std::vector<commands::KeyframePaste>& keys,
                                      document::Revision revision);
    [[nodiscard]] bool deleteSelectedKeyframes();
    [[nodiscard]] bool
    setSelectedKeyframesInterpolation(document::KeyframeInterpolation interpolation);
    void copySelectedKeyframes();
    [[nodiscard]] bool pasteCopiedKeyframes();

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
    // mapping does not name). Every surface that shows the mode reads it through this one method,
    // so the timeline row, the Properties row, and the node card cannot disagree about what a layer
    // is set to.
    [[nodiscard]] std::optional<core::BlendMode>
    blendModeForLayer(document::LayerId layerId) const noexcept;

    [[nodiscard]] bool addShapeLayer(document::ShapeKind kind,
                                     commands::ShapeLayerGeometry geometry = {});
    [[nodiscard]] bool addSolidLayer(const QString& name, core::Color4d color);
    // `size` is the em size in pixels and `color` a straight reference-linear-sRGB authoring value;
    // both default to the registered text schema's own defaults.
    [[nodiscard]] bool addTextLayer(const QString& name, const QString& text,
                                    double size = document::kDefaultTextSizePixels,
                                    core::Color4d color = core::Color4d{1.0, 1.0, 1.0, 1.0},
                                    std::optional<document::Vec2d> position = {});
    [[nodiscard]] bool setSelectedPosition(double x, double y, AuthoringTarget target = {});
    // The rest of the Layer Output transform. Anchor and scale are authored exactly as position is
    // (full-resolution composition pixels for the anchor, a unitless factor for the scale),
    // rotation in degrees clockwise on screen, and each is one undoable command that writes a
    // constant or a keyframe at the session time depending on the parameter's source.
    [[nodiscard]] bool setSelectedAnchor(double x, double y, AuthoringTarget target = {});
    [[nodiscard]] bool setSelectedScale(double x, double y, AuthoringTarget target = {});
    [[nodiscard]] bool setSelectedRotation(double degrees, AuthoringTarget target = {});
    [[nodiscard]] bool setSelectedOpacity(double opacity, AuthoringTarget target = {});
    // The properties panel's editable RGBA cells write through this, exactly mirroring
    // setSelectedOpacity()'s shape -- resolve the selection's document::kSolidColorParameterRole
    // parameter, then one transaction. Task S5 gave it Position's full branch set: a constant
    // source takes commands::SetParameterSource, an ANIMATED one takes commands::SetKeyframeAtTime
    // at the session time (a colour parameter can be animated now), and a driven one is refused the
    // way setSelectionScalarParameter()'s driven branch is.
    [[nodiscard]] bool setSelectedSolidColor(core::Color4d color, AuthoringTarget target = {});
    // Task S3: the three text parameters, written through exactly the paths their solid/opacity
    // counterparts already use -- one commands::SetParameterSource per call, one transaction, one
    // undo step. Content goes through its own method rather than setSelectionScalarParameter()
    // because the value is a string; size goes straight through that shared scalar helper; color
    // shares setSelectionColorParameter() with setSelectedSolidColor(), which is why a text color
    // and a solid color cannot drift apart. Each is a no-op returning false (with the usual
    // commandRejected() message) when the selection exposes no such parameter, and a committing
    // no-op returning true when the value is already what was asked for.
    [[nodiscard]] bool setSelectedTextContent(const QString& content, AuthoringTarget target = {});
    [[nodiscard]] bool setSelectedTextSize(double size, AuthoringTarget target = {});
    [[nodiscard]] bool setSelectedTextColor(core::Color4d color, AuthoringTarget target = {});
    // The blend mode, by explicit LayerId. This is the primitive the timeline row needs: a row
    // knows which layer it draws and must not have to move the selection to change that layer's
    // blending. One commands::SetParameterSource carrying the mode's stored integer, one
    // transaction, one undo step -- exactly the shape setSelectedSolidColor() uses, and for the
    // same reason: the schema is constant-only, so there is no keyframe branch. A mode already
    // equal to the one asked for commits nothing and returns true.
    [[nodiscard]] std::optional<document::LayerId> parentOf(document::LayerId layer) const;
    [[nodiscard]] std::vector<document::LayerId> childrenOf(document::LayerId layer) const;
    [[nodiscard]] std::vector<document::LayerId> candidateParents(document::LayerId layer) const;
    [[nodiscard]] bool setLayerParent(document::LayerId layer,
                                      std::optional<document::LayerId> parent);
    [[nodiscard]] bool setLayerBlendMode(document::LayerId layerId, core::BlendMode mode);
    // The selection-driven form, for the Properties row and the Layer node card, which author
    // whatever the contextual layer is. Resolves the selection's layer and delegates to
    // setLayerBlendMode(), so there is exactly one write path.
    [[nodiscard]] bool setSelectedBlendMode(core::BlendMode mode);
    [[nodiscard]] bool
    moveLayerBefore(document::LayerSlotId slotId,
                    std::optional<document::LayerSlotId> beforeSlotId = std::nullopt);

    // --- The keyframe gesture (task S5, item 0) ------------------------------------------------
    // What the diamond for `role` on the CURRENT selection should paint, at the current session
    // time. Pure projection: no command, no mutation, nothing cached.
    [[nodiscard]] KeyframeDiamondState keyframeDiamondState(std::string_view role) const;
    [[nodiscard]] KeyframeParameterState keyframeParameterState(std::string_view role) const;
    [[nodiscard]] KeyframeDiamondState
    keyframeDiamondState(std::string_view role, document::AnimationComponent component) const;
    [[nodiscard]] bool toggleKeyframe(std::string_view role,
                                      document::AnimationComponent component);
    // The same question keyed by PARAMETER rather than by the current selection's role. This is the
    // primitive; the role overload above resolves the role against the selection and delegates. The
    // node canvas needs this one: a card paints the diamond for ITS OWN node's parameter, which is
    // not necessarily the selected one, and reading it must never move the selection.
    [[nodiscard]] KeyframeDiamondState
    keyframeDiamondStateForParameter(document::ParameterId parameterId) const;
    [[nodiscard]] KeyframeDiamondState keyframeDiamondState(document::ParameterId parameterId,
                                                            document::AnimationComponent component,
                                                            core::RationalTime time) const;
    [[nodiscard]] KeyframeParameterState keyframeParameterState(document::ParameterId parameterId,
                                                                core::RationalTime time) const;
    // The AE diamond click, end to end, as exactly ONE undoable transaction per call:
    //   * a constant parameter becomes a curve with one key at the current value and time
    //     (CreateAnimationForParameter + SetKeyframeAtTimeForParameter together -- the second
    //     operation is parameter-keyed precisely so it can see the curve the first one created);
    //   * an animated parameter with no key at the current time gains one, valued at the curve's
    //   own
    //     exactly sampled value there, so inserting a key never moves the picture;
    //   * an animated parameter WITH a key at the current time loses it;
    //   * losing the LAST key converts the parameter back to a constant holding that key's value.
    // Refused (no transaction, message through commandRejected()) when the selection exposes no
    // such parameter, the schema declares no animation, or the source is driven.
    [[nodiscard]] bool toggleKeyframe(std::string_view role);
    // The gesture keyed by PARAMETER. Also the primitive: the role overload above is exactly this
    // one after resolving the role against the current selection. A card's diamond calls this
    // directly, so clicking it neither requires nor causes a selection change -- unlike the card's
    // VALUE fields, which write through the selection-based setSelected*() methods and therefore
    // still select their own node first.
    [[nodiscard]] bool toggleKeyframeForParameter(document::ParameterId parameterId);
    [[nodiscard]] bool toggleKeyframe(document::ParameterId parameterId,
                                      document::AnimationComponent component,
                                      core::RationalTime time);
    // The selected key's outgoing interpolation (task S5, item 2). A no-op false with no
    // transaction when no keyframe is selected or the command layer refuses (notably the final key,
    // whose interpolation is canonical Linear).
    [[nodiscard]] bool
    setSelectedKeyframeInterpolation(document::KeyframeInterpolation interpolation);
    // The selected key's CURRENT outgoing interpolation, for a menu that has to show which choice
    // is active and which is unavailable. nullopt when no key is selected or it no longer resolves.
    [[nodiscard]] std::optional<document::KeyframeInterpolation>
    selectedKeyframeInterpolation() const;
    // Whether the selected key is its curve's FINAL key, which is the one key whose interpolation
    // is pinned to Linear -- so a menu can disable Hold/Ease rather than offer a refusal.
    [[nodiscard]] bool selectedKeyframeIsFinal() const;

    // The value a row or card field should DISPLAY for `role` at the current session time: the
    // constant for a constant source, or the curve's exactly sampled value for an animated one
    // (task S5, item 0 -- "editing a value of an animated parameter at a time with no key inserts a
    // key", which is only reachable if the field is live and shows the animated value in the first
    // place). nullopt for a missing parameter, a driven source, or a value that is not of the
    // requested kind.
    [[nodiscard]] std::optional<double> effectiveScalarValue(std::string_view role) const;
    [[nodiscard]] std::optional<document::Vec2d> effectiveVec2Value(std::string_view role) const;
    [[nodiscard]] std::optional<core::Color4d> effectiveColorValue(std::string_view role) const;
    // The PARAMETER-keyed counterparts, for the same reason keyframeDiamondStateForParameter()
    // exists: a node card reads its own node's parameters, not the selection's.
    [[nodiscard]] std::optional<double>
    effectiveScalarValue(document::ParameterId parameterId) const;
    [[nodiscard]] std::optional<document::Vec2d>
    effectiveVec2Value(document::ParameterId parameterId) const;
    [[nodiscard]] std::optional<core::Color4d>
    effectiveColorValue(document::ParameterId parameterId) const;
    // Task DRIVE-1. The same reader for the four kinds that had none. A Vec3d can be animated (it
    // has a curve table), so it is sampled exactly as the three above are; a String, an Integer and
    // a Boolean never interpolate, so for them this is the constant the store holds. None of the
    // four answers for a DRIVEN parameter -- no reader here does, because a driven value is
    // produced by the value graph rather than read off the record -- which is what
    // drivenValueText() is for.
    [[nodiscard]] std::optional<document::Vec3d>
    effectiveVec3Value(document::ParameterId parameterId) const;
    [[nodiscard]] std::optional<QString>
    effectiveStringValue(document::ParameterId parameterId) const;
    [[nodiscard]] std::optional<std::int64_t>
    effectiveIntegerValue(document::ParameterId parameterId) const;
    [[nodiscard]] std::optional<bool>
    effectiveBooleanValue(document::ParameterId parameterId) const;

    // Task DRIVE-1. The driver-chain readers every surface that shows a driven parameter shares.
    // Properties grew all three inline first; the timeline needs the same three answers, and two
    // surfaces disagreeing about which node drives a parameter would be a bug nobody could see.
    //
    // The binding on one parameter, or nullptr when it carries none.
    [[nodiscard]] const document::DriverBindingSource*
    driverBindingFor(document::ParameterId parameterId) const noexcept;
    // The display name of the node driving `parameterId` -- the same name its node card carries --
    // or an empty string when the parameter is not driven at all. A binding naming a node the
    // composition no longer holds answers the honest "Missing driver" rather than an empty cell.
    [[nodiscard]] QString driverDisplayName(document::ParameterId parameterId) const;
    // Breadth-first from `seeds`, deduplicated, seeds themselves excluded, in the order reached.
    [[nodiscard]] std::vector<UpstreamNode> upstreamNodes(std::span<const document::NodeId> seeds,
                                                          UpstreamTraversal traversal) const;

    // Task DRIVE-1. The resolved text of one driven parameter at the session's current time, or an
    // empty string when nothing has been resolved for it yet. Resolving a driver means evaluating
    // the value graph, which is work for a worker thread rather than a paint, so this is a readback
    // of the newest finished evaluation and refreshDrivenValues() is the request for another.
    // ONE evaluation answers every surface: the Properties row and the timeline row for the same
    // parameter read the same string by construction.
    [[nodiscard]] QString drivenValueText(document::ParameterId parameterId) const;
    // Requests a fresh resolution of every driven parameter in the composition. Idempotent: a
    // request identical to the one already in flight or already answered -- same revision, same
    // time, same parameters -- starts nothing, so a surface may call it from the same refresh that
    // drivenValuesChanged() triggered without looping.
    void refreshDrivenValues();

    // Writes one authored value onto an EXACT parameter, by the same constant-or-keyframe rule
    // every layer row already follows (task FIX1, item G): a constant source is rewritten, an
    // animated one takes a key at the session time, and a driven one is refused with the same
    // wording. The node card's generic operand editor calls this, so editing an animated Scalar
    // node's number at a new time adds a key there instead of replacing its curve with a constant.
    [[nodiscard]] bool setParameterComponentValue(document::ParameterId parameterId,
                                                  document::AnimationComponent component,
                                                  double value);
    [[nodiscard]] bool setParameterValue(document::ParameterId parameterId,
                                         document::ParameterValue value,
                                         const QString& commandLabel);

    // Unfinished edits belong to the session. Matching setters update this live value until
    // commit; no command, revision or key is created by begin/update/cancel.
    [[nodiscard]] bool
    beginValueEdit(document::ParameterId parameterId,
                   std::optional<document::AnimationComponent> component = std::nullopt);
    [[nodiscard]] bool updateValueEdit(document::ParameterValue value);
    [[nodiscard]] bool commitValueEdit();
    void cancelValueEdit();
    [[nodiscard]] bool valueEditActive() const noexcept;
    [[nodiscard]] bool beginPathEdit(document::ParameterId parameter);
    [[nodiscard]] bool updatePathEdit(document::PathValue path, document::Vec2d centreDelta);
    [[nodiscard]] bool isValueEditing(document::ParameterId parameterId) const noexcept;
    [[nodiscard]] std::optional<document::ParameterValue>
    liveValue(document::ParameterId parameterId) const;
    [[nodiscard]] std::vector<runtime::SnapshotParameterOverride> valueEditOverrides() const;

    // Keyframe delete/move gestures (issue #84; docs/architecture/animation-and-time.md). Command
    // construction lives here, not in the widget -- the same "one place" precedent as
    // executePositionCommand(). Both are a no-op false with no transaction and the selection intact
    // when nothing is selected or the command layer refuses (e.g. DeleteKeyframe's final-key
    // refusal, UpdateScalarKeyframe/UpdateVec2Keyframe's duplicate-time refusal).
    [[nodiscard]] bool deleteSelectedKeyframe();
    // Reads the selected key's EXISTING value/interpolation from the current snapshot and passes
    // them unchanged with newTime (scalar vs Vec2d branch resolved once, here). A newTime exactly
    // equal to the key's current exact time commits nothing and returns true (docs/architecture/
    // animation-and-time.md's zero-move precedent, mirrored from commitTransformInteraction()).
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

    // One session-only gesture; all touched parameters commit atomically at the frozen time.
    [[nodiscard]] bool transformInteractionActive() const noexcept;
    [[nodiscard]] std::vector<runtime::SnapshotParameterOverride>
    transformInteractionOverrides() const;
    [[nodiscard]] std::optional<TransformInteractionRejection>
    beginTransformInteraction(TransformGesture gesture, ViewerMapping mapping,
                              TransformModifiers modifiers = {});
    void updateTransformInteraction(QPointF screenPoint, TransformModifiers modifiers = {});
    void cancelTransformInteraction();
    void invalidateTransformInteraction();
    [[nodiscard]] bool commitTransformInteraction();

    // Node authoring (task N3): the ONE public submission seam for node-operation transactions
    // built by the node editor (MoveNodes, ConnectPorts, DuplicateNodes, ...). Executes exactly
    // one transaction through the same command stack and publication path as every other session
    // edit (snapshot/selection normalization/history/rejection signals are identical), and returns
    // the full command result so the caller can read the ids the transaction created. Refusals
    // surface through commandRejected() exactly as they do for setSelectedPosition().
    [[nodiscard]] commands::CommandResult
    executeNodeTransaction(commands::Transaction&& transaction);
    // The shared public submission seam for editor-owned composition transactions. Like the node
    // submission path, this refreshes the projection and emits the ordinary history/rejection
    // signals after the command stack publishes.
    [[nodiscard]] commands::CommandResult executeTransaction(commands::Transaction&& transaction);

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
    void transformInteractionChanged();
    void liveValueChanged();
    // Task DRIVE-1: a fresh resolution of the composition's driven parameters has landed.
    void drivenValuesChanged();

  private:
    AssetController* assetController_ = nullptr;

    [[nodiscard]] bool execute(commands::Transaction&& transaction);
    [[nodiscard]] bool handleResult(const commands::CommandResult& result);
    [[nodiscard]] const document::NodeRecord*
    nodeForSelection(const CompositionSelection& selection) const noexcept;
    [[nodiscard]] const document::ParameterRecord*
    parameterForNode(const document::NodeRecord& node, std::string_view role) const noexcept;
    // The one resolution every value write goes through: the selection's parameter for `role`, or
    // the named node's own. See AuthoringTarget.
    [[nodiscard]] const document::ParameterRecord*
    parameterForTarget(std::string_view role, AuthoringTarget target) const noexcept;
    [[nodiscard]] bool setSelectionScalarParameter(std::string_view role, double value,
                                                   const QString& commandLabel,
                                                   AuthoringTarget target);
    // The Vec2d counterpart of setSelectionScalarParameter(), shared by every Vec2d transform row.
    // It routes through executePositionCommand() deliberately: the constant/keyframe/driven
    // decision is identical for position, anchor, and scale, so one write path serves all three.
    [[nodiscard]] bool setSelectionVec2Parameter(std::string_view role, double x, double y,
                                                 const QString& commandLabel,
                                                 AuthoringTarget target);
    // The one command-selection decision for writing a Color4d-valued parameter, shared by
    // setSelectedSolidColor() and setSelectedTextColor(). Identical in shape to the scalar and Vec2
    // helpers since task S5: constant source -> SetParameterSource, animation source ->
    // SetKeyframeAtTime at the session time, driver source -> refused.
    [[nodiscard]] bool setSelectionColorParameter(std::string_view role, core::Color4d color,
                                                  const QString& commandLabel,
                                                  AuthoringTarget target);
    // The one command-selection decision for writing a position value (constant source ->
    // SetParameterSource; animation source -> SetKeyframeAtTime at `time`; driver source ->
    // rejected), executed as exactly one transaction. setSelectedPosition() calls this with its
    // live-derived parameter/time; commitTransformInteraction() calls it with the FROZEN
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
    // branches) or if the curve fails to resolve/sample. Both beginTransformInteraction() (the
    // relaxed animated-base rule) and insertKeyframeAtTime() (the timeline insert gesture) call
    // this one place rather than each re-deriving the curve/sample logic.
    [[nodiscard]] std::optional<ParameterSample>
    sampleParameterValue(const document::ParameterRecord& parameter, core::RationalTime time) const;
    // The constant-or-sampled reader behind effectiveScalarValue()/effectiveVec2Value()/
    // effectiveColorValue(): one place that decides "what is this parameter's value right now",
    // so the three typed accessors cannot disagree about a driven or missing source.
    [[nodiscard]] std::optional<ParameterSample>
    effectiveParameterValue(const document::ParameterRecord* parameter) const;
    // The key (if any) sitting at EXACTLY `time` on the parameter's curve. The gesture's whole
    // three-way branch turns on this one question.
    [[nodiscard]] std::optional<document::KeyframeId>
    keyframeAtExactTime(const document::ParameterRecord& parameter, core::RationalTime time) const;
    // The two primitives behind both keyframeDiamondState() overloads and both toggleKeyframe()
    // overloads: the role-keyed and parameter-keyed public spellings differ only in how they find
    // the ParameterRecord, never in what they then decide.
    [[nodiscard]] KeyframeDiamondState
    diamondStateFor(const document::ParameterRecord* parameter) const;
    [[nodiscard]] bool toggleKeyframeFor(const document::ParameterRecord* parameter);
    [[nodiscard]] std::size_t keyframeCount(document::AnimationCurveId curveId) const;
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
    void invalidateTransformInteractionOnStaleRevision();

    // Session-only, never persisted (docs/architecture/animation-and-time.md, "Direct
    // Manipulation And Preview Overrides"). Named after exactly what the contract freezes.
    struct TransformInteraction final {
        document::Revision baseRevision;
        document::LayerId layerId;
        core::RationalTime time;
        ViewerMapping mapping;
        TransformGesture gesture;
        // position, anchor, scale, rotation, sampled once at time.
        std::array<document::ParameterId, 4> parameters;
        document::Vec2d position;
        document::Vec2d anchor;
        document::Vec2d scale;
        double rotation = 0;
        QTransform inverseParent;
        std::vector<runtime::SnapshotParameterOverride> overrides;
        enum class NativeKind { Raster, Size, Solid, PointText, BoxText, Line, Path };
        NativeKind nativeKind = NativeKind::Raster;
        std::vector<std::pair<document::ParameterId, document::ParameterValue>> native;
    };

    [[nodiscard]] bool appendParameterEdit(commands::Transaction& transaction,
                                           document::ParameterId parameterId,
                                           core::RationalTime time, document::ParameterValue value);

    // Pointers, not references (task U1, issue #72): rebind() must be able to atomically retarget
    // which document/command-stack this session projects after ProjectHost replaces the live
    // ProjectSession content. A reference member cannot be reseated; both are set at construction
    // and by rebind(), and are never null while this object is alive.
    std::shared_ptr<SessionColorConverterState> colorConverterState_;
    document::Document* document_;
    commands::CommandStack* commandStack_;
    document::Snapshot snapshot_;
    document::CompositionId compositionId_;
    core::RationalTime currentTime_ = core::RationalTime::fromInteger(0);
    CompositionSelection selection_;
    std::vector<commands::KeyframePaste> keyframeClipboard_;
    std::set<document::NodeId> selectedNodes_;
    std::optional<TransformInteraction> transformInteraction_;
    struct ValueEdit final {
        document::Revision revision;
        document::ParameterId parameter;
        core::RationalTime time;
        std::optional<document::AnimationComponent> component;
        document::ParameterValue base;
        document::ParameterValue value;
        struct PathAnchorEdit {
            document::ParameterId parameter;
            document::Vec2d base;
            document::Vec2d value;
        };
        std::optional<PathAnchorEdit> anchor{};
    };
    std::optional<ValueEdit> valueEdit_;
    // Task DRIVE-1's shared driver resolution. The evaluator is created on the first refresh that
    // finds a driven parameter and never before, so a composition with no drivers -- every existing
    // test fixture among them -- pays nothing at all for the capability. `drivenRequest_` is the
    // revision/time/parameter signature of the newest request, which is what makes
    // refreshDrivenValues() idempotent and therefore safe to call from a handler of the signal it
    // eventually emits.
    class DrivenValueResolver* drivenValues_ = nullptr;
    QString drivenRequest_;
    std::map<document::ParameterId, QString> drivenText_;
};

} // namespace bloom::ui
