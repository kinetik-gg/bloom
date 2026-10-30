// MCP parity: the layer edits of the Timeline and Viewer menus and of an asset drop (ADR 0030), and
// the Node canvas's compound adds. Each op builds the command the UI issues, so it validates and
// undoes exactly as the UI action does.

#include <bloom/commands/layer_drop_operations.hpp>
#include <bloom/commands/layer_stack_operations.hpp>
#include <bloom/commands/node_editor_operations.hpp>
#include <bloom/commands/node_layout_arrange.hpp>

#include "operation_registry_parity.hpp"

#include <set>
#include <string>
#include <vector>

namespace bloom::scripting::detail::parity {
namespace {

using document::AssetId;
using document::CompositionId;
using document::LayerId;
using document::NodeId;

[[nodiscard]] ArgumentSchema layerSet() {
    return fromContext(
        "layers", ValueKind::Array, ContextSource::Layers, ValueArray{Value(StableId{1})},
        "Array of layer ids (the ids query kind=layers and bloom.layer.add-* outputs "
        "return; not node ids). Defaults to every selected layer.");
}

const std::string kAddFromAssets =
    R"(Adds footage and compositions as layers, as dropping them from Assets on the Timeline or )"
    R"(the Viewer does (ADR 0030), in ONE undo step. Each asset becomes the layer for its kind: )"
    R"(an image, image sequence or video asset an image/video layer, an audio asset an audio )"
    R"(layer; each composition a nested-composition layer named after it. Every layer keeps its )"
    R"(media's own length (ADR 0024: after its last frame it shows nothing). )"
    R"(Order: the assets in the order given, then the compositions; the first item is the )"
    R"(uppermost layer. Placement: all new layers go on top of the stack (placement "top", the )"
    R"(default), below every layer ("bottom"), or directly above the layer `before` ("before"). )"
    R"(The first layer starts at `time` snapped to the nearest composition frame; with )"
    R"(sequence=true each next layer starts where the one before ends (Alt-drop, After Effects' )"
    R"(Sequence Layers), otherwise all start at `time`. )"
    R"(Side effects: adds a source node and a Layer Output node per item to the composition graph )"
    R"(and a stack slot on the output Merge; nested layers also route audio. Does not import )"
    R"(files: run bloom.asset.import first and pass the asset ids it outputs. )"
    R"(Outputs: one "layer" output per item, in item order (layer ids). )"
    R"(Errors: "This item cannot be a layer here" - an id that is not an image, sequence, video )"
    R"(or audio asset (fonts and LUTs are not layers), a missing asset or composition, or the )"
    R"(target composition itself; pass only footage and other compositions. "Nothing to add" - )"
    R"(both arrays empty. "The layer to insert above no longer exists" - `before` is not a layer )"
    R"(of this composition. A composition that would nest itself (a cycle) is rejected. )"
    R"(Undo: one step, the transaction label (the UI uses "Add Layer", "Add Composition Layer" or )"
    R"("Add N Layers"). )"
    R"(MCP example: {"op":"bloom.layer.add-from-assets","args":{"assets":[3,4],"time":0,)"
    R"("sequence":true}}.)";

const std::string kDelete =
    R"(Deletes layers, as Delete in the Timeline or Delete Layer in the Viewer's context menu )"
    R"(does: each layer's Layer Output node is removed with its stack slot, masks, track-matte )"
    R"(and parent links. Upstream source nodes are left in the graph, as in the UI. )"
    R"(Outputs: none. )"
    R"(Errors: "Layer N is not in the composition" - a stale or foreign layer id, re-read the )"
    R"(layers; "No layers were named" - an empty array with no selected layers. )"
    R"(Undo: one step; undo restores the layers with their ids. )"
    R"(MCP example: {"op":"bloom.layer.delete","args":{"layers":[2]}}.)";

const std::string kDuplicate =
    R"(Duplicates layers, as Duplicate Layer in the Viewer's context menu does: each layer's )"
    R"(Layer Output node is copied with its parameters, keyframes, masks, timing, parent and )"
    R"(matte, reading the same source as the original, and the copy takes a new stack slot )"
    R"(directly below the original. Layers duplicated together keep their parent and matte links )"
    R"(among the copies. )"
    R"(Outputs: one "layer" output per copy in argument order, plus "layer.<original>" -> copy, )"
    R"("node.<original>" -> copied node and "slot.<original>" -> new slot. )"
    R"(Errors: "Layer N is not in the composition" - a stale or foreign layer id; "Driven )"
    R"(parameters cannot be deeply duplicated" - a parameter driven by a node link. )"
    R"(Undo: one step. )"
    R"(MCP example: {"op":"bloom.layer.duplicate","args":{"layers":[2]}}.)";

