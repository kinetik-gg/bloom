#include <bloom/project/zip_container.hpp>

#include "zip_container_preflight.hpp"

#include <zip.h>
#include <zlib.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <new>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace bloom::project {

ZipContainerDocument::ZipContainerDocument(std::unique_ptr<ProjectIoMemoryResource> resource,
                                           std::pmr::vector<std::byte> manifest,
                                           std::pmr::vector<std::byte> document) noexcept
    : resource_(std::move(resource)), manifest_(std::move(manifest)),
      document_(std::move(document)) {}

} // namespace bloom::project

namespace bloom::project::detail {

namespace {

[[nodiscard]] ZipContainerError
translatePreflightError(const ZipContainerPreflightError error) noexcept {
    switch (error) {
    case ZipContainerPreflightError::None:
        return ZipContainerError::None;
    case ZipContainerPreflightError::InvalidLimits:
        return ZipContainerError::InvalidLimits;
    case ZipContainerPreflightError::ArchiveTooLarge:
        return ZipContainerError::ArchiveTooLarge;
    case ZipContainerPreflightError::ArchiveTruncated:
        return ZipContainerError::ArchiveTruncated;
    case ZipContainerPreflightError::MissingOrMalformedEocd:
        return ZipContainerError::MissingOrMalformedEocd;
    case ZipContainerPreflightError::ArchiveCommentForbidden:
        return ZipContainerError::ArchiveCommentForbidden;
    case ZipContainerPreflightError::MultiDiskForbidden:
        return ZipContainerError::MultiDiskForbidden;
    case ZipContainerPreflightError::WrongEntryCount:
        return ZipContainerError::WrongEntryCount;
    case ZipContainerPreflightError::WrongEntryName:
        return ZipContainerError::WrongEntryName;
    case ZipContainerPreflightError::WrongEntryOrder:
        return ZipContainerError::WrongEntryOrder;
    case ZipContainerPreflightError::Utf8FlagMissing:
        return ZipContainerError::Utf8FlagMissing;
    case ZipContainerPreflightError::ForbiddenGeneralPurposeFlag:
        return ZipContainerError::ForbiddenGeneralPurposeFlag;
    case ZipContainerPreflightError::UnsupportedCompressionMethod:
        return ZipContainerError::UnsupportedCompressionMethod;
    case ZipContainerPreflightError::ExtraFieldForbidden:
        return ZipContainerError::ExtraFieldForbidden;
    case ZipContainerPreflightError::EntryCommentForbidden:
        return ZipContainerError::EntryCommentForbidden;
    case ZipContainerPreflightError::Zip64Forbidden:
        return ZipContainerError::Zip64Forbidden;
    case ZipContainerPreflightError::LocalCentralHeaderDisagreement:
        return ZipContainerError::LocalCentralHeaderDisagreement;
    case ZipContainerPreflightError::OverlappingOrUnaccountedByteRange:
        return ZipContainerError::OverlappingOrUnaccountedByteRange;
    case ZipContainerPreflightError::NonRegularEntry:
        return ZipContainerError::NonRegularEntry;
    case ZipContainerPreflightError::ExecutableEntry:
        return ZipContainerError::ExecutableEntry;
    case ZipContainerPreflightError::ExpandedSizeLimitExceeded:
        return ZipContainerError::ExpandedSizeLimitExceeded;
    case ZipContainerPreflightError::ExpansionRatioExceeded:
        return ZipContainerError::ExpansionRatioExceeded;
    case ZipContainerPreflightError::StoredSizeMismatch:
        return ZipContainerError::StoredSizeMismatch;
    case ZipContainerPreflightError::SizeOverflow:
        return ZipContainerError::SizeOverflow;
    }
    return ZipContainerError::QualifiedReaderDisagreement;
}

// Conservative worst-case reservation for libzip/zlib's own working memory while opening the
// source and extracting the two entries: zlib's raw-deflate inflate window is bounded at 32 KiB
// (max windowBits 15), and libzip additionally keeps its zip_t/zip_source_t bookkeeping, a
// zip_stat_t per entry, and a modest internal buffer per open zip_file_t -- all well under a few
// hundred KiB for a strictly two-entry archive. 4 MiB stays generous across libzip point releases
// without charging a meaningful fraction of the default 1 GiB per-operation budget.
constexpr std::uint64_t kQualifiedReaderWorkingReservationBytes = 4ULL << 20U;

[[nodiscard]] bool statMatches(const zip_stat_t& stat, const ZipContainerPreflightEntry& entry,
                               const std::string_view expectedName) noexcept {
    constexpr auto kRequiredFields =
        ZIP_STAT_NAME | ZIP_STAT_SIZE | ZIP_STAT_COMP_SIZE | ZIP_STAT_CRC | ZIP_STAT_COMP_METHOD;
    if ((stat.valid & kRequiredFields) != kRequiredFields) {
        return false;
    }
    if (stat.name == nullptr || expectedName != std::string_view(stat.name)) {
        return false;
    }
    return stat.size == entry.uncompressedSize && stat.comp_size == entry.compressedSize &&
           stat.crc == entry.crc32Value &&
           static_cast<std::uint16_t>(stat.comp_method) == entry.method;
}

} // namespace

// The friend declared by bloom::project::zip_container.hpp names bloom::project::detail's own
// ZipContainerBuilder, so this class must live directly in this namespace rather than the
// anonymous namespace above (mirrors strict_json_dom.cpp's StrictJsonDomBuilder placement).
class ZipContainerBuilder final {
  public:
    [[nodiscard]] static ZipContainerReadResult read(std::span<const std::byte> archive,
                                                     const ZipContainerLimits& limits,
                                                     ProjectIoOperationMemory operation) noexcept;

