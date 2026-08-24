#include <bloom/document/document.hpp>

#include <limits>
#include <stdexcept>
#include <utility>

namespace bloom::document::detail {

struct DocumentIdentity final {};

// Central inventory for allocator-backed IDs in durable project truth. When a new durable
// collection or allocator namespace is added, extend this walk and the publication inventory test.
static void reserveProjectIds(IdAllocator& ids, const Project& project) noexcept {
    for (const auto& composition : project.compositions()) {
        ids.reserveExisting(composition.id());
        for (const auto& parameter : composition.parameters().records()) {
            ids.reserveExisting(parameter.id);
            if (const auto* animation = std::get_if<AnimationCurveSource>(&parameter.source)) {
                ids.reserveExisting(animation->curveId);
            } else if (const auto* driver = std::get_if<DriverBindingSource>(&parameter.source)) {
                ids.reserveExisting(driver->driverId);
            }
        }
        for (const auto& node : composition.graph().nodes()) {
            ids.reserveExisting(node.id);
        }
        for (const auto& edge : composition.graph().edges()) {
            ids.reserveExisting(edge.id);
        }
        for (const auto& boundary : composition.graph().layerOutputs()) {
            ids.reserveExisting(boundary.layerId);
        }
        for (const auto& entry : composition.graph().layerStack().entries()) {
            ids.reserveExisting(entry.slotId);
            ids.reserveExisting(entry.layerId);
        }
    }
}

struct DocumentState {
    explicit DocumentState(Project projectValue) : project(std::move(projectValue)) {
        reserveProjectIds(ids, project);
    }

    Project project;
    IdAllocator ids;
};

} // namespace bloom::document::detail

namespace {

using bloom::document::CommitResult;
using bloom::document::CommitStatus;
using bloom::document::Revision;

[[nodiscard]] CommitResult conflictResult() {
    return {CommitStatus::RevisionConflict, std::nullopt, {}};
}

[[nodiscard]] CommitResult overflowResult() {
    return {CommitStatus::RevisionOverflow, std::nullopt, {}};
}

[[nodiscard]] CommitResult provenanceResult(std::string path, std::string message) {
    bloom::document::ValidationResult validation;
    validation.add(bloom::document::ValidationCode::ForeignDocument, std::move(path),
                   std::move(message));
    return {CommitStatus::ForeignDocument, std::nullopt, std::move(validation)};
}

[[nodiscard]] CommitResult baseMismatchResult() {
    bloom::document::ValidationResult validation;
    validation.add(bloom::document::ValidationCode::RevisionMismatch, "draft.baseRevision",
                   "Draft base revision does not match the expected revision");
    return {CommitStatus::DraftBaseMismatch, std::nullopt, std::move(validation)};
}

[[nodiscard]] std::optional<Revision> nextRevision(const Revision current) noexcept {
    if (current.value() == std::numeric_limits<std::uint64_t>::max()) {
        return std::nullopt;
    }
    return Revision::fromRaw(current.value() + 1);
}

} // namespace