const std::string kAddAssetSource =
    R"(Adds a source node for an asset, as dropping a footage asset on the Node canvas does: an )"
    R"(Image Source for an image or image sequence, a Video Source for a video, an Audio Source )"
    R"(for audio, whose asset parameter names the asset (image and video sources also take the )"
    R"(asset's probed alpha association as their premultiply flag). The node is not wired: )"
    R"(connect it with bloom.node.connect, or use bloom.layer.add-from-assets for a whole layer. )"
    R"(Outputs: "node" (the new node id) and "parameter.<role>" for each of its parameters. )"
    R"(Errors: "The asset does not exist"; a font or LUT asset is rejected (it makes no source). )"
    R"(Undo: one step. )"
    R"(MCP example: {"op":"bloom.node.add-asset-source","args":{"asset":3,"position":[0,0]}}.)";

const std::string kAddCompositionSource =
    R"(Adds a Composition Source node that renders another composition, as dropping a )"
    R"(composition on the Node canvas does. Its composition parameter names `source`. Not wired; )"
    R"(bloom.layer.add-from-assets makes a nested-composition layer instead. )"
    R"(Outputs: "node" and "parameter.<role>" for each parameter. )"
    R"(Errors: "Source composition does not exist"; a source that would nest the composition )"
    R"(inside itself is rejected with the nesting cycle. )"
    R"(Undo: one step. )"
    R"(MCP example: {"op":"bloom.node.add-composition-source","args":{"source":2}}.)";

const std::string kAddFeeding =
    R"(Adds a node that feeds an existing input, in one step, as dragging a wire backwards from an )"
    R"(input socket to empty canvas and picking a node from the search does: the new node's first )"
    R"(output whose value kind matches the input is connected to `node`.`port` (or to a new slot of )"
    R"(a Merge stack when `role` is given), replacing what fed it. The node is placed to the left )"
    R"(of `node`, in the nearest free spot. )"
    R"(Outputs: "node", "editorNode" (both the new node id) and "parameter.<role>". )"
    R"(Errors: "Node has no compatible socket" - no output of the new type matches the input's )"
    R"(value kind; unknown node type or input; "No active composition". )"
    R"(Undo: one step. )"
    R"(MCP example: {"op":"bloom.node.add-feeding","args":{"nodeType":"bloom.value-scalar",)"
    R"("node":5,"port":"radius"}}.)";

const std::string kAddFedBy =
    R"(Adds a node fed by an existing output, in one step, as dragging a wire from an output )"
    R"(socket to empty canvas and picking a node from the search does: `node`.`port` is connected )"
    R"(to the new node's layer stack (a Merge) or its first input whose value kind matches. The )"
    R"(node is placed to the right of `node`, in the nearest free spot. )"
    R"(Outputs: "node", "editorNode" (both the new node id) and "parameter.<role>". )"
    R"(Errors: "Node has no compatible socket" - no input of the new type matches the output's )"
    R"(value kind; unknown node type or output; "No active composition". )"
    R"(Undo: one step. )"
    R"(MCP example: {"op":"bloom.node.add-fed-by","args":{"nodeType":"bloom.blur","node":5,)"
    R"("port":"image"}}.)";

