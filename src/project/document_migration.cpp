#include <bloom/project/document_migration.hpp>

#include <bloom/project/project_io_memory_resource.hpp>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <new>
#include <utility>

namespace {

using bloom::project::JsonValue;
using bloom::project::MigrationStepDescriptor;
using bloom::project::StrictJsonDomError;

[[nodiscard]] std::span<const std::byte> asBytes(const std::span<const char> bytes) noexcept {
    return {reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()};
}

// Linear scan is intentional and sufficient: even a large future production table stays small
// (one entry per shipped minor), and this only ever runs once per migration step, never per JSON
// value.
[[nodiscard]] const MigrationStepDescriptor*
findStep(const std::span<const MigrationStepDescriptor> steps,
         const bloom::document::SchemaVersion version) noexcept {
    for (const auto& candidate : steps) {
        if (candidate.sourceVersion == version) {
            return &candidate;
        }
    }
    return nullptr;
}

} // namespace

namespace bloom::project {

MigrationPathText MigrationPathText::from(const std::string_view text) noexcept {
    MigrationPathText result;
    const auto count = std::min(text.size(), result.chars_.size());
    std::memcpy(result.chars_.data(), text.data(), count);
    result.size_ = static_cast<std::uint16_t>(count);
    result.truncated_ = count != text.size();
    return result;
}

MigrationStepOutcome MigrationStepOutcome::success() noexcept { return MigrationStepOutcome{}; }

MigrationStepOutcome MigrationStepOutcome::failure(const std::string_view path) noexcept {
    MigrationStepOutcome result;
    result.error_ = MigrationStepError::TransformFailed;
    result.path_ = MigrationPathText::from(path);
    return result;
}

MigrationResult MigrationResult::identity() noexcept {
    MigrationResult result;
    result.outcome_ = MigrationOutcome::Identity;
    result.stepsApplied_ = 0;
    return result;
}

MigrationResult MigrationResult::migrated(StrictJsonDomResult finalParse,
                                          const std::uint32_t stepsApplied) noexcept {
    MigrationResult result;
    result.outcome_ = MigrationOutcome::Migrated;
    result.stepsApplied_ = stepsApplied;
    result.parse_.emplace(std::move(finalParse));
    return result;
}

MigrationResult MigrationResult::failure(const MigrationError error,
                                         const std::uint32_t stepsApplied,
                                         const document::SchemaVersion failedStepSourceVersion,
                                         const document::SchemaVersion failedStepTargetVersion,
                                         const std::string_view path,
                                         const StrictJsonDomError reparseError,
                                         const std::size_t reparseByteOffset) noexcept {
    MigrationResult result;
    result.outcome_ = MigrationOutcome::Failed;
    result.stepsApplied_ = stepsApplied;
    result.error_ = error;
    result.failedStepSourceVersion_ = failedStepSourceVersion;
    result.failedStepTargetVersion_ = failedStepTargetVersion;
    result.path_ = MigrationPathText::from(path);
    result.reparseError_ = reparseError;
    result.reparseByteOffset_ = reparseByteOffset;
    return result;
}

// See this file's header comment for the overall representation/re-parse contract. Each iteration
// below: (1) looks up the step registered for the version reached so far -- a miss is
// UnknownSourceVersion if no step has run yet, otherwise ChainGap; (2) runs that step's transform
// against the current DOM into a budget-charged output buffer; (3) strict-re-parses the step's
// output, which both re-validates it against the exact same gate real document.json bytes pass
// AND becomes the input DOM for the next iteration (or the final result). `currentParse` -- not
// `initialRoot` -- therefore owns every representation this function itself produces; the very
// first step still reads `initialRoot` (owned by the caller), never copying or re-parsing it
// redundantly before that first step has something to say about it.
MigrationResult migrateDocumentDom(const JsonValue& initialRoot,
                                   const document::SchemaVersion detectedVersion,
                                   const document::SchemaVersion currentVersion,
                                   const std::span<const MigrationStepDescriptor> steps,
                                   const StrictJsonDomLimits& reparseLimits,
                                   const ProjectIoOperationMemory& operation) noexcept {
    if (detectedVersion == currentVersion) {
        return MigrationResult::identity();
    }

    std::optional<StrictJsonDomResult> currentParse;
    const JsonValue* currentRoot = &initialRoot;
    auto version = detectedVersion;
    std::uint32_t stepsApplied = 0;

    while (version != currentVersion) {
        const auto* step = findStep(steps, version);
        if (step == nullptr) {
            return MigrationResult::failure(
                stepsApplied == 0 ? MigrationError::UnknownSourceVersion : MigrationError::ChainGap,
                stepsApplied, version, currentVersion, std::string_view{});
        }

        try {
            // Bound to `operation` exactly like every other transient Project I/O representation
            // (e.g. verifySaveArchive()'s own reencodeResource): this step's freshly-written bytes
            // and the DOM it read from both stay charged for as long as both are simultaneously
            // live, satisfying the "two live representations charge both" budget contract.
            ProjectIoMemoryResource stepResource(operation);
            std::pmr::vector<char> output(&stepResource);
            const auto stepOutcome = step->transform(*currentRoot, &stepResource, output);
            if (!stepOutcome) {
                return MigrationResult::failure(MigrationError::StepTransformFailed, stepsApplied,
                                                step->sourceVersion, step->targetVersion,
                                                stepOutcome.path());
            }

            auto reparsed = parseStrictJsonDom(asBytes(output), reparseLimits, operation);
            if (!reparsed) {
                // A budget rejection during re-parse is ResourceExhausted, not
                // StepEmittedInvalidJson: the bytes were never actually judged invalid, the parse
                // just could not run to completion under the current budget.
                const auto mappedError = reparsed.error() == StrictJsonDomError::ResourceExhausted
                                             ? MigrationError::ResourceExhausted
                                             : MigrationError::StepEmittedInvalidJson;
                return MigrationResult::failure(mappedError, stepsApplied, step->sourceVersion,
                                                step->targetVersion, reparsed.memberPath(),
                                                reparsed.error(), reparsed.byteOffset());
            }

            // The previous currentParse (if any) is destroyed here, releasing its charge, only
            // after the new one has been fully constructed and charged -- so the two
            // representations legitimately overlap for the assignment's duration rather than one
            // ever being freed early.
            currentParse.emplace(std::move(reparsed));
            currentRoot = &currentParse->document()->root();
            version = step->targetVersion;
            ++stepsApplied;
        } catch (const std::bad_alloc&) {
            return MigrationResult::failure(MigrationError::ResourceExhausted, stepsApplied,
                                            step->sourceVersion, step->targetVersion,
                                            std::string_view{});
        }
    }

    if (!currentParse.has_value()) {
        // Unreachable: the loop only exits when `version == currentVersion`, and `version` only
        // ever changes inside the loop body after a successful step has populated `currentParse`.
        // detectedVersion == currentVersion was already handled as Identity above, so reaching
        // here with no step ever having run is a framework invariant break, not a legitimate
        // outcome -- reported rather than trusted blindly, matching this codebase's existing
        // "should not occur" precedent (see StrictJsonDomError::ParseFailed's own comment).
        return MigrationResult::failure(MigrationError::ChainGap, stepsApplied, detectedVersion,
                                        currentVersion, std::string_view{});
    }
    return MigrationResult::migrated(std::move(*currentParse), stepsApplied);
}

} // namespace bloom::project
