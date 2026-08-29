#include "zip_container_preflight.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

namespace {

using bloom::project::detail::ZipContainerPreflightEntry;
using bloom::project::detail::ZipContainerPreflightError;
using bloom::project::detail::ZipContainerPreflightLimits;
using bloom::project::detail::ZipContainerPreflightResult;

constexpr std::uint32_t kLocalFileHeaderSignature = 0x04034b50U;
constexpr std::uint32_t kCentralDirectorySignature = 0x02014b50U;
constexpr std::uint32_t kEndOfCentralDirectorySignature = 0x06054b50U;
constexpr std::uint64_t kLocalHeaderFixedBytes = 30;
constexpr std::uint64_t kCentralHeaderFixedBytes = 46;
constexpr std::uint64_t kEocdFixedBytes = 22;
constexpr std::uint64_t kEocdMaximumCommentLength = 0xFFFFU;
constexpr std::uint16_t kZip64Sentinel16 = 0xFFFFU;
constexpr std::uint32_t kZip64Sentinel32 = 0xFFFFFFFFU;
constexpr std::uint16_t kUtf8FlagBit = 0x0800U;
constexpr std::uint16_t kMethodStored = 0;
constexpr std::uint16_t kMethodDeflate = 8;
constexpr std::string_view kManifestName = "manifest.json";
constexpr std::string_view kDocumentName = "document.json";

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

[[nodiscard]] std::uint8_t byteValue(const std::byte value) noexcept {
    return std::to_integer<std::uint8_t>(value);
}

// One fully decoded local file header: every field this profile inspects, plus the resolved data
// byte range (checked, in-bounds by construction of advance()).
struct LocalHeader final {
    std::uint64_t offset = 0;
    std::uint16_t flags = 0;
    std::uint16_t method = 0;
    std::uint32_t crc32Value = 0;
    std::uint32_t compressedSize = 0;
    std::uint32_t uncompressedSize = 0;
    std::string_view name;
    std::uint64_t dataOffset = 0;
    std::uint64_t dataEnd = 0;
};

// One fully decoded central directory file header.
struct CentralEntry final {
    std::uint64_t offset = 0;
    std::uint16_t versionMadeBy = 0;
    std::uint16_t flags = 0;
    std::uint16_t method = 0;
    std::uint32_t crc32Value = 0;
    std::uint32_t compressedSize = 0;
    std::uint32_t uncompressedSize = 0;
    std::string_view name;
    std::uint16_t diskNumberStart = 0;
    std::uint32_t externalAttrs = 0;
    std::uint32_t localHeaderOffset = 0;
    std::uint64_t entryEnd = 0;
};

struct EocdInfo final {
    std::uint64_t offset = 0;
    std::uint16_t diskNumber = 0;
    std::uint16_t diskWithCd = 0;
    std::uint16_t entriesThisDisk = 0;
    std::uint16_t entriesTotal = 0;
    std::uint32_t cdSize = 0;
    std::uint32_t cdOffset = 0;
};

class Scanner final {
  public:
    Scanner(const std::span<const std::byte> archive,
            const ZipContainerPreflightLimits limits) noexcept
        : archive_(archive), limits_(limits) {}

    [[nodiscard]] ZipContainerPreflightResult run() noexcept;

  private:
    [[nodiscard]] bool limitsAreValid() const noexcept {
        return limits_.maxArchiveBytes != 0 && limits_.maxManifestBytes != 0 &&
               limits_.maxDocumentBytes != 0 && limits_.maxTotalExpandedBytes != 0 &&
               limits_.maxExpansionRatio != 0;
    }

    [[nodiscard]] std::uint16_t readU16(const std::uint64_t offset) const noexcept {
        return static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(byteValue(archive_[offset])) |
            (static_cast<std::uint16_t>(byteValue(archive_[offset + 1])) << 8U));
    }