const std::string kInsertReroute =
    R"(Inserts a Reroute node into an existing link, as Shift+right-dragging across a wire does: )"
    R"(the reroute is fed by the link's source and feeds the link's destination input, at )"
    R"(`position`. Name the link by its destination input (`node` + `port`, or `role` + `slot` )"
    R"(for a Merge stack input). )"
    R"(Outputs: "node" and "editorNode" (the reroute's node id). )"
    R"(Errors: "That link no longer exists" - nothing feeds that input; read the graph again. )"
    R"(Undo: one step. )"
    R"(MCP example: {"op":"bloom.node.insert-reroute","args":{"node":5,"port":"image",)"
    R"("position":[200,0]}}.)";

const std::string kArrange =
    R"(Lays the node graph out from sources on the left to Composition Output on the right, as )"
    R"(the Node editor's Organize > Arrange All (no `nodes`) or Arrange Selection (`nodes`) does. )"
    R"(Arrange Selection lays out only those nodes and keeps their bounding box's top-left )"
    R"(corner. Node groups and driver links are respected. Only layout positions change, never )"
    R"(the graph or the render. Card sizes are the conservative document card size (the editor )"
    R"(measures its cards, so its spacing can differ slightly). )"
    R"(Outputs: none. )"
    R"(Errors: "Node N is not in the composition" - a stale node id. An empty graph is a )"
    R"(no-change. )"
    R"(Undo: one step. )"
    R"(MCP example: {"op":"bloom.node.arrange","args":{}}.)";

// A destination input by node + port, or by Merge stack role + slot.
[[nodiscard]] std::optional<document::InputPortRef> input(Reader& read, const std::string& node,
                                                          const std::string& port,
                                                          const std::string& role,
                                                          const std::string& slot) {
    const auto target = read.id<NodeId>(node);
    const auto stackRole = read.text(role);
    if (stackRole) {
        const auto slotId = read.id<document::LayerSlotId>(slot, false);
        return document::InputPortRef(document::LayerStackInputRef{
            .stackNodeId = target, .slotId = slotId, .role = *stackRole});
    }
    const auto name = read.text(port);
    if (!name) {
        read.reject(port, "is required unless a stack role is given");
        return std::nullopt;
    }
    return document::InputPortRef(document::NodeInputRef{.nodeId = target, .port = *name});
}

} // namespace

