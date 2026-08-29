#include <bloom/project/zip_container_writer.hpp>

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <memory_resource>
#include <new>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace bloom::project {

ZipContainerArchive::ZipContainerArchive(std::unique_ptr<ProjectIoMemoryResource> resource,
                                         std::pmr::vector<std::byte> bytes) noexcept
    : resource_(std::move(resource)), bytes_(std::move(bytes)) {}

} // namespace bloom::project

namespace bloom::project::detail {

namespace {

constexpr std::uint32_t kLocalFileHeaderSignature = 0x04034b50U;
constexpr std::uint32_t kCentralDirectorySignature = 0x02014b50U;
constexpr std::uint32_t kEocdSignature = 0x06054b50U;
constexpr std::uint64_t kLocalHeaderFixedBytes = 30;
constexpr std::uint64_t kCentralHeaderFixedBytes = 46;
constexpr std::uint64_t kEocdFixedBytes = 22;
constexpr std::uint16_t kVersionNeeded = 20;
// Exactly bit 11 (UTF-8 names), the only general-purpose flag the reader's preflight accepts, set
// identically in every local and central header.
constexpr std::uint16_t kGeneralPurposeFlags = 0x0800U;
constexpr std::uint16_t kMethodStored = 0;
constexpr std::uint16_t kMethodDeflate = 8;
constexpr std::string_view kManifestName = "manifest.json";
constexpr std::string_view kDocumentName = "document.json";
// The largest value the 32-bit local/central-header size and offset fields can hold without a
// ZIP64 extension, which this profile never emits.
constexpr std::uint64_t kMaxZipFieldValue = std::numeric_limits<std::uint32_t>::max();

// version made by: high byte 3 (Unix host, matching the reader's unconditional Unix-mode
// attribute check), low byte the same version-needed spelling used everywhere else in this
// profile.
constexpr std::uint8_t kVersionMadeByHostUnix = 3;
constexpr std::uint16_t kVersionMadeBy = static_cast<std::uint16_t>(
    (static_cast<std::uint16_t>(kVersionMadeByHostUnix) << 8U) | kVersionNeeded);
static_assert(kVersionMadeBy == 0x0314U);

// External attributes: Unix mode 0644 (regular file, rw-r--r--, no executable bit) placed in the
// high 16 bits, with the low DOS-compatible byte left zero so the reader's unconditional
// DOS-directory-bit check never fires.
constexpr std::uint32_t kRegularFileMode0644 = 0100644U;
constexpr std::uint32_t kExternalAttributesRegularFile = kRegularFileMode0644 << 16U;
static_assert(kExternalAttributesRegularFile == 0x81A40000U);

// DOS epoch 1980-01-01 00:00:00 (docs/architecture/project-format.md, "Constrained ZIP Profile").
// DOS time packs hour(5 bits) | minute(6 bits) | second/2(5 bits); midnight is all zero bits.
constexpr std::uint16_t kDosModTime = 0x0000U;
// DOS date packs (year-1980)(7 bits) | month(4 bits) | day(5 bits), 1-indexed month/day.
constexpr std::uint16_t kDosEpochYearOffset = 0;
constexpr std::uint16_t kDosEpochMonth = 1;
constexpr std::uint16_t kDosEpochDay = 1;
constexpr std::uint16_t kDosModDate =
    static_cast<std::uint16_t>((kDosEpochYearOffset << 9U) | (kDosEpochMonth << 5U) | kDosEpochDay);
static_assert(kDosModDate == 0x0021U);

// Conservative worst-case reservation for zlib's own internal deflateInit2 working memory (raw
// deflate, windowBits magnitude 15, memLevel 8): the documented zlib formula
// (1 << (windowBits+2)) + (1 << (memLevel+9)) is ~256 KiB for these parameters; this stays
// generous across zlib point releases and platform allocator overhead without charging a
// meaningful fraction of the default 1 GiB per-operation budget. Held for the duration of every
// deflateInit2/deflate/deflateEnd call below (both entries compress sequentially, one stream
// live at a time, so a single reservation covers the whole operation).
constexpr std::uint64_t kQualifiedWriterWorkingReservationBytes = 4ULL << 20U;

[[nodiscard]] constexpr bool checkedAdd(const std::uint64_t left, const std::uint64_t right,
                                        std::uint64_t& result) noexcept {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        return false;
    }
    result = left + right;
    return true;
}

[[nodiscard]] constexpr bool checkedMul(const std::uint64_t left, const std::uint64_t right,
                                        std::uint64_t& result) noexcept {
    if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left) {
        return false;
    }
    result = left * right;
    return true;
}