    [[nodiscard]] std::uint32_t readU32(const std::uint64_t offset) const noexcept {
        return static_cast<std::uint32_t>(byteValue(archive_[offset])) |
               (static_cast<std::uint32_t>(byteValue(archive_[offset + 1])) << 8U) |
               (static_cast<std::uint32_t>(byteValue(archive_[offset + 2])) << 16U) |
               (static_cast<std::uint32_t>(byteValue(archive_[offset + 3])) << 24U);
    }

    [[nodiscard]] std::string_view viewBytes(const std::uint64_t offset,
                                             const std::uint64_t length) const noexcept {
        // Archive bytes are proven in-bounds by every caller via advance() before this is
        // invoked; std::byte and char share the same object representation, so this reinterpret
        // is the standard way to hand a contiguous byte range to string_view for exact
        // byte-for-byte name comparison.
        return {reinterpret_cast<const char*>(archive_.data()) + offset, length};
    }

    // Advances `cursor` by `amount`, checked against arithmetic overflow and the archive's true
    // size. Every structural offset in this scanner is derived exclusively through this helper so
    // no field is ever read out of bounds.
    [[nodiscard]] bool advance(std::uint64_t& cursor, const std::uint64_t amount) noexcept {
        std::uint64_t next = 0;
        if (!checkedAdd(cursor, amount, next)) {
            setError(ZipContainerPreflightError::SizeOverflow, cursor);
            return false;
        }
        if (next > archive_.size()) {
            setError(ZipContainerPreflightError::ArchiveTruncated, archive_.size());
            return false;
        }
        cursor = next;
        return true;
    }

    void setError(const ZipContainerPreflightError error, const std::uint64_t offset) noexcept {
        error_ = error;
        byteOffset_ = offset;
    }

    [[nodiscard]] ZipContainerPreflightResult currentFailure() const noexcept {
        return {.error = error_, .byteOffset = static_cast<std::size_t>(byteOffset_)};
    }

    [[nodiscard]] bool locateEocd(EocdInfo& out) noexcept;
    [[nodiscard]] bool validateEocdFields(const EocdInfo& eocd) noexcept;
    [[nodiscard]] bool parseLocalHeader(std::uint64_t offset, LocalHeader& out) noexcept;
    [[nodiscard]] bool parseCentralEntry(std::uint64_t offset, CentralEntry& out) noexcept;
    [[nodiscard]] bool checkGeneralPurposeFlag(std::uint16_t flags, std::uint64_t offset) noexcept;
    [[nodiscard]] bool checkMethod(std::uint16_t method, std::uint64_t offset) noexcept;
    [[nodiscard]] bool checkStoredConsistency(std::uint16_t method, std::uint32_t compressedSize,
                                              std::uint32_t uncompressedSize,
                                              std::uint64_t offset) noexcept;
    [[nodiscard]] bool resolveNames(std::string_view firstName, std::string_view secondName,
                                    std::uint64_t firstOffset, std::uint64_t secondOffset) noexcept;
    [[nodiscard]] bool checkAgreement(const LocalHeader& local, const CentralEntry& central,
                                      std::uint64_t expectedLocalOffset) noexcept;
    [[nodiscard]] bool checkAttributes(const CentralEntry& central) noexcept;
    [[nodiscard]] bool checkRatio(const ZipContainerPreflightEntry& entry) noexcept;
    [[nodiscard]] bool checkLimits(const ZipContainerPreflightEntry& manifest,
                                   const ZipContainerPreflightEntry& document) noexcept;

    std::span<const std::byte> archive_;
    ZipContainerPreflightLimits limits_;
    ZipContainerPreflightError error_ = ZipContainerPreflightError::None;
    std::uint64_t byteOffset_ = 0;
};