void addLayerAndNodeOperations(std::vector<OperationDescriptor>& descriptors) {
    add(descriptors, "bloom.layer.delete", kDelete, {composition(), layerSet()},
        [](const std::string& op, const Arguments& args) {
            Reader read(op, args);
            const auto compositionId = read.id<CompositionId>("composition");
            auto layers = read.ids<LayerId>("layers");
            if (read.failed())
                return read.error();
            return made(std::make_unique<commands::DeleteLayers>(compositionId, std::move(layers)));
        });

    add(descriptors, "bloom.layer.duplicate", kDuplicate, {composition(), layerSet()},
        [](const std::string& op, const Arguments& args) {
            Reader read(op, args);
            const auto compositionId = read.id<CompositionId>("composition");
            auto layers = read.ids<LayerId>("layers");
            if (read.failed())
                return read.error();
            return made(
                std::make_unique<commands::DuplicateLayers>(compositionId, std::move(layers)));
        });

    add(descriptors, "bloom.layer.add-from-assets", kAddFromAssets,
        {composition(),
         fromContext(
             "time", ValueKind::Time, ContextSource::Time, std::int64_t{0},
             "Time: a whole frame of the composition at its frame rate, or an exact [numerator, "
             "denominator] time in seconds). Where the first layer starts, snapped to the "
             "nearest frame inside the composition. Defaults to the playhead."),
         maybe("assets", ValueKind::Array, ValueArray{Value(StableId{1})},
               "Array of asset ids: image, image sequence, video or audio assets, in stack order "
               "(first is uppermost). Default: none.",
               true),
         maybe("compositions", ValueKind::Array, ValueArray{},
               "Array of composition ids to nest, after the assets. Not the target composition. "
               "Default: none."),
         maybe("placement", ValueKind::String, "top",
               R"(Enum "top" (default: above every layer), "bottom" (below every layer) or )"
               R"("before" (directly above the layer `before`).)"),
         maybe("before", ValueKind::Id, StableId{1},
               R"(Id. The layer the new layers go directly above. Required with placement )"
               R"("before"; giving it alone implies "before".)"),
         maybe("sequence", ValueKind::Boolean, false,
               "Boolean. true: each layer starts where the previous one ends. Default false.")},
        [](const std::string& op, const Arguments& args) {
            Reader read(op, args);
            const auto compositionId = read.id<CompositionId>("composition");
            const auto start = read.time("time");
            const auto assets = read.ids<AssetId>("assets", false);
            const auto compositions = read.ids<CompositionId>("compositions", false);
            const auto placement = read.text("placement");
            const auto before = read.id<LayerId>("before", false);
            const auto sequence = read.boolean("sequence");
            if (read.failed())
                return read.error();
            commands::LayerDropPlacement where{.start = start.value_or(core::RationalTime{}),
                                               .sequence = sequence.value_or(false)};
            const auto mode = placement.value_or(before.isValid() ? "before" : "top");
            if (mode == "top") {
                where.stack = commands::LayerDropPlacement::Stack::Top;
            } else if (mode == "bottom") {
                where.stack = commands::LayerDropPlacement::Stack::Bottom;
            } else if (mode == "before") {
                if (!before.isValid())
                    return argumentFailure(op, "before", R"(is required with placement "before")");
                where.stack = commands::LayerDropPlacement::Stack::Before;
                where.beforeLayer = before;
            } else {
                return argumentFailure(op, "placement", R"(expects "top", "bottom" or "before")");
            }
            std::vector<commands::LayerDropItem> items;
            for (const auto asset : assets)
                items.emplace_back(asset);
            for (const auto nested : compositions)
                items.emplace_back(nested);
            if (items.empty())
                return argumentFailure(op, "assets",
                                       "needs at least one asset or composition id (assets or "
                                       "compositions)");
            return made(std::make_unique<commands::AddDroppedLayers>(compositionId,
                                                                     std::move(items), where));
        });

    add(descriptors, "bloom.node.add-asset-source", kAddAssetSource,
        {composition(),
         need("asset", ValueKind::Id, StableId{1},
              "Id. An image, image sequence, video or audio asset."),
         maybe("position", ValueKind::Vec2, document::Vec2d{0.0, 0.0},
               "Vec2 [x, y] in Node canvas units (pixels at 100% zoom). Default [0, 0].")},
        [](const std::string& op, const Arguments& args) {
            Reader read(op, args);
            const auto compositionId = read.id<CompositionId>("composition");
            const auto asset = read.id<AssetId>("asset");
            const auto position = read.vec2("position");
            if (read.failed())
                return read.error();
            return made(std::make_unique<commands::AddAssetSourceNode>(
                compositionId, asset, position.value_or(document::Vec2d{})));
        });

    add(descriptors, "bloom.node.add-composition-source", kAddCompositionSource,
        {composition(),
         need("source", ValueKind::Id, StableId{2},
              "Id. The composition the node renders; not the target composition."),
         maybe("position", ValueKind::Vec2, document::Vec2d{0.0, 0.0},
               "Vec2 [x, y] in Node canvas units. Default [0, 0].")},
        [](const std::string& op, const Arguments& args) {
            Reader read(op, args);
            const auto compositionId = read.id<CompositionId>("composition");
            const auto source = read.id<CompositionId>("source");
            const auto position = read.vec2("position");
            if (read.failed())
                return read.error();
            return made(std::make_unique<commands::AddCompositionSource>(
                compositionId, source, position.value_or(document::Vec2d{}), false));
        });

    add(descriptors, "bloom.node.add-feeding", kAddFeeding,
        {composition(),
         need("nodeType", ValueKind::String, "bloom.value-scalar",
              "String. The node definition id of the new node."),
         need("node", ValueKind::Id, StableId{1}, "Id. The node whose input the new node feeds."),
         maybe("port", ValueKind::String, "radius",
               "String. That input's port name. Required unless role is given.", true),
         maybe("role", ValueKind::String, "content",
               "String. A Merge stack role on `node` instead of a port; a new slot is made."),
         maybe("position", ValueKind::Vec2, document::Vec2d{0.0, 0.0},
               "Vec2 [x, y] in Node canvas units; the node is then moved beside `node`. "
               "Default [0, 0].")},
        [](const std::string& op, const Arguments& args) {
            Reader read(op, args);
            const auto compositionId = read.id<CompositionId>("composition");
            const auto type = read.text("nodeType");
            auto to = input(read, "node", "port", "role", "slot");
            const auto position = read.vec2("position");
            if (read.failed() || !to || !type)
                return read.error();
            return made(std::make_unique<commands::AddEditorNode>(
                compositionId, *type, position.value_or(document::Vec2d{}), std::move(to)));
        });

    add(descriptors, "bloom.node.add-fed-by", kAddFedBy,
        {composition(),
         need("nodeType", ValueKind::String, "bloom.blur",
              "String. The node definition id of the new node."),
         need("node", ValueKind::Id, StableId{1}, "Id. The node whose output feeds the new node."),
         need("port", ValueKind::String, "image", "String. That output's port name."),
         maybe("position", ValueKind::Vec2, document::Vec2d{0.0, 0.0},
               "Vec2 [x, y] in Node canvas units; the node is then moved beside `node`. "
               "Default [0, 0].")},
        [](const std::string& op, const Arguments& args) {
            Reader read(op, args);
            const auto compositionId = read.id<CompositionId>("composition");
            const auto type = read.text("nodeType");
            const auto node = read.id<NodeId>("node");
            const auto port = read.text("port");
            const auto position = read.vec2("position");
            if (read.failed() || !type || !port)
                return read.error();
            return made(std::make_unique<commands::AddEditorNode>(
                compositionId, *type, position.value_or(document::Vec2d{}), std::nullopt,
                document::OutputPortRef{.nodeId = node, .port = *port}));
        });

    add(descriptors, "bloom.node.arrange", kArrange,
        {composition(),
         maybe("nodes", ValueKind::Array, ValueArray{},
               "Array of node ids to arrange among themselves. Default (omitted or empty): every "
               "node of the composition.")},
        [](const std::string& op, const Arguments& args) {
            Reader read(op, args);
            const auto compositionId = read.id<CompositionId>("composition");
            const auto nodes = read.ids<NodeId>("nodes", false);
            if (read.failed())
                return read.error();
            return made(std::make_unique<commands::ArrangeNodes>(
                compositionId, std::set<NodeId>(nodes.begin(), nodes.end())));
        });

    add(descriptors, "bloom.node.insert-reroute", kInsertReroute,
        {composition(),
         need("node", ValueKind::Id, StableId{1}, "Id. The node whose input the link feeds."),
         maybe("port", ValueKind::String, "image",
               "String. That input's port name. Required unless role is given.", true),
         maybe("role", ValueKind::String, "content",
               "String. A Merge stack role instead of a port; use with slot."),
         maybe("slot", ValueKind::Id, StableId{1}, "Id. The Merge stack slot of that role."),
         maybe("position", ValueKind::Vec2, document::Vec2d{0.0, 0.0},
               "Vec2 [x, y] in Node canvas units. Default [0, 0].")},
        [](const std::string& op, const Arguments& args) {
            Reader read(op, args);
            const auto compositionId = read.id<CompositionId>("composition");
            auto destination = input(read, "node", "port", "role", "slot");
            const auto position = read.vec2("position");
            if (read.failed() || !destination)
                return read.error();
            return made(std::make_unique<commands::InsertReroute>(
                compositionId, *std::move(destination), position.value_or(document::Vec2d{})));
        });
}

} // namespace bloom::scripting::detail::parity