[[nodiscard]] std::span<const std::byte> nameBytes(const std::string_view name) noexcept {
    return {reinterpret_cast<const std::byte*>(name.data()), name.size()};
}

// One entry's fully resolved on-disk representation: the method the stored-fallback rule chose,
// the payload's CRC-32 and true uncompressed size, and the exact bytes to write after the local
// header (the compressed stream, or the stored payload copy). `data` is also this function's
// intermediate deflate-attempt buffer: encodeEntry() first sizes it as the deflate destination,
// then either trims it in place (deflate kept) or replaces it with a stored copy (deflate
// discarded), so every byte it ever holds is charged through the caller's resource.
struct EncodedEntry final {
    explicit EncodedEntry(std::pmr::memory_resource* const resource) : data(resource) {}

    std::uint16_t method = kMethodStored;
    std::uint32_t crc32Value = 0;
    std::uint32_t uncompressedSize = 0;
    std::pmr::vector<std::byte> data;
};

// Appends fixed-width little-endian fields and raw byte ranges to a pre-sized destination buffer,
// checked against arithmetic overflow and the buffer's true size on every write (mirrors
// zip_container_preflight.cpp's Scanner::advance()). A `false` return means the precomputed
// layout and this writer's actual output disagree -- an internal invariant break, never a
// legitimate runtime condition -- and every caller treats it as a fail-closed SizeOverflow rather
// than writing past the buffer.
class ArchiveWriter final {
  public:
    explicit ArchiveWriter(std::pmr::vector<std::byte>& destination) noexcept
        : destination_(destination) {}

    [[nodiscard]] std::uint64_t cursor() const noexcept { return cursor_; }

    [[nodiscard]] bool putU16(const std::uint16_t value) noexcept {
        const std::array<std::byte, 2> bytes{static_cast<std::byte>(value & 0xFFU),
                                             static_cast<std::byte>((value >> 8U) & 0xFFU)};
        return putBytes(bytes);
    }

    [[nodiscard]] bool putU32(const std::uint32_t value) noexcept {
        const std::array<std::byte, 4> bytes{static_cast<std::byte>(value & 0xFFU),
                                             static_cast<std::byte>((value >> 8U) & 0xFFU),
                                             static_cast<std::byte>((value >> 16U) & 0xFFU),
                                             static_cast<std::byte>((value >> 24U) & 0xFFU)};
        return putBytes(bytes);
    }

    [[nodiscard]] bool putBytes(const std::span<const std::byte> bytes) noexcept {
        std::uint64_t next = 0;
        if (!checkedAdd(cursor_, bytes.size(), next) || next > destination_.size()) {
            return false;
        }
        std::copy(bytes.begin(), bytes.end(),
                  destination_.begin() + static_cast<std::ptrdiff_t>(cursor_));
        cursor_ = next;
        return true;
    }

  private:
    std::pmr::vector<std::byte>& destination_;
    std::uint64_t cursor_ = 0;
};

[[nodiscard]] bool writeLocalHeader(ArchiveWriter& writer, const std::string_view name,
                                    const EncodedEntry& entry) noexcept {
    return writer.putU32(kLocalFileHeaderSignature) && writer.putU16(kVersionNeeded) &&
           writer.putU16(kGeneralPurposeFlags) && writer.putU16(entry.method) &&
           writer.putU16(kDosModTime) && writer.putU16(kDosModDate) &&
           writer.putU32(entry.crc32Value) &&
           writer.putU32(static_cast<std::uint32_t>(entry.data.size())) &&
           writer.putU32(entry.uncompressedSize) &&
           writer.putU16(static_cast<std::uint16_t>(name.size())) && writer.putU16(0) &&
           writer.putBytes(nameBytes(name));
}

[[nodiscard]] bool writeCentralHeader(ArchiveWriter& writer, const std::string_view name,
                                      const EncodedEntry& entry,
                                      const std::uint32_t localHeaderOffset) noexcept {
    return writer.putU32(kCentralDirectorySignature) && writer.putU16(kVersionMadeBy) &&
           writer.putU16(kVersionNeeded) && writer.putU16(kGeneralPurposeFlags) &&
           writer.putU16(entry.method) && writer.putU16(kDosModTime) &&
           writer.putU16(kDosModDate) && writer.putU32(entry.crc32Value) &&
           writer.putU32(static_cast<std::uint32_t>(entry.data.size())) &&
           writer.putU32(entry.uncompressedSize) &&
           writer.putU16(static_cast<std::uint16_t>(name.size())) &&
           writer.putU16(0) /* extra length */ && writer.putU16(0) /* comment length */ &&
           writer.putU16(0) /* disk number start */ && writer.putU16(0) /* internal attrs */ &&
           writer.putU32(kExternalAttributesRegularFile) && writer.putU32(localHeaderOffset) &&
           writer.putBytes(nameBytes(name));
}