bool Scanner::locateEocd(EocdInfo& out) noexcept {
    const auto size = static_cast<std::uint64_t>(archive_.size());
    const auto maxBack = kEocdFixedBytes + kEocdMaximumCommentLength;
    const auto scanFloor = size > maxBack ? size - maxBack : 0;
    auto candidate = size - kEocdFixedBytes;
    while (true) {
        if (readU32(candidate) == kEndOfCentralDirectorySignature) {
            const auto commentLength = readU16(candidate + 20);
            if (candidate + kEocdFixedBytes + commentLength == size) {
                if (commentLength != 0) {
                    setError(ZipContainerPreflightError::ArchiveCommentForbidden, candidate);
                    return false;
                }
                out.offset = candidate;
                out.diskNumber = readU16(candidate + 4);
                out.diskWithCd = readU16(candidate + 6);
                out.entriesThisDisk = readU16(candidate + 8);
                out.entriesTotal = readU16(candidate + 10);
                out.cdSize = readU32(candidate + 12);
                out.cdOffset = readU32(candidate + 16);
                return true;
            }
        }
        if (candidate == scanFloor) {
            break;
        }
        --candidate;
    }
    setError(ZipContainerPreflightError::MissingOrMalformedEocd, size - kEocdFixedBytes);
    return false;
}

bool Scanner::validateEocdFields(const EocdInfo& eocd) noexcept {
    if (eocd.diskNumber != 0 || eocd.diskWithCd != 0) {
        setError(ZipContainerPreflightError::MultiDiskForbidden, eocd.offset + 4);
        return false;
    }
    if (eocd.entriesThisDisk == kZip64Sentinel16 || eocd.entriesTotal == kZip64Sentinel16 ||
        eocd.cdSize == kZip64Sentinel32 || eocd.cdOffset == kZip64Sentinel32) {
        setError(ZipContainerPreflightError::Zip64Forbidden, eocd.offset);
        return false;
    }
    if (eocd.entriesThisDisk != 2 || eocd.entriesTotal != 2) {
        setError(ZipContainerPreflightError::WrongEntryCount, eocd.offset + 8);
        return false;
    }
    return true;
}

bool Scanner::checkGeneralPurposeFlag(const std::uint16_t flags,
                                      const std::uint64_t offset) noexcept {
    if (flags == kUtf8FlagBit) {
        return true;
    }
    if (flags == 0) {
        setError(ZipContainerPreflightError::Utf8FlagMissing, offset);
        return false;
    }
    setError(ZipContainerPreflightError::ForbiddenGeneralPurposeFlag, offset);
    return false;
}

bool Scanner::checkMethod(const std::uint16_t method, const std::uint64_t offset) noexcept {
    if (method == kMethodStored || method == kMethodDeflate) {
        return true;
    }
    setError(ZipContainerPreflightError::UnsupportedCompressionMethod, offset);
    return false;
}

bool Scanner::checkStoredConsistency(const std::uint16_t method, const std::uint32_t compressedSize,
                                     const std::uint32_t uncompressedSize,
                                     const std::uint64_t offset) noexcept {
    if (method == kMethodStored && compressedSize != uncompressedSize) {
        setError(ZipContainerPreflightError::StoredSizeMismatch, offset);
        return false;
    }
    return true;
}

bool Scanner::parseLocalHeader(const std::uint64_t offset, LocalHeader& out) noexcept {
    std::uint64_t cursor = offset;
    if (!advance(cursor, kLocalHeaderFixedBytes)) {
        return false;
    }
    if (readU32(offset) != kLocalFileHeaderSignature) {
        setError(ZipContainerPreflightError::OverlappingOrUnaccountedByteRange, offset);
        return false;
    }
    const auto flags = readU16(offset + 6);
    const auto method = readU16(offset + 8);
    const auto crc32Value = readU32(offset + 14);
    const auto compressedSize = readU32(offset + 18);
    const auto uncompressedSize = readU32(offset + 22);
    const auto nameLength = readU16(offset + 26);
    const auto extraLength = readU16(offset + 28);

    const auto nameStart = cursor;
    if (!advance(cursor, nameLength)) {
        return false;
    }
    const auto name = viewBytes(nameStart, nameLength);
    if (!advance(cursor, extraLength)) {
        return false;
    }
    const auto dataStart = cursor;
    if (!advance(cursor, compressedSize)) {
        return false;
    }

    if (!checkGeneralPurposeFlag(flags, offset)) {
        return false;
    }
    if (!checkMethod(method, offset)) {
        return false;
    }
    if (compressedSize == kZip64Sentinel32 || uncompressedSize == kZip64Sentinel32) {
        setError(ZipContainerPreflightError::Zip64Forbidden, offset);
        return false;
    }
    if (!checkStoredConsistency(method, compressedSize, uncompressedSize, offset)) {
        return false;
    }
    if (extraLength != 0) {
        setError(ZipContainerPreflightError::ExtraFieldForbidden, offset);
        return false;
    }

    out = LocalHeader{
        .offset = offset,
        .flags = flags,
        .method = method,
        .crc32Value = crc32Value,
        .compressedSize = compressedSize,
        .uncompressedSize = uncompressedSize,
        .name = name,
        .dataOffset = dataStart,
        .dataEnd = cursor,
    };
    return true;
}