  private:
    [[nodiscard]] static ZipContainerReadResult makeFailure(ZipContainerError error,
                                                            std::size_t byteOffset) noexcept;
    [[nodiscard]] static ZipContainerReadResult makeSuccess(ZipContainerDocument document) noexcept;

    // Not noexcept: sizing the destination buffer and reading into it are ordinary std::pmr::vector
    // operations bound to ProjectIoMemoryResource's throwing memory_resource surface. A budget
    // rejection here must propagate as std::bad_alloc to read()'s try/catch.
    [[nodiscard]] static bool extractEntry(zip_t* archiveHandle, zip_uint64_t index,
                                           const ZipContainerPreflightEntry& entry,
                                           std::string_view expectedName,
                                           std::pmr::vector<std::byte>& outBuffer,
                                           ZipContainerError& error);
};

bool ZipContainerBuilder::extractEntry(zip_t* const archiveHandle, const zip_uint64_t index,
                                       const ZipContainerPreflightEntry& entry,
                                       const std::string_view expectedName,
                                       std::pmr::vector<std::byte>& outBuffer,
                                       ZipContainerError& error) {
    outBuffer.resize(entry.uncompressedSize);

    zip_stat_t stat{};
    zip_stat_init(&stat);
    if (zip_stat_index(archiveHandle, index, 0, &stat) != 0 ||
        !statMatches(stat, entry, expectedName)) {
        error = ZipContainerError::QualifiedReaderDisagreement;
        return false;
    }

    zip_file_t* const file = zip_fopen_index(archiveHandle, index, 0);
    if (file == nullptr) {
        error = ZipContainerError::QualifiedReaderDisagreement;
        return false;
    }
    struct FileGuard final {
        zip_file_t* handle;
        ~FileGuard() { zip_fclose(handle); }
    } fileGuard{file};

    if (!outBuffer.empty()) {
        const auto readBytes = zip_fread(file, outBuffer.data(), outBuffer.size());
        if (readBytes < 0) {
            error = ZipContainerError::QualifiedReaderDisagreement;
            return false;
        }
        if (static_cast<std::uint64_t>(readBytes) != outBuffer.size()) {
            error = ZipContainerError::ExpandedSizeMismatch;
            return false;
        }
    }

    // A trailing probe read: the destination buffer is exactly the declared uncompressed size, so
    // it can never be written past regardless of how much decompressed data the stream actually
    // holds. A stream that keeps producing bytes past its declared size is caught here, not by an
    // out-of-bounds write. libzip verifies its own internal running CRC exactly when it detects
    // EOF, so a mismatched CRC surfaces as a failed *read* here (ZIP_ER_CRC) rather than as a
    // clean zero-byte EOF. That failure alone is never trusted as the verification (per contract,
    // libzip's internal check is welcome redundancy, not authoritative): it is treated as
    // equivalent to reaching EOF and deferred to the independent CRC-32 comparison below, which
    // is computed with the qualified zlib crc32_z() over the buffer this code fully controls and
    // is the actual source of any reported CrcMismatch. Any other negative probe read is a
    // genuine, unrelated qualified-reader disagreement.
    std::array<std::byte, 1> probe{};
    const auto probeRead = zip_fread(file, probe.data(), probe.size());
    if (probeRead < 0) {
        const auto* const fileError = zip_file_get_error(file);
        if (fileError == nullptr || zip_error_code_zip(fileError) != ZIP_ER_CRC) {
            error = ZipContainerError::QualifiedReaderDisagreement;
            return false;
        }
    } else if (probeRead != 0) {
        error = ZipContainerError::ExpandedSizeMismatch;
        return false;
    }

    const auto computedCrc =
        crc32_z(0L, reinterpret_cast<const Bytef*>(outBuffer.data()), outBuffer.size());
    if (static_cast<std::uint32_t>(computedCrc) != entry.crc32Value) {
        error = ZipContainerError::CrcMismatch;
        return false;
    }
    return true;
}

ZipContainerReadResult ZipContainerBuilder::read(const std::span<const std::byte> archive,
                                                 const ZipContainerLimits& limits,
                                                 ProjectIoOperationMemory operation) noexcept {
    if (limits.maxArchiveBytes == 0 || limits.maxManifestBytes == 0 ||
        limits.maxDocumentBytes == 0 || limits.maxTotalExpandedBytes == 0 ||
        limits.maxExpansionRatio == 0) {
        return makeFailure(ZipContainerError::InvalidLimits, 0);
    }

    const ZipContainerPreflightLimits preflightLimits{
        .maxArchiveBytes = limits.maxArchiveBytes,
        .maxManifestBytes = limits.maxManifestBytes,
        .maxDocumentBytes = limits.maxDocumentBytes,
        .maxTotalExpandedBytes = limits.maxTotalExpandedBytes,
        .maxExpansionRatio = limits.maxExpansionRatio,
    };
    const auto preflight = preflightZipContainer(archive, preflightLimits);
    if (!preflight.succeeded()) {
        return makeFailure(translatePreflightError(preflight.error), preflight.byteOffset);
    }

    try {
        // Reserve libzip/zlib's worst-case working memory before any dependency call, per the
        // contract that unmetered dependency allocation is not permitted; held for the duration
        // of every libzip call below and released by RAII on every return path.
        auto workingReservationResult = operation.reserve(kQualifiedReaderWorkingReservationBytes);
        if (!workingReservationResult) {
            return makeFailure(ZipContainerError::ResourceExhausted, 0);
        }
        auto workingReservation = std::move(workingReservationResult).takeReservation();

        std::unique_ptr<ProjectIoMemoryResource> resource;
        try {
            resource = std::make_unique<ProjectIoMemoryResource>(std::move(operation));
        } catch (const std::bad_alloc&) {
            return makeFailure(ZipContainerError::ResourceExhausted, 0);
        }

        std::pmr::vector<std::byte> manifestBuffer(resource.get());
        std::pmr::vector<std::byte> documentBuffer(resource.get());

        zip_error_t zipError{};
        zip_error_init(&zipError);
        zip_source_t* const source =
            zip_source_buffer_create(archive.data(), archive.size(), 0, &zipError);
        if (source == nullptr) {
            zip_error_fini(&zipError);
            return makeFailure(ZipContainerError::QualifiedReaderDisagreement, 0);
        }
        zip_t* const archiveHandle =
            zip_open_from_source(source, ZIP_RDONLY | ZIP_CHECKCONS, &zipError);
        zip_error_fini(&zipError);
        if (archiveHandle == nullptr) {
            // zip_open_from_source() did not take ownership on failure; the source must be freed
            // here or it leaks.
            zip_source_free(source);
            return makeFailure(ZipContainerError::QualifiedReaderDisagreement, 0);
        }
        // On success, libzip keeps its own reference to `source` and frees it when the archive is
        // closed or discarded; freeing it here as well would be a use of a resource libzip still
        // owns.

        struct ArchiveGuard final {
            zip_t* handle;
            ~ArchiveGuard() {
                if (handle != nullptr) {
                    zip_discard(handle);
                }
            }
        } archiveGuard{archiveHandle};

        if (zip_get_num_entries(archiveHandle, 0) != 2) {
            return makeFailure(ZipContainerError::QualifiedReaderDisagreement, 0);
        }

        ZipContainerError extractionError = ZipContainerError::None;
        if (!extractEntry(archiveHandle, 0, preflight.manifest, "manifest.json", manifestBuffer,
                          extractionError) ||
            !extractEntry(archiveHandle, 1, preflight.document, "document.json", documentBuffer,
                          extractionError)) {
            return makeFailure(extractionError, 0);
        }

        if (zip_close(archiveHandle) != 0) {
            // Every read already succeeded on a read-only archive; a close failure here is the
            // qualified reader disagreeing with its own established state. archiveGuard still owns
            // `handle` and discards it below.
            return makeFailure(ZipContainerError::QualifiedReaderDisagreement, 0);
        }
        // zip_close() already freed the handle on success; the guard must not discard it again.
        archiveGuard.handle = nullptr;

        ZipContainerDocument document(std::move(resource), std::move(manifestBuffer),
                                      std::move(documentBuffer));
        return makeSuccess(std::move(document));
    } catch (const std::bad_alloc&) {
        return makeFailure(ZipContainerError::ResourceExhausted, 0);
    } catch (...) {
        return makeFailure(ZipContainerError::QualifiedReaderDisagreement, 0);
    }
}

ZipContainerReadResult ZipContainerBuilder::makeFailure(const ZipContainerError error,
                                                        const std::size_t byteOffset) noexcept {
    ZipContainerReadResult result;
    result.error_ = error;
    result.byteOffset_ = byteOffset;
    return result;
}

ZipContainerReadResult ZipContainerBuilder::makeSuccess(ZipContainerDocument document) noexcept {
    ZipContainerReadResult result;
    result.error_ = ZipContainerError::None;
    result.document_.emplace(std::move(document));
    return result;
}

} // namespace bloom::project::detail

namespace bloom::project {

ZipContainerReadResult readZipContainer(const std::span<const std::byte> archive,
                                        const ZipContainerLimits& limits,
                                        ProjectIoOperationMemory operation) noexcept {
    return detail::ZipContainerBuilder::read(archive, limits, std::move(operation));
}

} // namespace bloom::project
