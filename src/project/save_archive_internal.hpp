#ifndef BLOOM_PROJECT_SAVE_ARCHIVE_INTERNAL_HPP
#define BLOOM_PROJECT_SAVE_ARCHIVE_INTERNAL_HPP

#include <bloom/project/canonical_document.hpp>
#include <bloom/project/canonical_manifest.hpp>
#include <bloom/project/project_io_memory.hpp>
#include <bloom/project/project_io_memory_resource.hpp>
#include <bloom/project/save_archive.hpp>
#include <bloom/project/zip_container_writer.hpp>

#include <cstddef>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <vector>

// Shared build-without-verify implementation behind buildSaveArchive(), buildVerifiedSaveArchive()
// (both save_archive.cpp) and stageSaveArchive() (staged_save.cpp). Not a public Project I/O
// contract: only these two translation units include this header, matching the existing
// document_decode_internal.hpp / canonical_json_string_detail.hpp precedent for a private
// cross-file implementation seam within one CMake target.
//
// buildSaveArchive() only needs the finished archive bytes and discards everything else.
// buildVerifiedSaveArchive() and stageSaveArchive() both additionally need the exact manifest.json
// and document.json entry bytes that were encoded, to build a SaveArchiveExpectedContent for
// verifySaveArchive() -- stageSaveArchive() runs that verification over the staged/read-back
// bytes instead of the in-memory archive, but it is the same expected-content shape either way. So
// this routine hands back the entry bytes alongside the archive rather than just the archive, and
// each of the three callers uses only what it needs.
namespace bloom::project::detail {

// The entry-byte pmr::vector<char> buffers below are backed by a heap-allocated
// ProjectIoMemoryResource at a stable address, mirroring ZipContainerArchive's own ownership
// pattern (project/include/bloom/project/zip_container_writer.hpp): a constructed pmr::vector
// remembers its originating memory_resource's address, so that resource must outlive every use of
// the vectors and must never move once a vector is bound to it. A stack-local resource (as
// buildVerifiedSaveArchive used before this split) cannot outlive its constructing function, so a
// struct returned by value needs the resource on the heap instead.
struct SaveArchiveBuiltEntries final {
    std::unique_ptr<ProjectIoMemoryResource> resource;
    std::pmr::vector<char> manifestBytes;
    std::pmr::vector<char> documentBytes;

    [[nodiscard]] std::span<const std::byte> manifestByteSpan() const noexcept;
    [[nodiscard]] std::span<const std::byte> documentByteSpan() const noexcept;
};

// Exactly one of {archive, failure} is engaged. `entries` is engaged if and only if `archive` is
// (they are produced together on success and never separately).
struct SaveArchiveBuildOutcome final {
    std::optional<ZipContainerWriteResult> archive;
    std::optional<SaveArchiveBuiltEntries> entries;
    std::optional<SaveArchiveFailure> failure;

    [[nodiscard]] explicit operator bool() const noexcept { return archive.has_value(); }

    // Every accessor below re-checks its own optional immediately before using it, even though
    // every caller is only meant to reach it after its own operator bool() check: this keeps every
    // access statically guarded (never an unchecked std::optional dereference) and fails closed --
    // via std::terminate(), matching this codebase's existing precedent for a provably-impossible
    // internal invariant break (see LinuxSharedState::releaseActiveTarget() in
    // staged_artifact_linux.cpp) -- rather than silently reading an empty optional if the
    // {archive, entries} XOR {failure} invariant above were ever violated by a future edit.
    [[nodiscard]] SaveArchiveFailure takeFailure() && noexcept;
    [[nodiscard]] ZipContainerWriteResult takeArchive() && noexcept;
    [[nodiscard]] std::span<const std::byte> archiveBytes() const& noexcept;
    [[nodiscard]] SaveArchiveExpectedContent
    expectedContent(document::SchemaVersion documentSchemaVersion) const& noexcept;
};

// Encodes manifest.json and document.json and writes the two-entry Constrained ZIP Profile
// container from their bytes; never reopens, decodes, reconstructs, or re-encodes anything (that
// is verifySaveArchive()'s job, run by callers that want it against whichever bytes they hold).
// Charges every allocation through `operation`, exactly as buildVerifiedSaveArchive() did before
// this split. Never throws.
[[nodiscard]] SaveArchiveBuildOutcome
buildSaveArchiveEntries(const CanonicalManifestV1& manifest, const CanonicalDocumentV1& document,
                        const SaveArchiveLimits& limits,
                        ProjectIoOperationMemory operation) noexcept;

} // namespace bloom::project::detail

#endif // BLOOM_PROJECT_SAVE_ARCHIVE_INTERNAL_HPP