bool Scanner::parseCentralEntry(const std::uint64_t offset, CentralEntry& out) noexcept {
    std::uint64_t cursor = offset;
    if (!advance(cursor, kCentralHeaderFixedBytes)) {
        return false;
    }
    if (readU32(offset) != kCentralDirectorySignature) {
        setError(ZipContainerPreflightError::OverlappingOrUnaccountedByteRange, offset);
        return false;
    }
    const auto versionMadeBy = readU16(offset + 4);
    const auto flags = readU16(offset + 8);
    const auto method = readU16(offset + 10);
    const auto crc32Value = readU32(offset + 16);
    const auto compressedSize = readU32(offset + 20);
    const auto uncompressedSize = readU32(offset + 24);
    const auto nameLength = readU16(offset + 28);
    const auto extraLength = readU16(offset + 30);
    const auto commentLength = readU16(offset + 32);
    const auto diskNumberStart = readU16(offset + 34);
    const auto externalAttrs = readU32(offset + 38);
    const auto localHeaderOffset = readU32(offset + 42);

    const auto nameStart = cursor;
    if (!advance(cursor, nameLength)) {
        return false;
    }
    const auto name = viewBytes(nameStart, nameLength);
    if (!advance(cursor, extraLength)) {
        return false;
    }
    if (!advance(cursor, commentLength)) {
        return false;
    }

    if (!checkGeneralPurposeFlag(flags, offset)) {
        return false;
    }
    if (!checkMethod(method, offset)) {
        return false;
    }
    if (compressedSize == kZip64Sentinel32 || uncompressedSize == kZip64Sentinel32 ||
        localHeaderOffset == kZip64Sentinel32) {
        setError(ZipContainerPreflightError::Zip64Forbidden, offset);
        return false;
    }
    if (!checkStoredConsistency(method, compressedSize, uncompressedSize, offset)) {
        return false;
    }
    if (extraLength != 0) {
        setError(ZipContainerPreflightError::ExtraFieldForbidden, offset);
        return false;
    }
    if (commentLength != 0) {
        setError(ZipContainerPreflightError::EntryCommentForbidden, offset);
        return false;
    }

    out = CentralEntry{
        .offset = offset,
        .versionMadeBy = versionMadeBy,
        .flags = flags,
        .method = method,
        .crc32Value = crc32Value,
        .compressedSize = compressedSize,
        .uncompressedSize = uncompressedSize,
        .name = name,
        .diskNumberStart = diskNumberStart,
        .externalAttrs = externalAttrs,
        .localHeaderOffset = localHeaderOffset,
        .entryEnd = cursor,
    };
    return true;
}

bool Scanner::resolveNames(const std::string_view firstName, const std::string_view secondName,
                           const std::uint64_t firstOffset,
                           const std::uint64_t secondOffset) noexcept {
    if (firstName == kManifestName && secondName == kDocumentName) {
        return true;
    }
    if (firstName == kDocumentName && secondName == kManifestName) {
        setError(ZipContainerPreflightError::WrongEntryOrder, secondOffset);
        return false;
    }
    setError(ZipContainerPreflightError::WrongEntryName, firstOffset);
    return false;
}

