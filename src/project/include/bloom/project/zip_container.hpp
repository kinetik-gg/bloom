#pragma once

#include <bloom/project/project_io_memory.hpp>
#include <bloom/project/project_io_memory_resource.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <vector>

// The bounded Constrained ZIP Profile container reader, per
// docs/architecture/project-format.md ("Constrained ZIP Profile" + "Resource Limits").
// `readZipContainer()` first runs a raw, allocation-free preflight scan
// (`bloom::project::detail::preflightZipContainer()`) that proves the archive bytes are a
// conforming exactly-two-entry container before any qualified reader is invoked, then extracts
// both entries with a qualified libzip 1.11 reader confined to zip_container.cpp, independently
// re-verifies every libzip-reported fact against what the preflight established, and computes an
// independent CRC-32 with the qualified zlib `crc32()` over each fully expanded buffer. No libzip
// or zlib type, header, or macro appears here or in any other public Bloom contract. The reader
// parses no JSON: on success it hands back the two expanded entry byte buffers untouched.

namespace bloom::project {

namespace detail {
class ZipContainerBuilder;
} // namespace detail

// The v1 Constrained ZIP Profile resource limits table (docs/architecture/project-format.md,
// "Resource Limits"). Limits apply before allocation where the size is declared, and during
// streaming (expanded-size mismatch) otherwise.
struct ZipContainerLimits final {
    std::uint64_t maxArchiveBytes = 272629760;       // 260 MiB physical
    std::uint64_t maxManifestBytes = 1048576;        // 1 MiB expanded
    std::uint64_t maxDocumentBytes = 268435456;      // 256 MiB expanded
    std::uint64_t maxTotalExpandedBytes = 269484032; // 257 MiB expanded total
    std::uint64_t maxExpansionRatio = 1000;          // per entry, max(1, compressedSize)
};

enum class ZipContainerError : std::uint8_t {
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
    // The fully expanded byte count of an entry, measured while streaming it out of the qualified
    // reader, did not equal its declared uncompressed size -- including a stream that keeps
    // producing bytes past the declared size (a decompression bomb caught before it can overrun
    // the exactly-sized destination buffer, which is never given more capacity than declared).
    ExpandedSizeMismatch,
    CrcMismatch,
    SizeOverflow,
    ResourceExhausted,
    // The qualified libzip reader disagreed with a fact the raw preflight already established
    // (open failure after a passing preflight, a zip_stat_index mismatch, or a read that did not
    // behave as the preflight-derived entry size predicted). Never trusted blindly.
    QualifiedReaderDisagreement,
};

// A parsed container owning its PMR resource and both expanded entry buffers for as long as the
// document lives. The resource lives behind a stable heap address so moving the document never
// invalidates the buffers' allocator state (mirrors StrictJsonDomDocument).
class ZipContainerDocument final {
  public:
    ZipContainerDocument(const ZipContainerDocument&) = delete;
    ZipContainerDocument& operator=(const ZipContainerDocument&) = delete;
    ZipContainerDocument(ZipContainerDocument&&) noexcept = default;
    ZipContainerDocument& operator=(ZipContainerDocument&&) noexcept = default;
    ~ZipContainerDocument() = default;

    [[nodiscard]] std::span<const std::byte> manifestBytes() const noexcept {
        return {manifest_.data(), manifest_.size()};
    }
    [[nodiscard]] std::span<const std::byte> documentBytes() const noexcept {
        return {document_.data(), document_.size()};
    }

  private:
    friend class detail::ZipContainerBuilder;

    ZipContainerDocument(std::unique_ptr<ProjectIoMemoryResource> resource,
                         std::pmr::vector<std::byte> manifest,
                         std::pmr::vector<std::byte> document) noexcept;

    std::unique_ptr<ProjectIoMemoryResource> resource_;
    std::pmr::vector<std::byte> manifest_;
    std::pmr::vector<std::byte> document_;
};

// [[nodiscard]] failure-aware result. On success, document() names the extracted buffers; on
// failure, error() names the typed cause and byteOffset() names the offending archive byte
// position where meaningful (0 otherwise).
class [[nodiscard]] ZipContainerReadResult final {
  public:
    ZipContainerReadResult(const ZipContainerReadResult&) = delete;
    ZipContainerReadResult& operator=(const ZipContainerReadResult&) = delete;
    ZipContainerReadResult(ZipContainerReadResult&&) noexcept = default;
    ZipContainerReadResult& operator=(ZipContainerReadResult&&) noexcept = default;
    ~ZipContainerReadResult() = default;

    [[nodiscard]] explicit operator bool() const noexcept {
        return error_ == ZipContainerError::None;
    }
    [[nodiscard]] ZipContainerError error() const noexcept { return error_; }
    [[nodiscard]] std::size_t byteOffset() const noexcept { return byteOffset_; }
    [[nodiscard]] const ZipContainerDocument* document() const& noexcept {
        return document_.has_value() ? &*document_ : nullptr;
    }
    [[nodiscard]] const ZipContainerDocument* document() const&& = delete;

  private:
    friend class detail::ZipContainerBuilder;

    ZipContainerReadResult() noexcept = default;

    std::optional<ZipContainerDocument> document_;
    ZipContainerError error_ = ZipContainerError::InvalidLimits;
    std::size_t byteOffset_ = 0;
};

// Runs the bounded raw preflight against `archive` with `limits`, then extracts both entries with
// the qualified libzip/zlib pair, charging every allocation -- including a documented worst-case
// dependency-working-memory reservation held for the duration of the libzip calls -- through
// `operation`. Budget rejection is reported as ResourceExhausted rather than throwing or falling
// back to unmetered heap allocation. `archive` must outlive neither the call nor any live libzip
// handle it creates internally; no reference to it survives the call. Never throws.
[[nodiscard]] ZipContainerReadResult readZipContainer(std::span<const std::byte> archive,
                                                      const ZipContainerLimits& limits,
                                                      ProjectIoOperationMemory operation) noexcept;

} // namespace bloom::project