namespace bloom::document {

const Project& Snapshot::project() const noexcept { return state_->project; }

const IdAllocator& Snapshot::ids() const noexcept { return state_->ids; }

Draft::Draft(Revision baseRevision, std::shared_ptr<const detail::DocumentIdentity> identity,
             std::unique_ptr<detail::DocumentState> state) noexcept
    : baseRevision_(baseRevision), identity_(std::move(identity)), state_(std::move(state)) {}

Draft::Draft(Draft&&) noexcept = default;
Draft& Draft::operator=(Draft&&) noexcept = default;
Draft::~Draft() = default;

const Project& Draft::project() const noexcept { return state_->project; }
Project& Draft::project() noexcept { return state_->project; }
const IdAllocator& Draft::ids() const noexcept { return state_->ids; }
IdAllocator& Draft::ids() noexcept { return state_->ids; }
ValidationResult Draft::validate() const { return state_->project.validate(); }

Document::Document(Project initialProject) {
    auto initialState = std::make_shared<detail::DocumentState>(std::move(initialProject));
    const auto validation = initialState->project.validate();
    if (!validation.ok()) {
        throw std::invalid_argument("Initial Bloom document project is invalid");
    }
    identity_ = std::make_shared<detail::DocumentIdentity>();
    state_ = std::move(initialState);
}

Document::~Document() = default;

Snapshot Document::snapshot() const {
    const std::scoped_lock lock(mutex_);
    return Snapshot(revision_, identity_, state_);
}

Draft Document::draft(const Snapshot& base) const {
    if (base.state_ == nullptr) {
        throw std::invalid_argument("Cannot draft from an empty snapshot");
    }
    if (base.identity_ != identity_) {
        throw DocumentProvenanceError("Cannot draft a snapshot owned by another document");
    }
    return Draft(base.revision_, identity_, std::make_unique<detail::DocumentState>(*base.state_));
}

CommitResult Document::commit(const Revision expectedRevision, Draft&& draftValue) {
    if (draftValue.identity_ != identity_) {
        return provenanceResult("draft", "Draft is owned by another document");
    }
    if (draftValue.baseRevision_ != expectedRevision) {
        return baseMismatchResult();
    }
    {
        const std::scoped_lock lock(mutex_);
        if (expectedRevision != revision_) {
            return conflictResult();
        }
    }

    if (draftValue.state_ == nullptr) {
        ValidationResult validation;
        validation.add(ValidationCode::InvalidValue, "draft", "Draft has no document state");
        return {CommitStatus::InvalidDraft, std::nullopt, std::move(validation)};
    }
    auto validation = draftValue.validate();
    if (!validation.ok()) {
        return {CommitStatus::InvalidDraft, std::nullopt, std::move(validation)};
    }

    const std::scoped_lock lock(mutex_);
    if (expectedRevision != revision_) {
        return conflictResult();
    }
    const auto publishedRevision = nextRevision(revision_);
    if (!publishedRevision.has_value()) {
        return overflowResult();
    }

    draftValue.state_->ids.mergeHighWater(state_->ids);
    detail::reserveProjectIds(draftValue.state_->ids, draftValue.state_->project);
    state_ = std::make_shared<detail::DocumentState>(std::move(*draftValue.state_));
    draftValue.state_.reset();
    revision_ = *publishedRevision;
    return {CommitStatus::Committed, Snapshot(revision_, identity_, state_), {}};
}

CommitResult Document::restore(const Revision expectedRevision,
                               const Snapshot& historicalSnapshot) {
    if (historicalSnapshot.identity_ != identity_) {
        return provenanceResult("snapshot", "Snapshot is owned by another document");
    }
    {
        const std::scoped_lock lock(mutex_);
        if (expectedRevision != revision_) {
            return conflictResult();
        }
    }

    if (historicalSnapshot.state_ == nullptr) {
        ValidationResult validation;
        validation.add(ValidationCode::InvalidValue, "snapshot",
                       "Historical snapshot has no document state");
        return {CommitStatus::InvalidDraft, std::nullopt, std::move(validation)};
    }
    const auto validation = historicalSnapshot.project().validate();
    if (!validation.ok()) {
        return {CommitStatus::InvalidDraft, std::nullopt, validation};
    }

    const std::scoped_lock lock(mutex_);
    if (expectedRevision != revision_) {
        return conflictResult();
    }
    const auto publishedRevision = nextRevision(revision_);
    if (!publishedRevision.has_value()) {
        return overflowResult();
    }

    auto restoredState = std::make_shared<detail::DocumentState>(*historicalSnapshot.state_);
    restoredState->ids.mergeHighWater(state_->ids);
    detail::reserveProjectIds(restoredState->ids, restoredState->project);
    state_ = std::move(restoredState);
    revision_ = *publishedRevision;
    return {CommitStatus::Committed, Snapshot(revision_, identity_, state_), {}};
}

} // namespace bloom::document
