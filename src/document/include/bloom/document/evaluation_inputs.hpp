#pragma once

#include <bloom/document/project.hpp>

namespace bloom::document {

// Issue #399. The ONE classification of project truth into evaluation inputs and presentation.
//
// An evaluation input is a field a compiled plan or an evaluated frame can depend on. Everything
// else -- node-editor layout, names, label colors, organization, viewer overlays, the export
// queue -- is presentation: it is saved, undoable and dirties the project, but no rendered pixel
// reads it. The command stack proves a transaction render-neutral with sameEvaluationInputs()
// (src/commands/command_stack.cpp), and a render-neutral transaction leaves the retained
// evaluation snapshot, the compiled plan, the preview frame cache and the operation cache valid
// (docs/architecture/animation-and-time.md, "Invalidation is the key").
//
// How a field is classified, from the coarsest rule to the finest:
//
// 1. Presentation collections are skipped whole, so every field added to their records is
//    presentation BY CONSTRUCTION: node groups (NodeGroupRecord: name, members, padding, and any
//    later frame color), asset folders, the export queue, and the project name.
// 2. Node layout (NodeLayoutRecord) is presentation except `muted`, the one field evaluation reads,
//    and it reads it only through nodeMuted(). A new card field (collapse, color, width) is
//    presentation by construction; a field the compiler must read gets its own accessor here.
// 3. Records that mix both kinds list their presentation fields in evaluationView(), which resets
//    exactly those; every other field, including one added later, stays an evaluation input until
//    someone names it there. A forgotten classification therefore costs a re-render, never a stale
//    frame. Today: a layer's name, label color, lock, shy flag and name-follows-source flag, a
//    mask's name; an asset's name, folder, tags and Assets order.
// 4. Composition settings are listed in sameEvaluationInputs(Composition): duration, format and
//    working color space are evaluation inputs; name, folder, work area, safe areas, timecode
//    display and the viewer background color are presentation. A new Composition member must be
//    classified there as well as in Composition::sameShell().
//
// A name is presentation only while nothing evaluated refers to it. Every evaluation reference in
// the document is by stable id (edges, driver bindings, track mattes, parents, composition
// sources); text a node evaluates by name -- a channel expression naming a channel -- lives in a
// parameter value and is therefore an evaluation input like any parameter. A feature that makes an
// evaluated value read a layer, node or group name must move that name out of rule 1 or 3.
//
// Session state (selection, current time, zoom, panel layout) is not project truth and never
// reaches any of this.

// The one node-layout field evaluation reads: whether `node` is muted (bypassed).
[[nodiscard]] bool nodeMuted(const NodeLayout& layout, NodeId node) noexcept;

// The record with every presentation field reset to its default (rule 3).
[[nodiscard]] LayerOutputBoundary evaluationView(LayerOutputBoundary boundary);
[[nodiscard]] AssetRecord evaluationView(AssetRecord asset);

// Whether two graphs present identical evaluation inputs (topology, nodes, parameters bindings,
// stacks, and layer boundaries through evaluationView()).
[[nodiscard]] bool sameEvaluationGraph(const CanonicalGraph& left, const CanonicalGraph& right);
// Whether two node layouts mute exactly the same nodes.
[[nodiscard]] bool sameEvaluationLayout(const NodeLayout& left, const NodeLayout& right);
// Whether two versions of one composition present identical evaluation inputs.
[[nodiscard]] bool sameEvaluationInputs(const Composition& left, const Composition& right);
// Whether two versions of a project present identical evaluation inputs: the proof that an edit
// between them cannot change any rendered pixel of any composition. Unchanged shared storage is
// recognized by identity, so the cost is proportional to what the edit touched.
[[nodiscard]] bool sameEvaluationInputs(const Project& left, const Project& right);

} // namespace bloom::document