bool Scanner::checkAgreement(const LocalHeader& local, const CentralEntry& central,
                             const std::uint64_t expectedLocalOffset) noexcept {
    if (local.name != central.name || local.flags != central.flags ||
        local.method != central.method || local.crc32Value != central.crc32Value ||
        local.compressedSize != central.compressedSize ||
        local.uncompressedSize != central.uncompressedSize ||
        central.localHeaderOffset != expectedLocalOffset) {
        setError(ZipContainerPreflightError::LocalCentralHeaderDisagreement, central.offset);
        return false;
    }
    return true;
}

bool Scanner::checkAttributes(const CentralEntry& central) noexcept {
    constexpr std::uint8_t kHostUnix = 3;
    constexpr std::uint32_t kUnixFileTypeRegular = 0x8U;
    constexpr std::uint32_t kUnixExecuteBits = 0111U;
    constexpr std::uint32_t kDosDirectoryBit = 0x10U;

    // The DOS-compatible low attribute byte is checked unconditionally, on every host: many zip
    // tools populate it regardless of the primary host they declare, so a crafted entry cannot
    // claim Unix host with clean Unix mode type/execute bits while still smuggling the DOS
    // directory bit through in the same external-attributes field.
    if ((central.externalAttrs & kDosDirectoryBit) != 0) {
        setError(ZipContainerPreflightError::NonRegularEntry, central.offset);
        return false;
    }

    const auto host = static_cast<std::uint8_t>(central.versionMadeBy >> 8U);
    if (host == kHostUnix) {
        const auto mode = central.externalAttrs >> 16U;
        const auto fileType = (mode >> 12U) & 0xFU;
        if (fileType != 0 && fileType != kUnixFileTypeRegular) {
            setError(ZipContainerPreflightError::NonRegularEntry, central.offset);
            return false;
        }
        if ((mode & kUnixExecuteBits) != 0) {
            setError(ZipContainerPreflightError::ExecutableEntry, central.offset);
            return false;
        }
    }
    return true;
}

bool Scanner::checkRatio(const ZipContainerPreflightEntry& entry) noexcept {
    const auto denominator = std::max<std::uint64_t>(1, entry.compressedSize);
    std::uint64_t allowedMaximum = 0;
    if (!checkedMul(limits_.maxExpansionRatio, denominator, allowedMaximum)) {
        setError(ZipContainerPreflightError::SizeOverflow, entry.localHeaderOffset);
        return false;
    }
    if (entry.uncompressedSize > allowedMaximum) {
        setError(ZipContainerPreflightError::ExpansionRatioExceeded, entry.localHeaderOffset);
        return false;
    }
    return true;
}

bool Scanner::checkLimits(const ZipContainerPreflightEntry& manifest,
                          const ZipContainerPreflightEntry& document) noexcept {
    if (manifest.uncompressedSize > limits_.maxManifestBytes) {
        setError(ZipContainerPreflightError::ExpandedSizeLimitExceeded, manifest.localHeaderOffset);
        return false;
    }
    if (document.uncompressedSize > limits_.maxDocumentBytes) {
        setError(ZipContainerPreflightError::ExpandedSizeLimitExceeded, document.localHeaderOffset);
        return false;
    }
    std::uint64_t total = 0;
    if (!checkedAdd(manifest.uncompressedSize, document.uncompressedSize, total)) {
        setError(ZipContainerPreflightError::SizeOverflow, 0);
        return false;
    }
    if (total > limits_.maxTotalExpandedBytes) {
        setError(ZipContainerPreflightError::ExpandedSizeLimitExceeded, 0);
        return false;
    }
    if (!checkRatio(manifest)) {
        return false;
    }
    if (!checkRatio(document)) {
        return false;
    }
    return true;
}

