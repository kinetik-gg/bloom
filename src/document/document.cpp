#include <bloom/document/document.hpp>

#include <limits>
#include <stdexcept>
#include <utility>

namespace bloom::document::detail {

struct DocumentIdentity final {};

// Central inventory for allocator-backed IDs in durable project truth. When a new durable
// collection or allocator namespace is added, extend this walk and the publication inventory test.
template <typename Visitor>
[[nodiscard]] static bool visitProjectIds(const Project& project, Visitor&& visitor) noexcept {
    for (const auto& folder : project.assetFolders())
        if (!visitor(folder.id))
            return false;
    for (const auto& asset : project.assets()) {
        if (!visitor(asset.id))
            return false;
    }
    for (const auto& record : project.extensionRecords()) {
        if (!visitor(record.id)) {
            return false;
        }
    }
    for (const auto& block : project.typedDataBlocks()) {
        const auto* id = std::get_if<DataBlockRecordId>(&block.id);
        if (id != nullptr && !visitor(*id))
            return false;
    }
    for (const auto& composition : project.compositions()) {
        if (!visitor(composition.id())) {
            return false;
        }
        for (const auto& parameter : composition.parameters().records()) {
            if (!visitor(parameter.id)) {
                return false;
            }
            // A driver source carries a NodeId now (task S7), and that node is already visited by
            // the graph walk below -- so there is nothing to visit here. A driver binding holds no
            // id of its own any more; see DriverBindingSource in parameter.hpp for why the durable
            // record is the node-and-port pair rather than an id into a driver table.
        }
        for (const auto& record : composition.animationCurves().records()) {
            if (!visitor(animationCurveId(record))) {
                return false;
            }
            if (const auto* scalar = std::get_if<ScalarAnimationCurve>(&record)) {
                for (const auto& keyframe : scalar->keyframes) {
                    if (!visitor(keyframe.id))
                        return false;
                }
            } else {
                const auto visitComponents = [&](const auto& curve) {
                    for (const auto& component : curve.components)
                        for (const auto& keyframe : component.keyframes)
                            if (!visitor(keyframe.id))
                                return false;
                    return true;
                };
                if (const auto* vector = std::get_if<Vec2AnimationCurve>(&record)) {
                    if (!visitComponents(*vector))
                        return false;
                } else if (const auto* vector3 = std::get_if<Vec3AnimationCurve>(&record)) {
                    if (!visitComponents(*vector3))
                        return false;
                } else if (const auto* color = std::get_if<Color4AnimationCurve>(&record)) {
                    if (!visitComponents(*color))
                        return false;
                }
            }
        }
        for (const auto& node : composition.graph().nodes()) {
            if (!visitor(node.id)) {
                return false;
            }
        }
        for (const auto& edge : composition.graph().edges()) {
            if (!visitor(edge.id)) {
                return false;
            }
        }
        for (const auto& boundary : composition.graph().layerOutputs()) {
            if (!visitor(boundary.layerId)) {
                return false;
            }
        }
        for (const auto& stack : composition.graph().merges())
            for (const auto& entry : stack.entries()) {
                if (!visitor(entry.slotId) ||
                    (entry.layerId.isValid() && !visitor(entry.layerId))) {
                    return false;
                }
            }
        for (const auto& [groupId, group] : composition.nodeGroups()) {
            if (!visitor(groupId)) {
                return false;
            }
        }
    }
    return true;
}

static void reserveProjectIds(IdAllocator& ids, const Project& project) noexcept {
    [[maybe_unused]] const auto visited = visitProjectIds(project, [&ids](const auto id) {
        ids.reserveExisting(id);
        return true;
    });
}

[[nodiscard]] static bool projectIdsAreCovered(const IdAllocator& ids,
                                               const Project& project) noexcept {
    return visitProjectIds(project, [&ids](const auto id) { return ids.covers(id); });
}

struct DocumentState {
    explicit DocumentState(Project projectValue) : project(std::move(projectValue)) {
        reserveProjectIds(ids, project);
        rootByteCharge = project.rootByteCharge();
    }

    DocumentState(Project projectValue, const IdAllocatorHighWater persistedHighWater)
        : project(std::move(projectValue)), ids(IdAllocator::fromHighWater(persistedHighWater)) {
        rootByteCharge = project.rootByteCharge();
    }

