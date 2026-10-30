// MCP parity: the Assets panel's delete and New Composition from Selection, the Properties
// panel's Reset to default, its typed node-parameter edits (enums, text, switches) and a video
// source's audio stream. Each op builds the command the panel issues.

#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/parameter_reset_operations.hpp>
#include <bloom/commands/project_item_operations.hpp>
#include <bloom/commands/video_source_operations.hpp>

#include "operation_registry_parity.hpp"

#include <limits>
#include <string>
#include <vector>

namespace bloom::scripting::detail::parity {
namespace {

using document::AssetFolderId;
using document::AssetId;
using document::CompositionId;
using document::NodeId;
using document::ParameterId;

const std::string kDeleteItems =
    R"(Deletes Assets items as Delete in the Assets panel does, in one undo step: the assets, )"
    R"(then the compositions, then the folders. withContents=false (the default) removes each )"
    R"(folder alone and moves what it held to its parent folder; withContents=true also deletes )"
    R"(every asset, composition and subfolder inside it. Nothing asks first: the panel's "used )"
    R"(item" warning is the caller's to give (layers and nodes that used a deleted item keep )"
    R"(referring to it and show it as missing). Deleting the composition the editor shows )"
    R"(switches the editor to another one. )"
    R"(Outputs: none. )"
    R"(Errors: "Nothing to delete" - all three arrays empty; "The folder does not exist"; an )"
    R"(unknown asset or composition id is rejected by its remove command; deleting the last )"
    R"(composition is refused. )"
    R"(Undo: one step restores every item with its id and folder. )"
    R"(MCP example: {"op":"bloom.asset.delete-items","args":{"folders":[2],)"
    R"("withContents":true}}.)";

const std::string kNewFromAssets =
    R"(Creates a composition from footage, as New Composition from Selection in the Assets panel )"
    R"(does: sized, timed and named from the first image, sequence or video asset (the first )"
    R"(asset when all are audio; a video's frame rate and a video or audio duration are used), )"
    R"(with what the footage does not state taken from `template`, filed in `folder`, and every )"
    R"(footage asset added as a layer, the first uppermost. Fonts and LUTs are skipped. It does )"
    R"(not make the new composition the one the editor shows. )"
    R"(Outputs: "composition" (the new id) and one "layer" per footage asset. )"
    R"(Errors: "Asset N does not exist"; "Name at least one image, sequence, video or audio )"
    R"(asset"; "The template composition does not exist"; "The destination folder does not )"
    R"(exist". )"
    R"(Undo: one step. )"
    R"(MCP example: {"op":"bloom.composition.add-from-assets","args":{"assets":[3,4]}}.)";

const std::string kReset =
    R"(Resets a node parameter to its node type's default, as "Reset to default" on a )"
    R"(Properties row does. A driven parameter is disconnected (restoring the default); a )"
    R"(constant parameter takes the default value; an animated parameter gets a key with the )"
    R"(default value at `time` and keeps its other keys (convert it to constant first, with )"
    R"(bloom.animation.convert-to-constant, to drop the animation). )"
    R"(Outputs: none, or the key's "keyframe" output for an animated parameter. )"
    R"(Errors: "The parameter is not in the composition" - a stale id; "The parameter has no node )"
    R"(default to reset to" - it does not belong to a node (a layer-stack or composition value); )"
    R"(a text, enum or path default cannot be keyed on an animated parameter. )"
    R"(Undo: one step. )"
    R"(MCP example: {"op":"bloom.parameter.reset","args":{"parameter":12}}.)";

const std::string kSetNodeValue =
    R"(Sets one node parameter to a constant by its role, as the Properties panel's typed )"
    R"(controls do: dropdowns (an enum index, as `integer`), switches (`boolean`), text fields )"
    R"((`text`), numbers (`number`) and vectors or colours (`vector`, 2 to 4 numbers). Give )"
    R"(one value argument; if several are given the first of integer, number, boolean, text, )"
    R"(vector is used. A role the node type declares as optional and the node does )"
    R"(not hold yet is created, as the panel does. An animated or driven parameter becomes a )"
    R"(constant. )"
    R"(Outputs: "parameter" when an optional parameter was created. )"
    R"(Errors: "Node does not exist"; "The node has no such optional parameter" - a role the node )"
    R"(type does not declare (query kind=node-types lists the roles); "The value violates the )"
    R"(parameter schema" - the wrong value kind or a value out of range. )"
    R"(Undo: one step. )"
    R"(MCP example: {"op":"bloom.node.set-parameter-value","args":{"node":5,"role":"blendMode",)"
    R"("integer":3}}.)";

const std::string kAudioStream =
    R"(Chooses which audio stream of a Video Source node's file plays, as the Audio Stream )"
    R"(dropdown in the Properties panel's Video section does. `stream` is the container's stream )"
    R"(index as the asset's probe lists it. )"
    R"(Outputs: none. )"
    R"(Errors: the node is not a Video Source, or its asset has no audio stream with that index. )"
    R"(Undo: one step. )"
    R"(MCP example: {"op":"bloom.node.set-video-audio-stream","args":{"node":5,"stream":1}}.)";

} // namespace

