#pragma once

#include <bloom/document/schema_version.hpp>
#include <bloom/project/project_io_memory.hpp>
#include <bloom/project/strict_json_dom.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

// Sequential DOM-to-DOM document migration (see docs/architecture/project-format.md, "Versions,
// Migrations, And Preservation": "supported older versions migrate through sequential
// deterministic DOM migrations before trusted document decoding").
//
// Representation: StrictJsonDomDocument (strict_json_dom.hpp) is an immutable parsed tree, so a
// step cannot transform it in place -- doing bytes-to-bytes at the raw JSON level would also lose
// the strict-parse guarantee for whatever a step produces. Instead, one MigrationStep reads the
// current parsed DOM (read-only) and writes brand-new canonical JSON bytes for the *next* schema
// version's document.json shape, using the same canonical writer primitives document.json's own
// encoder uses (canonical_json_writer.hpp, unknown_json_number.hpp); migrateDocumentDom() then
// strict-re-parses those bytes through parseStrictJsonDom() before handing the result to the next
// step, or to the caller. Re-parsing between every step keeps every intermediate stage behind the
// exact same strict gate real document.json bytes must pass, and makes the whole chain
// byte-checkable for determinism -- two runs of the same steps over the same input produce
// byte-identical intermediate and final JSON. For the short window each step runs in, both the DOM
// being read and the bytes being written (and, once re-parsed, the next DOM) are simultaneously
// live and both charge the caller's ProjectIoOperationMemory budget, exactly like every other
// Project I/O representation swap (see docs/architecture/project-format.md, "Resource Limits":
// migration is a named charged operation whose resident footprint includes "canonical output").
//
// Document DOM only: manifest.json has no migration step of its own in this slice. Manifest
// round-trip capture is already called out as later work in manifest_decode.hpp's own file
// comment; a manifest migration analogue (mirroring this module one layer up) would naturally
// follow that capture work, not precede it, since a manifest step would need the same DOM-in/
// bytes-out shape this module already establishes.
//
// Registry: kProductionDocumentMigrationSteps below is the real production table -- currently
// empty, because schema {1,0} is the only version Bloom has ever shipped. document_decode.hpp's
// own gates (UnsupportedMajorVersion via DomainViolation for an unrecognized major; the
// newer-minor RT1 capture/PreservationRequired route for a same-major newer minor) already reject
// or redirect everything that is not exactly {1,0}, without this module ever being consulted for
// either case -- see save_archive.cpp's runReopenChain(), the one production call site, for the
// exact routing condition that keeps this module out of both paths. migrateDocumentDom() itself is
// fully generic and injectable over both the step table and the "current" version it migrates to,
// which is what lets document_migration_tests.cpp prove the chaining/failure/determinism machinery
// end-to-end with a synthetic version pair no production schema uses, without needing a production
// seam of its own.
//
// Version detection is not this module's job: the caller already lexically reads the document
// root's schemaVersion before trusted decode, as part of its own existing version-agreement check
// (see save_archive.cpp's documentRootVersion()); this module only consumes that already-detected
// value through its `detectedVersion` parameter.

namespace bloom::project {

// One migration step's outcome: either the step wrote complete canonical bytes into its output
// buffer, or it reports a typed failure with a bounded diagnostic path into the DOM it was reading
// (mirroring StrictJsonDomPathText / DocumentDecodePathText's shape and truncation contract).
enum class MigrationStepError : std::uint8_t {
    None,
    // The transform did not recognize the DOM shape it was given: a member it required was
    // missing, held the wrong JSON kind, or held a value outside the domain this exact step knows
    // how to migrate.
    TransformFailed,
};

// A bounded diagnostic path, formatted as `/key/0/key`, mirroring every other decode-layer path
// text type in this codebase (StrictJsonDomPathText, DocumentDecodePathText,
// ManifestDecodePathText). Deliberately a fresh local type rather than a reused one: this module
// sits below save_archive.hpp's composition boundary (which is what defines SaveArchiveErrorPath),
// exactly as document_decode.hpp and manifest_decode.hpp each keep their own.
class MigrationPathText final {
  public:
    constexpr MigrationPathText() noexcept = default;

    [[nodiscard]] static MigrationPathText from(std::string_view text) noexcept;

    [[nodiscard]] std::string_view view() const& noexcept { return {chars_.data(), size_}; }
    [[nodiscard]] std::string_view view() const&& = delete;
    [[nodiscard]] bool truncated() const noexcept { return truncated_; }

