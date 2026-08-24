#pragma once

#include <bloom/document/project.hpp>
#include <bloom/document/validation.hpp>

#include <compare>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>

namespace bloom::document {

class Revision final {
  public:
    constexpr Revision() noexcept = default;
    [[nodiscard]] static constexpr Revision fromRaw(std::uint64_t value) noexcept {
        return Revision(value);
    }

    [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
    friend constexpr auto operator<=>(const Revision&, const Revision&) noexcept = default;

  private:
    explicit constexpr Revision(std::uint64_t value) noexcept : value_(value) {}
    std::uint64_t value_ = 0;
};

namespace detail {
struct DocumentIdentity;
struct DocumentState;
} // namespace detail

class DocumentProvenanceError final : public std::invalid_argument {
  public:
    using std::invalid_argument::invalid_argument;
};

class Snapshot final {
  public:
    Snapshot(const Snapshot&) noexcept = default;
    Snapshot(Snapshot&&) noexcept = default;
    Snapshot& operator=(const Snapshot&) noexcept = default;
    Snapshot& operator=(Snapshot&&) noexcept = default;
    ~Snapshot() = default;

    [[nodiscard]] Revision revision() const noexcept { return revision_; }
    [[nodiscard]] const Project& project() const noexcept;
    [[nodiscard]] const IdAllocator& ids() const noexcept;

  private:
    friend class Document;
    Snapshot(Revision revision, std::shared_ptr<const detail::DocumentIdentity> identity,
             std::shared_ptr<const detail::DocumentState> state) noexcept
        : revision_(revision), identity_(std::move(identity)), state_(std::move(state)) {}

    Revision revision_;
    std::shared_ptr<const detail::DocumentIdentity> identity_;
    std::shared_ptr<const detail::DocumentState> state_;
};

class Draft final {
  public:
    Draft(const Draft&) = delete;
    Draft& operator=(const Draft&) = delete;
    Draft(Draft&&) noexcept;
    Draft& operator=(Draft&&) noexcept;
    ~Draft();

    [[nodiscard]] const Project& project() const noexcept;
    [[nodiscard]] Project& project() noexcept;
    [[nodiscard]] const IdAllocator& ids() const noexcept;
    [[nodiscard]] IdAllocator& ids() noexcept;
    [[nodiscard]] ValidationResult validate() const;

  private:
    friend class Document;
    Draft(Revision baseRevision, std::shared_ptr<const detail::DocumentIdentity> identity,
          std::unique_ptr<detail::DocumentState> state) noexcept;

    Revision baseRevision_;
    std::shared_ptr<const detail::DocumentIdentity> identity_;
    std::unique_ptr<detail::DocumentState> state_;
};

enum class CommitStatus {
    Committed,
    RevisionConflict,
    InvalidDraft,
    RevisionOverflow,
    ForeignDocument,
    DraftBaseMismatch,
};

struct CommitResult {
    CommitStatus status;
    std::optional<Snapshot> snapshot;
    ValidationResult validation;

    [[nodiscard]] bool committed() const noexcept { return status == CommitStatus::Committed; }
};

class Document final {
  public:
    explicit Document(Project initialProject);
    Document(Project initialProject, IdAllocatorHighWater persistedHighWater);
    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;
    Document(Document&&) = delete;
    Document& operator=(Document&&) = delete;
    ~Document();

    [[nodiscard]] Snapshot snapshot() const;
    [[nodiscard]] Draft draft(const Snapshot& base) const;
    [[nodiscard]] CommitResult commit(Revision expectedRevision, Draft&& draft);
    [[nodiscard]] CommitResult restore(Revision expectedRevision,
                                       const Snapshot& historicalSnapshot);

  private:
    mutable std::mutex mutex_;
    std::shared_ptr<const detail::DocumentIdentity> identity_;
    Revision revision_;
    std::shared_ptr<const detail::DocumentState> state_;
};

} // namespace bloom::document