void addProjectOperations(std::vector<OperationDescriptor>& descriptors) {
    add(descriptors, "bloom.asset.delete-items", kDeleteItems,
        {maybe("assets", ValueKind::Array, ValueArray{Value(StableId{1})},
               "Array of asset ids. Default: none.", true),
         maybe("compositions", ValueKind::Array, ValueArray{},
               "Array of composition ids. Default: none."),
         maybe("folders", ValueKind::Array, ValueArray{},
               "Array of Assets folder ids. Default: none."),
         maybe("withContents", ValueKind::Boolean, false,
               "Boolean. true also deletes everything inside the folders. Default false.")},
        [](const std::string& op, const Arguments& args) {
            Reader read(op, args);
            commands::ProjectItems items{.assets = read.ids<AssetId>("assets", false),
                                         .compositions =
                                             read.ids<CompositionId>("compositions", false),
                                         .folders = read.ids<AssetFolderId>("folders", false)};
            const auto contents = read.boolean("withContents");
            if (read.failed())
                return read.error();
            if (items.assets.empty() && items.compositions.empty() && items.folders.empty())
                return argumentFailure(op, "assets",
                                       "needs at least one asset, composition or folder id");
            return made(std::make_unique<commands::DeleteProjectItems>(std::move(items),
                                                                       contents.value_or(false)));
        });

    add(descriptors, "bloom.composition.add-from-assets", kNewFromAssets,
        {fromContext("template", ValueKind::Id, ContextSource::Composition, StableId{1},
                     "Id. The composition whose settings fill what the footage does not state "
                     "(pixel aspect, background, working colour space, safe areas). Defaults to "
                     "the current composition."),
         need("assets", ValueKind::Array, ValueArray{Value(StableId{1})},
              "Array of asset ids; the first visual one sizes the composition."),
         maybe("folder", ValueKind::Id, StableId{1},
               "Id. The Assets folder the composition is filed in. Default: the Assets root.")},
        [](const std::string& op, const Arguments& args) {
            Reader read(op, args);
            const auto templateComposition = read.id<CompositionId>("template");
            auto assets = read.ids<AssetId>("assets");
            const auto folder = read.id<AssetFolderId>("folder", false);
            if (read.failed())
                return read.error();
            return made(std::make_unique<commands::AddCompositionFromFootage>(
                std::move(assets), templateComposition, std::string{},
                folder.isValid() ? std::optional(folder) : std::nullopt));
        });

    add(descriptors, "bloom.parameter.reset", kReset,
        {composition(),
         fromContext("time", ValueKind::Time, ContextSource::Time, std::int64_t{0},
                     "Time. For an animated parameter only: the key time of the default key, a "
                     "whole number or an exact [numerator, denominator] in seconds, as "
                     "bloom.animation.set-keyframe-at-time-for-parameter takes it. Defaults to "
                     "the playhead."),
         need("parameter", ValueKind::Id, StableId{1},
              "Id. The parameter (query kind=parameters lists ids).")},
        [](const std::string& op, const Arguments& args) {
            Reader read(op, args);
            const auto compositionId = read.id<CompositionId>("composition");
            const auto time = read.time("time");
            const auto parameter = read.id<ParameterId>("parameter");
            if (read.failed())
                return read.error();
            return made(std::make_unique<commands::ResetParameter>(
                compositionId, parameter, time.value_or(core::RationalTime{})));
        });

    add(descriptors, "bloom.node.set-parameter-value", kSetNodeValue,
        {composition(), need("node", ValueKind::Id, StableId{1}, "Id. The node."),
         need("role", ValueKind::String, "blendMode",
              "String. The parameter's role on the node type (for example \"blendMode\", "
              "\"text\", \"enabled\")."),
         maybe("integer", ValueKind::Integer, std::int64_t{0},
               "Integer. A whole number or an enum index (dropdowns).", true),
         maybe("number", ValueKind::Double, 1.0, "Number. A real value."),
         maybe("boolean", ValueKind::Boolean, true, "Boolean. A switch."),
         maybe("text", ValueKind::String, "Title", "String. A text value."),
         maybe("vector", ValueKind::Value, core::Color4d{1.0, 1.0, 1.0, 1.0},
               "Value. A vec2 [x, y], vec3 [x, y, z] or colour [r, g, b, a] (scene-linear, "
               "alpha 0..1).")},
        [](const std::string& op, const Arguments& args) {
            Reader read(op, args);
            const auto compositionId = read.id<CompositionId>("composition");
            const auto node = read.id<NodeId>("node");
            const auto role = read.text("role");
            // The first value argument given, in schema order, is the value (every client sends
            // one; the schema cannot say "exactly one").
            std::optional<document::ParameterValue> value;
            const auto integer = read.integer("integer");
            const auto number = read.number("number");
            const auto flag = read.boolean("boolean");
            const auto text = read.text("text");
            const auto vector = read.vector("vector");
            if (read.failed())
                return read.error();
            if (integer)
                value = *integer;
            else if (number)
                value = *number;
            else if (flag)
                value = *flag;
            else if (text)
                value = *text;
            else if (vector)
                value = *vector;
            if (!value || !role)
                return argumentFailure(op, "integer",
                                       "needs one of integer, number, boolean, text or vector");
            return made(std::make_unique<commands::SetNodeParameterValue>(compositionId, node,
                                                                          *role, *value));
        });

    add(descriptors, "bloom.node.set-video-audio-stream", kAudioStream,
        {composition(), need("node", ValueKind::Id, StableId{1}, "Id. The Video Source node."),
         need("stream", ValueKind::Integer, std::int64_t{1},
              "Integer >= 0. The container's audio stream index.")},
        [](const std::string& op, const Arguments& args) {
            Reader read(op, args);
            const auto compositionId = read.id<CompositionId>("composition");
            const auto node = read.id<NodeId>("node");
            const auto stream = read.integer("stream");
            if (read.failed())
                return read.error();
            if (!stream || *stream < 0 || *stream > std::numeric_limits<std::uint32_t>::max())
                return argumentFailure(op, "stream", "expects a stream index from 0");
            return made(std::make_unique<commands::SetVideoAudioStream>(
                compositionId, node, static_cast<std::uint32_t>(*stream)));
        });
}

} // namespace bloom::scripting::detail::parity