  private:
    std::array<char, 512> chars_{};
    std::uint16_t size_ = 0;
    bool truncated_ = false;
};

class [[nodiscard]] MigrationStepOutcome final {
  public:
    [[nodiscard]] static MigrationStepOutcome success() noexcept;
    [[nodiscard]] static MigrationStepOutcome failure(std::string_view path) noexcept;

    [[nodiscard]] explicit operator bool() const noexcept {
        return error_ == MigrationStepError::None;
    }
    [[nodiscard]] MigrationStepError error() const noexcept { return error_; }
    [[nodiscard]] std::string_view path() const noexcept { return path_.view(); }

  private:
    MigrationStepError error_ = MigrationStepError::None;
    MigrationPathText path_;
};

// One step's deterministic DOM-in/bytes-out transform. `root` is the source version's parsed
// document root (read-only; the DOM is immutable and this signature enforces that -- see the file
// comment above). `resource` is bound to the caller's ProjectIoOperationMemory budget for the
// duration of this call; every allocation the transform makes through it (including `output`'s own
// growth) is charged automatically, exactly like every other Project I/O PMR use in this codebase
// (e.g. save_archive.cpp's own ProjectIoMemoryResource use for its re-encode stages). The transform
// writes the target version's complete canonical document.json bytes into `output` (which starts
// empty, already bound to `resource`) and returns success(), or leaves `output` in an unspecified
// state and returns failure() with a path into `root`. May throw std::bad_alloc on budget
// rejection, mirroring decodeDocumentEnvelope()'s own throwing contract; migrateDocumentDom() is
// the termination-free boundary that catches it.
using MigrationStepTransform = MigrationStepOutcome (*)(const JsonValue& root,
                                                        std::pmr::memory_resource* resource,
                                                        std::pmr::vector<char>& output);

// One registered step: the exact {major, minor} it consumes, the exact next version it produces
// (migrateDocumentDom() requires this to be either the chain's final `currentVersion` or another
// registered step's sourceVersion -- see MigrationError::ChainGap below), and its transform.
// Deliberately a trivial aggregate (no user-declared constructor) so a registry can be a
// compile-time constexpr array -- see kProductionDocumentMigrationSteps.
struct MigrationStepDescriptor final {
    document::SchemaVersion sourceVersion;
    document::SchemaVersion targetVersion;
    MigrationStepTransform transform = nullptr;
};

// v1 ships no real migration steps -- see this file's own top comment. Kept as the fixed
// production table so the registration mechanism (a std::span<const MigrationStepDescriptor>
// parameter on migrateDocumentDom(), never a compiled-in global) has exactly one production
// caller, matching its real future shape once a schema bump adds the first real step.
inline constexpr std::array<MigrationStepDescriptor, 0> kProductionDocumentMigrationSteps{};

enum class MigrationOutcome : std::uint8_t {
    // detectedVersion == currentVersion: no step ran, and this result owns no DOM. The caller must
    // keep using its own already-parsed DOM instead -- see migratedRoot()'s own contract. This
    // pins byte identity for the already-current, by far the overwhelmingly common, case: nothing
    // is re-encoded or re-parsed.
    Identity,
    // One or more steps ran; migratedRoot() names the final re-parsed DOM.
    Migrated,
    // A typed framework error occurred; error()/stepsApplied()/failedStepSourceVersion()/
    // failedStepTargetVersion()/path() are valid, and, only for StepEmittedInvalidJson,
    // reparseError()/reparseByteOffset() are too.
    Failed,
};

enum class MigrationError : std::uint8_t {
    None,
    // detectedVersion is neither currentVersion nor any registered step's sourceVersion: there is
    // no chain that could ever reach currentVersion from here.
    UnknownSourceVersion,
    // A step's targetVersion was reached but is neither currentVersion nor any registered step's
    // sourceVersion: the chain has a hole between two otherwise-valid steps.
    ChainGap,
    // A step's transform() returned MigrationStepError::TransformFailed.
    StepTransformFailed,
    // A step wrote bytes that failed the mandatory strict re-parse -- the framework catching its
    // own step's bug rather than trusting step output blindly (see this file's top comment).
    StepEmittedInvalidJson,
    // A budget reservation or PMR allocation was rejected while running a step or re-parsing its
    // output.
    ResourceExhausted,
};

// [[nodiscard]] move-only result (owns a StrictJsonDomResult -- and therefore, transitively, a
// live PMR resource -- on Migrated, matching StrictJsonDomDocument's own move-only, non-copyable
// resource ownership).
class [[nodiscard]] MigrationResult final {
  public:
    MigrationResult(MigrationResult&&) noexcept = default;
    MigrationResult& operator=(MigrationResult&&) noexcept = default;
    MigrationResult(const MigrationResult&) = delete;
    MigrationResult& operator=(const MigrationResult&) = delete;
    ~MigrationResult() = default;