    Project project;
    IdAllocator ids;
    std::size_t rootByteCharge = 0;
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
std::size_t Snapshot::rootByteCharge() const noexcept { return state_->rootByteCharge; }

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
std::size_t Draft::rootByteCharge() const { return state_->project.rootByteCharge(); }
ValidationResult Draft::validate() const { return state_->project.validate(); }

// Consume moved input; copy the publication shell to isolate escaped authored pointers.
// NOLINTNEXTLINE(performance-unnecessary-value-param)
Document::Document(Project initialProject) {
    auto initialState = std::make_shared<detail::DocumentState>(initialProject);
    initialState->project.freezeCompositionContent();
    const auto validation = initialState->project.validate();
    if (!validation.ok()) {
        throw std::invalid_argument("Initial Bloom document project is invalid");
    }
    identity_ = std::make_shared<detail::DocumentIdentity>();
    state_ = std::move(initialState);
}

// Consume moved input; copy the publication shell to isolate escaped authored pointers.
// NOLINTNEXTLINE(performance-unnecessary-value-param)
Document::Document(Project initialProject, const IdAllocatorHighWater persistedHighWater) {
    auto initialState = std::make_shared<detail::DocumentState>(initialProject, persistedHighWater);
    initialState->project.freezeCompositionContent();
    const auto validation = initialState->project.validate();
    if (!validation.ok()) {
        throw std::invalid_argument("Initial Bloom document project is invalid");
    }
    if (!detail::projectIdsAreCovered(initialState->ids, initialState->project)) {
        throw std::invalid_argument(
            "Persisted Bloom allocator state does not cover every declared project ID");
    }
    identity_ = std::make_shared<detail::DocumentIdentity>();
    state_ = std::move(initialState);
}

Document::~Document() = default;

Document::Work Document::work() const noexcept {
    return {.drafts = drafts_.load(std::memory_order_relaxed),
            .validations = validations_.load(std::memory_order_relaxed),
            .publications = publications_.load(std::memory_order_relaxed),
            .restores = restores_.load(std::memory_order_relaxed),
            .observerCallbacks = observerCallbacks_.load(std::memory_order_relaxed)};
}

DocumentObserverId Document::addObserver(DocumentObserver observer) {
    if (!observer) {
        return 0;
    }
    const std::scoped_lock lock(mutex_);
    if (nextObserverId_ == 0) {
        return 0;
    }
    const auto id = nextObserverId_++;
    if (nextObserverId_ == 0) {
        nextObserverId_ = 0;
    }
    observers_.emplace_back(id, std::move(observer));
    return id;
}

void Document::removeObserver(const DocumentObserverId observerId) noexcept {
    if (observerId == 0) {
        return;
    }
    const std::scoped_lock lock(mutex_);
    std::erase_if(observers_,
                  [observerId](const auto& entry) { return entry.first == observerId; });
}

void Document::notify(const DocumentEvent& event) const noexcept {
    std::vector<DocumentObserver> callbacks;
    try {
        const std::scoped_lock lock(mutex_);
        callbacks.reserve(observers_.size());
        for (const auto& [id, observer] : observers_) {
            static_cast<void>(id);
            callbacks.push_back(observer);
        }
    } catch (...) {
        return;
    }
    for (const auto& callback : callbacks) {
        observerCallbacks_.fetch_add(1, std::memory_order_relaxed);
        try {
            callback(event);
        } catch (...) {
            // Observers are diagnostic seams. A faulty observer must never change document truth.
            static_cast<void>(0);
        }
    }
}

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
    auto state = std::make_unique<detail::DocumentState>(*base.state_);
    drafts_.fetch_add(1, std::memory_order_relaxed);
    return Draft(base.revision_, identity_, std::move(state));
}

CommitResult Document::commit(const Revision expectedRevision, Draft&& draftValue) {
    const auto reject = [this, expectedRevision](CommitResult result) {
        const auto currentRevision = snapshot().revision();
        notify({.kind = DocumentEventKind::Rejected,
                .status = result.status,
                .beforeRevision = expectedRevision,
                .afterRevision = currentRevision});
        return result;
    };
    if (draftValue.identity_ != identity_) {
        return reject(provenanceResult("draft", "Draft is owned by another document"));
    }
    if (draftValue.baseRevision_ != expectedRevision) {
        return reject(baseMismatchResult());
    }
    if (draftValue.state_ == nullptr) {
        ValidationResult validation;
        validation.add(ValidationCode::InvalidValue, "draft", "Draft has no document state");
        return reject({CommitStatus::InvalidDraft, std::nullopt, std::move(validation)});
    }
    validations_.fetch_add(1, std::memory_order_relaxed);
    auto validation = draftValue.validate();
    if (!validation.ok()) {
        return reject({CommitStatus::InvalidDraft, std::nullopt, std::move(validation)});
    }

    // A Draft exposes mutable Project/Composition references. Publish a separate shell and
    // freeze the shell's private copy of every edited payload before taking the commit lock;
    // moving the Draft itself would let a retained reference mutate an immutable Snapshot.
    auto publishedState = std::make_shared<detail::DocumentState>(*draftValue.state_);
    publishedState->project.freezeCompositionContent();
    publishedState->rootByteCharge = publishedState->project.rootByteCharge();

    std::unique_lock lock(mutex_);
    if (expectedRevision != revision_) {
        lock.unlock();
        return reject(conflictResult());
    }
    const auto publishedRevision = nextRevision(revision_);
    if (!publishedRevision.has_value()) {
        lock.unlock();
        return reject(overflowResult());
    }

    publishedState->ids.mergeHighWater(state_->ids);
    detail::reserveProjectIds(publishedState->ids, publishedState->project);
    state_ = std::move(publishedState);
    // Leave the caller's Draft independent and alive until its own destructor. A reference kept
    // while editing remains valid, but it can only mutate the unpublished draft copy.
    revision_ = *publishedRevision;
    publications_.fetch_add(1, std::memory_order_relaxed);
    auto result = CommitResult{CommitStatus::Committed, Snapshot(revision_, identity_, state_), {}};
    // The lock is deliberately released before callbacks run. `notify()` takes the same mutex to
    // snapshot the observer list and observers are allowed to read the new snapshot.
    lock.unlock();
    notify({.kind = DocumentEventKind::RevisionChanged,
            .status = result.status,
            .beforeRevision = expectedRevision,
            .afterRevision = result.snapshot->revision()});
    return result;
}

CommitResult Document::restore(const Revision expectedRevision,
                               const Snapshot& historicalSnapshot) {
    const auto reject = [this, expectedRevision](CommitResult result) {
        const auto currentRevision = snapshot().revision();
        notify({.kind = DocumentEventKind::Rejected,
                .status = result.status,
                .beforeRevision = expectedRevision,
                .afterRevision = currentRevision});
        return result;
    };
    if (historicalSnapshot.identity_ != identity_) {
        return reject(provenanceResult("snapshot", "Snapshot is owned by another document"));
    }
    if (historicalSnapshot.state_ == nullptr) {
        ValidationResult validation;
        validation.add(ValidationCode::InvalidValue, "snapshot",
                       "Historical snapshot has no document state");
        return reject({CommitStatus::InvalidDraft, std::nullopt, std::move(validation)});
    }
    // Only Document mints Snapshots, from validated construction or a validated commit.
    // Their frozen state cannot change after publication, so undo/redo need no second project scan.

    std::unique_lock lock(mutex_);
    if (expectedRevision != revision_) {
        lock.unlock();
        return reject(conflictResult());
    }
    const auto publishedRevision = nextRevision(revision_);
    if (!publishedRevision.has_value()) {
        lock.unlock();
        return reject(overflowResult());
    }

    auto restoredState = std::make_shared<detail::DocumentState>(*historicalSnapshot.state_);
    restoredState->ids.mergeHighWater(state_->ids);
    detail::reserveProjectIds(restoredState->ids, restoredState->project);
    state_ = std::move(restoredState);
    revision_ = *publishedRevision;
    publications_.fetch_add(1, std::memory_order_relaxed);
    restores_.fetch_add(1, std::memory_order_relaxed);
    auto result = CommitResult{CommitStatus::Committed, Snapshot(revision_, identity_, state_), {}};
    lock.unlock();
    notify({.kind = DocumentEventKind::RevisionChanged,
            .status = result.status,
            .beforeRevision = expectedRevision,
            .afterRevision = result.snapshot->revision()});
    return result;
}

} // namespace bloom::document