// Fixed archive overhead: two 30-byte local headers plus their exact names, two 46-byte central
// headers plus the same names (no extra field or comment anywhere), and one 22-byte EOCD with a
// zero-length comment.
[[nodiscard]] bool computeArchiveSize(const std::uint64_t manifestDataSize,
                                      const std::uint64_t documentDataSize,
                                      std::uint64_t& out) noexcept {
    std::uint64_t total = kEocdFixedBytes;
    std::uint64_t entryBytes = 0;
    if (!checkedAdd(kLocalHeaderFixedBytes, kManifestName.size(), entryBytes) ||
        !checkedAdd(total, entryBytes, total) || !checkedAdd(total, manifestDataSize, total)) {
        return false;
    }
    if (!checkedAdd(kLocalHeaderFixedBytes, kDocumentName.size(), entryBytes) ||
        !checkedAdd(total, entryBytes, total) || !checkedAdd(total, documentDataSize, total)) {
        return false;
    }
    if (!checkedAdd(kCentralHeaderFixedBytes, kManifestName.size(), entryBytes) ||
        !checkedAdd(total, entryBytes, total)) {
        return false;
    }
    if (!checkedAdd(kCentralHeaderFixedBytes, kDocumentName.size(), entryBytes) ||
        !checkedAdd(total, entryBytes, total)) {
        return false;
    }
    out = total;
    return true;
}

// Writes the complete archive -- manifest local header+data, document local header+data, central
// directory (manifest then document), EOCD with counts 2/2 -- into a buffer already sized to
// exactly computeArchiveSize()'s result. `false` means the precomputed size and the actual
// written byte count disagree (see ArchiveWriter's contract above).
[[nodiscard]] bool assembleArchive(const EncodedEntry& manifestEntry,
                                   const EncodedEntry& documentEntry,
                                   std::pmr::vector<std::byte>& archiveBytes) noexcept {
    ArchiveWriter writer(archiveBytes);
    const auto manifestLocalOffset = writer.cursor();
    if (!writeLocalHeader(writer, kManifestName, manifestEntry) ||
        !writer.putBytes(manifestEntry.data)) {
        return false;
    }
    const auto documentLocalOffset = writer.cursor();
    if (!writeLocalHeader(writer, kDocumentName, documentEntry) ||
        !writer.putBytes(documentEntry.data)) {
        return false;
    }
    if (manifestLocalOffset > kMaxZipFieldValue || documentLocalOffset > kMaxZipFieldValue) {
        return false; // unreachable: the caller already rejected archives above maxArchiveBytes.
    }

    const auto centralDirectoryOffset = writer.cursor();
    if (!writeCentralHeader(writer, kManifestName, manifestEntry,
                            static_cast<std::uint32_t>(manifestLocalOffset)) ||
        !writeCentralHeader(writer, kDocumentName, documentEntry,
                            static_cast<std::uint32_t>(documentLocalOffset))) {
        return false;
    }
    const auto centralDirectorySize = writer.cursor() - centralDirectoryOffset;
    if (centralDirectoryOffset > kMaxZipFieldValue || centralDirectorySize > kMaxZipFieldValue) {
        return false; // unreachable: same archive-size ceiling as above.
    }

    if (!writer.putU32(kEocdSignature) || !writer.putU16(0) /* disk number */ ||
        !writer.putU16(0) /* disk with central directory */ ||
        !writer.putU16(2) /* entries on this disk */ || !writer.putU16(2) /* entries total */ ||
        !writer.putU32(static_cast<std::uint32_t>(centralDirectorySize)) ||
        !writer.putU32(static_cast<std::uint32_t>(centralDirectoryOffset)) ||
        !writer.putU16(0) /* comment length */) {
        return false;
    }
    return writer.cursor() == archiveBytes.size();
}

} // namespace

// The friend declared by bloom::project::zip_container_writer.hpp names bloom::project::detail's
// own ZipContainerWriteBuilder, so this class must live directly in this namespace rather than the
// anonymous namespace above (mirrors zip_container.cpp's ZipContainerBuilder placement).
class ZipContainerWriteBuilder final {
  public:
    [[nodiscard]] static ZipContainerWriteResult write(std::span<const std::byte> manifest,
                                                       std::span<const std::byte> document,
                                                       const ZipContainerLimits& limits,
                                                       ProjectIoOperationMemory operation) noexcept;

