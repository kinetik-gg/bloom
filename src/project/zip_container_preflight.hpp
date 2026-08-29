#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace bloom::project::detail {

// Mirrors bloom::project::ZipContainerLimits exactly; kept as an independent type (rather than
// reusing the public struct) so this raw scanner has no dependency on the public header, matching
// strict_json_preflight.hpp's separation from strict_json_dom.hpp.
struct ZipContainerPreflightLimits final {
    std::uint64_t maxArchiveBytes = 272629760;
    std::uint64_t maxManifestBytes = 1048576;
    std::uint64_t maxDocumentBytes = 268435456;
    std::uint64_t maxTotalExpandedBytes = 269484032;
    std::uint64_t maxExpansionRatio = 1000;
};

// A strict subset of bloom::project::ZipContainerError: every value here has a exact translation
// in the public enum; the public enum additionally has extraction-stage-only values
// (ExpandedSizeMismatch, CrcMismatch, ResourceExhausted, QualifiedReaderDisagreement) that this
// allocation-free, libzip-free scanner can never produce.
enum class ZipContainerPreflightError : std::uint8_t {
    None,
    InvalidLimits,
    ArchiveTooLarge,
    ArchiveTruncated,
    MissingOrMalformedEocd,
    ArchiveCommentForbidden,
    MultiDiskForbidden,
    WrongEntryCount,
    WrongEntryName,
    WrongEntryOrder,
    Utf8FlagMissing,
    ForbiddenGeneralPurposeFlag,
    UnsupportedCompressionMethod,
    ExtraFieldForbidden,
    EntryCommentForbidden,
    Zip64Forbidden,
    LocalCentralHeaderDisagreement,
    OverlappingOrUnaccountedByteRange,
    NonRegularEntry,
    ExecutableEntry,
    ExpandedSizeLimitExceeded,
    ExpansionRatioExceeded,
    StoredSizeMismatch,
    SizeOverflow,
};

// The exact byte range and declared metadata the preflight established for one entry, handed to
// the extraction stage so it can charge the destination buffer and cross-check every fact libzip
// reports rather than trusting it blindly.
struct ZipContainerPreflightEntry final {
    std::uint64_t localHeaderOffset = 0;
    std::uint64_t dataOffset = 0;
    std::uint32_t compressedSize = 0;
    std::uint32_t uncompressedSize = 0;
    std::uint32_t crc32Value = 0;
    std::uint16_t method = 0;
};

struct ZipContainerPreflightResult final {
    ZipContainerPreflightError error = ZipContainerPreflightError::InvalidLimits;
    std::size_t byteOffset = 0;
    ZipContainerPreflightEntry manifest{};
    ZipContainerPreflightEntry document{};

    [[nodiscard]] constexpr bool succeeded() const noexcept {
        return error == ZipContainerPreflightError::None;
    }
    [[nodiscard]] explicit constexpr operator bool() const noexcept { return succeeded(); }
};

// Performs only strict, allocation-free structural validation of the Constrained ZIP Profile
// (docs/architecture/project-format.md, "Constrained ZIP Profile" + "Resource Limits") against
// raw archive bytes: no libzip call, no decompression, and no entry payload byte is inspected
// (only header fields). Success proves an exact, non-overlapping, fully-accounted byte layout for
// exactly two regular-file entries named "manifest.json" then "document.json" and returns their
// declared metadata; it is not itself a trusted extraction. The scanner allocates nothing and
// retains no reference to `archive` past the call.
[[nodiscard]] ZipContainerPreflightResult
preflightZipContainer(std::span<const std::byte> archive,
                      const ZipContainerPreflightLimits& limits) noexcept;

} // namespace bloom::project::detail