    [[nodiscard]] static MigrationResult identity() noexcept;
    [[nodiscard]] static MigrationResult migrated(StrictJsonDomResult finalParse,
                                                  std::uint32_t stepsApplied) noexcept;
    [[nodiscard]] static MigrationResult
    failure(MigrationError error, std::uint32_t stepsApplied,
            document::SchemaVersion failedStepSourceVersion,
            document::SchemaVersion failedStepTargetVersion, std::string_view path,
            StrictJsonDomError reparseError = StrictJsonDomError::None,
            std::size_t reparseByteOffset = 0) noexcept;

    [[nodiscard]] explicit operator bool() const noexcept {
        return outcome_ != MigrationOutcome::Failed;
    }
    [[nodiscard]] MigrationOutcome outcome() const noexcept { return outcome_; }
    // Valid for every outcome: 0 for Identity, the count of steps that actually ran for Migrated,
    // and the count that ran before the failing step (0 if the very first lookup failed) for
    // Failed.
    [[nodiscard]] std::uint32_t stepsApplied() const noexcept { return stepsApplied_; }
    // Non-null only when outcome() == Migrated. Identity callers must keep using their own
    // already-parsed DOM (see MigrationOutcome::Identity); Failed callers have no DOM at all.
    [[nodiscard]] const JsonValue* migratedRoot() const& noexcept {
        return (outcome_ == MigrationOutcome::Migrated && parse_.has_value())
                   ? &parse_->document()->root()
                   : nullptr;
    }
    [[nodiscard]] const JsonValue* migratedRoot() const&& = delete;

    [[nodiscard]] MigrationError error() const noexcept { return error_; }
    [[nodiscard]] document::SchemaVersion failedStepSourceVersion() const noexcept {
        return failedStepSourceVersion_;
    }
    [[nodiscard]] document::SchemaVersion failedStepTargetVersion() const noexcept {
        return failedStepTargetVersion_;
    }
    [[nodiscard]] std::string_view path() const noexcept { return path_.view(); }
    [[nodiscard]] StrictJsonDomError reparseError() const noexcept { return reparseError_; }
    [[nodiscard]] std::size_t reparseByteOffset() const noexcept { return reparseByteOffset_; }

  private:
    MigrationResult() = default;

    MigrationOutcome outcome_ = MigrationOutcome::Failed;
    std::uint32_t stepsApplied_ = 0;
    std::optional<StrictJsonDomResult> parse_;
    MigrationError error_ = MigrationError::None;
    document::SchemaVersion failedStepSourceVersion_;
    document::SchemaVersion failedStepTargetVersion_;
    MigrationPathText path_;
    StrictJsonDomError reparseError_ = StrictJsonDomError::None;
    std::size_t reparseByteOffset_ = 0;
};

// Applies `steps` sequentially from `detectedVersion` to `currentVersion` against `initialRoot`
// (an already strict-parsed, but not yet trusted-decoded, document DOM root). `currentVersion` and
// `steps` are both explicit parameters -- rather than fixed constants -- specifically so tests can
// exercise the full chaining/failure/determinism machinery with a synthetic version pair no
// production schema uses; the one production caller instead passes
// kCanonicalDocumentSchemaVersionV1 and kProductionDocumentMigrationSteps (see save_archive.cpp's
// runReopenChain()). `reparseLimits`/`operation` bound and charge every re-parse exactly like the
// caller's own initial parseStrictJsonDom() call. Never throws.
[[nodiscard]] MigrationResult migrateDocumentDom(
    const JsonValue& initialRoot, document::SchemaVersion detectedVersion,
    document::SchemaVersion currentVersion, std::span<const MigrationStepDescriptor> steps,
    const StrictJsonDomLimits& reparseLimits, const ProjectIoOperationMemory& operation) noexcept;

} // namespace bloom::project