  private:
    [[nodiscard]] static ZipContainerWriteResult
    makeFailure(ZipContainerWriteError error,
                ZipContainerWriteEntry entry = ZipContainerWriteEntry::None) noexcept;
    [[nodiscard]] static ZipContainerWriteResult makeSuccess(ZipContainerArchive archive) noexcept;

    // Not noexcept: resizing/assigning the PMR-backed intermediate and final buffers is ordinary
    // std::pmr::vector work bound to ProjectIoMemoryResource's throwing memory_resource surface.
    // A budget rejection here must propagate as std::bad_alloc to write()'s try/catch.
    [[nodiscard]] static bool encodeEntry(std::span<const std::byte> payload,
                                          std::uint64_t maxExpansionRatio, EncodedEntry& out,
                                          ZipContainerWriteError& error);
};

bool ZipContainerWriteBuilder::encodeEntry(const std::span<const std::byte> payload,
                                           const std::uint64_t maxExpansionRatio, EncodedEntry& out,
                                           ZipContainerWriteError& error) {
    out.uncompressedSize = static_cast<std::uint32_t>(payload.size());
    out.crc32Value = static_cast<std::uint32_t>(
        crc32_z(0L, reinterpret_cast<const Bytef*>(payload.data()), payload.size()));

    z_stream stream{};
    constexpr int kLevel = 6;
    constexpr int kWindowBits = -15; // negative: raw deflate, no zlib/gzip header or trailer.
    constexpr int kMemLevel = 8;
    if (deflateInit2(&stream, kLevel, Z_DEFLATED, kWindowBits, kMemLevel, Z_DEFAULT_STRATEGY) !=
        Z_OK) {
        error = ZipContainerWriteError::QualifiedCompressorFailure;
        return false;
    }
    struct StreamGuard final {
        z_stream* handle;
        ~StreamGuard() { deflateEnd(handle); }
    } streamGuard{&stream};

    out.data.resize(deflateBound(&stream, static_cast<uLong>(payload.size())));
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(payload.data()));
    stream.avail_in = static_cast<uInt>(payload.size());
    stream.next_out = reinterpret_cast<Bytef*>(out.data.data());
    stream.avail_out = static_cast<uInt>(out.data.size());
    if (deflate(&stream, Z_FINISH) != Z_STREAM_END) {
        error = ZipContainerWriteError::QualifiedCompressorFailure;
        return false;
    }
    const auto producedBytes = static_cast<std::uint64_t>(out.data.size() - stream.avail_out);

    // Stored fallback rule (docs/architecture/project-format.md, "Constrained ZIP Profile"):
    // store this entry instead of the deflate stream just produced when either
    //   (a) deflate did not reduce it (produced byte count >= payload byte count), or
    //   (b) the deflated form would exceed the reader's permitted expansion ratio
    //       (payloadSize > maxExpansionRatio * max(1, deflateSize)) -- the default reader would
    //       reject that entry outright (e.g. megabytes of a single repeated byte).
    // A stored entry always satisfies the reader: its ratio is exactly 1.
    bool useStore = producedBytes >= payload.size();
    if (!useStore) {
        const auto denominator = std::max<std::uint64_t>(1, producedBytes);
        std::uint64_t allowedMaximum = 0;
        if (checkedMul(maxExpansionRatio, denominator, allowedMaximum) &&
            payload.size() > allowedMaximum) {
            useStore = true;
        }
        // A checkedMul overflow leaves allowedMaximum conceptually unbounded for any payload this
        // writer can ever hold under the Resource Limits table, so deflate stays legal.
    }

    if (useStore) {
        out.method = kMethodStored;
        out.data.assign(payload.begin(), payload.end());
    } else {
        out.method = kMethodDeflate;
        out.data.resize(producedBytes);
    }
    return true;
}

