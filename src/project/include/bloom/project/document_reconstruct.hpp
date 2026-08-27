#pragma once

#include <bloom/document/color_settings.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/project/document_decode.hpp>

#include <cstdint>
#include <memory>
#include <optional>

// Reconstructs a live bloom::document::Document from a bloom::project::DecodedDocumentEnvelope (see
// document_decode.hpp): the R3 slice named in docs/architecture/project-format.md's "Canonical
// Document Shape", "Inclusive Allocator State", and "Opaque Extension Envelope" sections that
// document_decode.hpp's own file comment defers -- restoring the id allocator and applying every
// document-construction invariant beyond decode's wire-shape and within-composition
// cross-reference checks.
//
// reconstructDocument() never adds, bypasses, or weakens a document-model surface: every decoded
// value is installed through the exact checked adder the live model already exposes --
// bloom::document::CanonicalGraph::addNode()/addEdge()/addLayerOutput(),
// bloom::document::LayerStack::append(), bloom::document::ParameterStore::insert(),
// bloom::document::AnimationCurveStore::insert(), bloom::document::Project::addComposition()/
// addExtensionRecord(), bloom::document::Project::validate(), and finally
// bloom::document::Document's two-argument constructor, which is the sole place that checks the
// contract's inclusive-watermark rule: every decoded id must already be covered by its namespace's
// persisted high water (see "Inclusive Allocator State" -- deleted and undone allocations stay
// permanently consumed). Any rejection from one of those surfaces is reported as a typed
// ReconstructionRejected value naming the offending record by id; it is never silently downgraded,
// retried, or patched around by hand-rolling a parallel check in this module.
namespace bloom::project {

// Coarse stage identifying which checked document-model surface rejected reconstruction. Ordered to
// match the reconstruction walk: per-composition graph assembly, per-composition store assembly,
// composition admission, extension admission, whole-project validation, and finally Document
// construction (the inclusive-watermark check).
enum class ReconstructionStage : std::uint8_t {
    // Reserved zero value for a default-constructed (never-failed) ReconstructionRejected; never
    // returned by reconstructDocument() itself.
    None,
    // bloom::document::CanonicalGraph::addNode() rejected a decoded node.
    GraphNode,
    // bloom::document::CanonicalGraph::addEdge() rejected a decoded edge.
    GraphEdge,
    // bloom::document::CanonicalGraph::addLayerOutput() rejected a decoded Layer Output boundary.
    LayerOutput,
    // bloom::document::LayerStack::append() rejected a decoded Layer Stack entry (for example a
    // duplicate slot or layer id within one composition's stack).
    LayerStackEntry,
    // bloom::document::ParameterStore::insert() rejected a decoded parameter.
    ParameterStore,
    // bloom::document::AnimationCurveStore::insert() rejected a decoded animation curve.
    AnimationCurveStore,
    // bloom::document::Project::addComposition() rejected the fully assembled composition (for
    // example an invalid human-facing name).
    CompositionAdd,
    // bloom::document::Project::addExtensionRecord() rejected a decoded extension record (for
    // example a duplicate id -- unreachable given decode's own sort/uniqueness check, but still a
    // typed possibility of that checked surface).
    ExtensionRecordAdd,
    // bloom::document::Project::validate() reported at least one issue after every composition and
    // extension record was added: a whole-project or cross-composition invariant (schema/role
    // agreement, orphan extension subject or host-table target, cross-composition id collision, an
    // unowned or multiply-owned animation curve, ...) that a single checked adder cannot see in
    // isolation.
    ProjectValidate,
    // bloom::document::Document's two-argument constructor rejected the validated project against
    // the decoded idAllocation.highestIssued state: at least one declared id is not covered by its
    // namespace's persisted high water.
    DocumentConstruct,
};

// Names the offending record by id where the failing stage is scoped to one. `compositionId` is
// invalid (zero) for a stage that is not composition-scoped (CompositionAdd names the composition
// being added through compositionId itself; ExtensionRecordAdd/ProjectValidate/DocumentConstruct
// are project-wide and leave both ids zero). `recordId` holds the raw value of whichever typed id
// the stage's own checked adder was given -- a ParameterId, AnimationCurveId, NodeId, EdgeId,
// LayerId, or LayerSlotId depending on `stage` -- and is zero when not applicable. Exact JSON
// member paths are intentionally not reproduced here: reconstruction operates on already-decoded
// values, and the decode-time DocumentDecodeResult path remains the place that names JSON
// structure.
struct ReconstructionRejected final {
    ReconstructionStage stage = ReconstructionStage::None;
    document::CompositionId compositionId;
    std::uint64_t recordId = 0;

    friend bool operator==(const ReconstructionRejected&,
                           const ReconstructionRejected&) noexcept = default;
};

// A successfully reconstructed document plus its caller-owned color settings, mirroring
// CanonicalDocumentV1's split: bloom::document::Project does not itself own color settings (see
// canonical_document.hpp), so the decoded value travels alongside the live Document rather than
// inside it.
struct ReconstructedDocument final {
    std::unique_ptr<document::Document> document;
    document::ColorSettings colorSettings;
};

// [[nodiscard]] failure-aware result, mirroring DocumentDecodeResult's value()/error() shape
// (document_decode.hpp): value() returns a pointer, null on failure, and is deleted on an rvalue
// receiver so a caller can never hold a pointer into a temporary about to be destroyed. Document is
// move-only (see document.hpp), so this result type is itself move-only; a caller that wants to
// keep the reconstructed Document past the result's own lifetime moves it out of value()->document.
class [[nodiscard]] ReconstructDocumentResult final {
  public:
    ReconstructDocumentResult(ReconstructDocumentResult&&) noexcept = default;
    ReconstructDocumentResult& operator=(ReconstructDocumentResult&&) noexcept = default;
    ReconstructDocumentResult(const ReconstructDocumentResult&) = delete;
    ReconstructDocumentResult& operator=(const ReconstructDocumentResult&) = delete;
    ~ReconstructDocumentResult() = default;

    [[nodiscard]] static ReconstructDocumentResult success(ReconstructedDocument result);
    [[nodiscard]] static ReconstructDocumentResult failure(ReconstructionRejected rejection);

    [[nodiscard]] explicit operator bool() const noexcept { return succeeded_; }
    [[nodiscard]] ReconstructionRejected rejection() const noexcept { return rejection_; }

    [[nodiscard]] ReconstructedDocument* value() & noexcept {
        return result_.has_value() ? &*result_ : nullptr;
    }
    [[nodiscard]] const ReconstructedDocument* value() const& noexcept {
        return result_.has_value() ? &*result_ : nullptr;
    }
    [[nodiscard]] const ReconstructedDocument* value() const&& = delete;

  private:
    ReconstructDocumentResult() = default;

    bool succeeded_ = false;
    std::optional<ReconstructedDocument> result_;
    ReconstructionRejected rejection_;
};

// Reconstructs a live Document from `envelope` (moved from, so nothing is retained past return on
// success or failure). See the file-level comment above for the exact checked surfaces this walks
// through and the ordering ReconstructionStage documents. May throw std::bad_alloc; every
// document-model rejection is reported through the returned result rather than thrown.
[[nodiscard]] ReconstructDocumentResult reconstructDocument(DecodedDocumentEnvelope envelope);

} // namespace bloom::project