ZipContainerPreflightResult Scanner::run() noexcept {
    if (!limitsAreValid()) {
        setError(ZipContainerPreflightError::InvalidLimits, 0);
        return currentFailure();
    }
    const auto size = static_cast<std::uint64_t>(archive_.size());
    if (size > limits_.maxArchiveBytes) {
        setError(ZipContainerPreflightError::ArchiveTooLarge, limits_.maxArchiveBytes);
        return currentFailure();
    }
    if (size < kEocdFixedBytes) {
        setError(ZipContainerPreflightError::ArchiveTruncated, size);
        return currentFailure();
    }

    EocdInfo eocd{};
    if (!locateEocd(eocd)) {
        return currentFailure();
    }
    if (!validateEocdFields(eocd)) {
        return currentFailure();
    }

    LocalHeader localA{};
    if (!parseLocalHeader(0, localA)) {
        return currentFailure();
    }
    LocalHeader localB{};
    if (!parseLocalHeader(localA.dataEnd, localB)) {
        return currentFailure();
    }
    if (!resolveNames(localA.name, localB.name, localA.offset, localB.offset)) {
        return currentFailure();
    }

    CentralEntry centralA{};
    if (!parseCentralEntry(localB.dataEnd, centralA)) {
        return currentFailure();
    }
    CentralEntry centralB{};
    if (!parseCentralEntry(centralA.entryEnd, centralB)) {
        return currentFailure();
    }
    if (!resolveNames(centralA.name, centralB.name, centralA.offset, centralB.offset)) {
        return currentFailure();
    }

    const auto centralDirectoryStart = localB.dataEnd;
    const auto centralDirectoryEnd = centralB.entryEnd;
    if (eocd.cdOffset != centralDirectoryStart ||
        eocd.cdSize != centralDirectoryEnd - centralDirectoryStart) {
        setError(ZipContainerPreflightError::OverlappingOrUnaccountedByteRange,
                 centralDirectoryStart);
        return currentFailure();
    }
    if (centralDirectoryEnd != eocd.offset) {
        setError(ZipContainerPreflightError::OverlappingOrUnaccountedByteRange,
                 centralDirectoryEnd);
        return currentFailure();
    }

    if (!checkAgreement(localA, centralA, 0)) {
        return currentFailure();
    }
    if (!checkAgreement(localB, centralB, localA.dataEnd)) {
        return currentFailure();
    }
    if (!checkAttributes(centralA)) {
        return currentFailure();
    }
    if (!checkAttributes(centralB)) {
        return currentFailure();
    }
    if (centralA.diskNumberStart != 0) {
        setError(ZipContainerPreflightError::MultiDiskForbidden, centralA.offset + 34);
        return currentFailure();
    }
    if (centralB.diskNumberStart != 0) {
        setError(ZipContainerPreflightError::MultiDiskForbidden, centralB.offset + 34);
        return currentFailure();
    }

    ZipContainerPreflightEntry manifest{
        .localHeaderOffset = 0,
        .dataOffset = localA.dataOffset,
        .compressedSize = localA.compressedSize,
        .uncompressedSize = localA.uncompressedSize,
        .crc32Value = localA.crc32Value,
        .method = localA.method,
    };
    ZipContainerPreflightEntry document{
        .localHeaderOffset = localA.dataEnd,
        .dataOffset = localB.dataOffset,
        .compressedSize = localB.compressedSize,
        .uncompressedSize = localB.uncompressedSize,
        .crc32Value = localB.crc32Value,
        .method = localB.method,
    };
    if (!checkLimits(manifest, document)) {
        return currentFailure();
    }

    return {
        .error = ZipContainerPreflightError::None,
        .byteOffset = archive_.size(),
        .manifest = manifest,
        .document = document,
    };
}

} // namespace

namespace bloom::project::detail {

ZipContainerPreflightResult
preflightZipContainer(const std::span<const std::byte> archive,
                      const ZipContainerPreflightLimits& limits) noexcept {
    return Scanner(archive, limits).run();
}

} // namespace bloom::project::detail