ZipContainerWriteResult ZipContainerWriteBuilder::write(
    const std::span<const std::byte> manifest, const std::span<const std::byte> document,
    const ZipContainerLimits& limits, ProjectIoOperationMemory operation) noexcept {
    if (limits.maxArchiveBytes == 0 || limits.maxManifestBytes == 0 ||
        limits.maxDocumentBytes == 0 || limits.maxTotalExpandedBytes == 0 ||
        limits.maxExpansionRatio == 0) {
        return makeFailure(ZipContainerWriteError::InvalidLimits);
    }

    // Entry payload sizes must fit the 32-bit local/central header size fields; anything needing
    // a ZIP64 representation is refused. The Resource Limits table keeps this unreachable at the
    // default limits, but a caller-supplied limits value could raise the per-entry ceilings above
    // 4 GiB, so this check runs unconditionally, ahead of and independent from the policy limit
    // comparisons below.
    if (manifest.size() > kMaxZipFieldValue || document.size() > kMaxZipFieldValue) {
        return makeFailure(ZipContainerWriteError::SizeOverflow);
    }

    if (manifest.size() > limits.maxManifestBytes) {
        return makeFailure(ZipContainerWriteError::EntrySizeLimitExceeded,
                           ZipContainerWriteEntry::Manifest);
    }
    if (document.size() > limits.maxDocumentBytes) {
        return makeFailure(ZipContainerWriteError::EntrySizeLimitExceeded,
                           ZipContainerWriteEntry::Document);
    }

    std::uint64_t totalExpanded = 0;
    if (!checkedAdd(manifest.size(), document.size(), totalExpanded)) {
        return makeFailure(ZipContainerWriteError::SizeOverflow);
    }
    if (totalExpanded > limits.maxTotalExpandedBytes) {
        return makeFailure(ZipContainerWriteError::TotalExpandedLimitExceeded);
    }

    try {
        // Reserve zlib's worst-case working memory before any dependency call, per the contract
        // that unmetered dependency allocation is not permitted; held for the duration of every
        // deflate call below and released by RAII on every return path.
        auto workingReservationResult = operation.reserve(kQualifiedWriterWorkingReservationBytes);
        if (!workingReservationResult) {
            return makeFailure(ZipContainerWriteError::ResourceExhausted);
        }
        auto workingReservation = std::move(workingReservationResult).takeReservation();

        std::unique_ptr<ProjectIoMemoryResource> resource;
        try {
            resource = std::make_unique<ProjectIoMemoryResource>(std::move(operation));
        } catch (const std::bad_alloc&) {
            return makeFailure(ZipContainerWriteError::ResourceExhausted);
        }

        ZipContainerWriteError entryError = ZipContainerWriteError::None;
        EncodedEntry manifestEntry(resource.get());
        if (!encodeEntry(manifest, limits.maxExpansionRatio, manifestEntry, entryError)) {
            return makeFailure(entryError);
        }
        EncodedEntry documentEntry(resource.get());
        if (!encodeEntry(document, limits.maxExpansionRatio, documentEntry, entryError)) {
            return makeFailure(entryError);
        }

        std::uint64_t archiveSize = 0;
        if (!computeArchiveSize(manifestEntry.data.size(), documentEntry.data.size(),
                                archiveSize)) {
            return makeFailure(ZipContainerWriteError::SizeOverflow);
        }
        if (archiveSize > limits.maxArchiveBytes) {
            return makeFailure(ZipContainerWriteError::ArchiveSizeLimitExceeded);
        }

        std::pmr::vector<std::byte> archiveBytes(static_cast<std::size_t>(archiveSize),
                                                 resource.get());
        if (!assembleArchive(manifestEntry, documentEntry, archiveBytes)) {
            return makeFailure(ZipContainerWriteError::SizeOverflow);
        }

        ZipContainerArchive archive(std::move(resource), std::move(archiveBytes));
        return makeSuccess(std::move(archive));
    } catch (const std::bad_alloc&) {
        return makeFailure(ZipContainerWriteError::ResourceExhausted);
    } catch (...) {
        // No other exception source is expected: zlib failures are reported as ordinary return
        // codes above, never thrown. Fail closed rather than propagate an unidentified exception.
        return makeFailure(ZipContainerWriteError::QualifiedCompressorFailure);
    }
}

ZipContainerWriteResult
ZipContainerWriteBuilder::makeFailure(const ZipContainerWriteError error,
                                      const ZipContainerWriteEntry entry) noexcept {
    ZipContainerWriteResult result;
    result.error_ = error;
    result.entry_ = entry;
    return result;
}

ZipContainerWriteResult
ZipContainerWriteBuilder::makeSuccess(ZipContainerArchive archive) noexcept {
    ZipContainerWriteResult result;
    result.error_ = ZipContainerWriteError::None;
    result.archive_.emplace(std::move(archive));
    return result;
}

} // namespace bloom::project::detail

namespace bloom::project {

ZipContainerWriteResult writeZipContainer(const std::span<const std::byte> manifest,
                                          const std::span<const std::byte> document,
                                          const ZipContainerLimits& limits,
                                          ProjectIoOperationMemory operation) noexcept {
    return detail::ZipContainerWriteBuilder::write(manifest, document, limits,
                                                   std::move(operation));
}

} // namespace bloom::project
