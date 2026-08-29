#include "zip_container_preflight.hpp"

#include "zip_container_test_support.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using bloom::project::detail::preflightZipContainer;
using bloom::project::detail::ZipContainerPreflightError;
using bloom::project::detail::ZipContainerPreflightLimits;
using bloom::project::detail::ZipContainerPreflightResult;
using bloom::project::test::ArchiveWriter;
using bloom::project::test::buildConformingArchive;
using bloom::project::test::EntrySpec;
using bloom::project::test::makeDeflateEntry;
using bloom::project::test::makeStoredEntry;
using bloom::project::test::toBytes;

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message,
                const std::source_location location = std::source_location::current()) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }

    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

[[nodiscard]] ZipContainerPreflightResult run(const std::vector<std::byte>& archive,
                                              const ZipContainerPreflightLimits& limits = {}) {
    return preflightZipContainer(std::span<const std::byte>(archive), limits);
}

[[nodiscard]] EntrySpec manifestEntry(const std::string_view payload = "{}") {
    return makeStoredEntry("manifest.json", toBytes(payload));
}

[[nodiscard]] EntrySpec documentEntry(const std::string_view payload = "{}") {
    return makeStoredEntry("document.json", toBytes(payload));
}

void testConformingArchives(Expectations& expectations) {
    {
        const auto archive =
            buildConformingArchive(manifestEntry("{\"m\":1}"), documentEntry("{\"d\":2}"));
        const auto result = run(archive);
        expectations.expect(result.succeeded(), "a stored-only conforming archive is accepted");
        expectations.expect(result.manifest.uncompressedSize == 7 &&
                                result.document.uncompressedSize == 7,
                            "declared sizes are captured exactly");
        const auto second = run(archive);
        expectations.expect(second.succeeded() &&
                                second.manifest.crc32Value == result.manifest.crc32Value,
                            "preflighting the same bytes twice is deterministic");
    }
    {
        const std::string manifestPayload(2000, 'm');
        const std::string documentPayload(3000, 'd');
        const auto archive =
            buildConformingArchive(makeDeflateEntry("manifest.json", toBytes(manifestPayload)),
                                   makeDeflateEntry("document.json", toBytes(documentPayload)));
        const auto result = run(archive);
        expectations.expect(result.succeeded() && result.manifest.method == 8 &&
                                result.document.method == 8,
                            "a deflate-only conforming archive is accepted");
    }
    {
        const auto archive = buildConformingArchive(
            manifestEntry(), makeDeflateEntry("document.json", toBytes(std::string(500, 'x'))));
        const auto result = run(archive);
        expectations.expect(result.succeeded() && result.manifest.method == 0 &&
                                result.document.method == 8,
                            "a mixed stored/deflate archive is accepted");
    }
}

void testEmptyPayloadEntry(Expectations& expectations) {
    const auto archive = buildConformingArchive(manifestEntry(""), documentEntry("{}"));
    const auto result = run(archive);
    expectations.expect(result.succeeded() && result.manifest.uncompressedSize == 0 &&
                            result.manifest.crc32Value == 0,
                        "a zero-byte manifest entry is structurally accepted with CRC 0");
}

void testInvalidLimits(Expectations& expectations) {
    ZipContainerPreflightLimits limits{};
    limits.maxManifestBytes = 0;
    const auto archive = buildConformingArchive(manifestEntry(), documentEntry());
    const auto result = run(archive, limits);
    expectations.expect(!result.succeeded() &&
                            result.error == ZipContainerPreflightError::InvalidLimits,
                        "a zeroed limit field is rejected before any byte is inspected");
}

void testArchiveTooLarge(Expectations& expectations) {
    ZipContainerPreflightLimits limits{};
    const auto archive = buildConformingArchive(manifestEntry(), documentEntry());
    limits.maxArchiveBytes = archive.size() - 1;
    const auto result = run(archive, limits);
    expectations.expect(!result.succeeded() &&
                            result.error == ZipContainerPreflightError::ArchiveTooLarge,
                        "an archive one byte over the physical size ceiling is rejected");
}

void testTruncation(Expectations& expectations) {
    const auto archive = buildConformingArchive(manifestEntry(), documentEntry());
    {
        std::vector<std::byte> truncated(archive.begin(), archive.begin() + 10);
        const auto result = run(truncated);
        expectations.expect(!result.succeeded() &&
                                result.error == ZipContainerPreflightError::ArchiveTruncated,
                            "an archive shorter than one EOCD record is rejected as truncated");
    }
    {
        // A local header whose declared compressed size claims more bytes than the whole
        // (otherwise genuinely valid and self-consistent) archive contains: the central directory
        // and EOCD honestly describe the true, small physical layout, so this is caught by the
        // local-header data range walking past end-of-file, not by a missing EOCD.
        auto manifest = manifestEntry();
        manifest.localCompressedSize = manifest.centralCompressedSize = 1'000'000;
        const auto truncated = buildConformingArchive(manifest, documentEntry());
        const auto result = run(truncated);
        expectations.expect(!result.succeeded() &&
                                result.error == ZipContainerPreflightError::ArchiveTruncated,
                            "a declared entry data range extending past the true end of the "
                            "archive is rejected as truncated");
    }
    {
        // Cutting bytes off the tail moves the EOCD's signature out of the scanner's fixed
        // 22-byte window without leaving any other self-consistent EOCD candidate nearby, so this
        // is indistinguishable from a genuinely missing/malformed EOCD rather than a distinct
        // "structure truncated mid-record" case.
        std::vector<std::byte> truncated(archive.begin(), archive.end() - 5);
        const auto result = run(truncated);
        expectations.expect(!result.succeeded() &&
                                result.error == ZipContainerPreflightError::MissingOrMalformedEocd,
                            "cutting bytes off the archive's tail is rejected as a malformed EOCD");
    }
    {
        std::vector<std::byte> empty;
        const auto result = run(empty);
        expectations.expect(!result.succeeded() &&
                                result.error == ZipContainerPreflightError::ArchiveTruncated,
                            "an empty archive is rejected as truncated");
    }
}

void testEocdLocationAndComment(Expectations& expectations) {
    {
        auto archive = buildConformingArchive(manifestEntry(), documentEntry());
        archive.push_back(std::byte{0xAB});
        archive.push_back(std::byte{0xCD});
        const auto result = run(archive);
        expectations.expect(
            !result.succeeded() &&
                result.error == ZipContainerPreflightError::MissingOrMalformedEocd,
            "trailing bytes after a well-formed EOCD are rejected as malformed EOCD");
    }
    {
        ArchiveWriter writer;
        const auto manifest = manifestEntry();
        const auto document = documentEntry();
        const auto manifestOffset = writer.appendLocal(manifest);
        const auto documentOffset = writer.appendLocal(document);
        const auto centralStart = writer.size();
        writer.appendCentral(manifest, static_cast<std::uint32_t>(manifestOffset));
        writer.appendCentral(document, static_cast<std::uint32_t>(documentOffset));
        const auto centralSize = writer.size() - centralStart;
        writer.appendEocd(0, 0, 2, 2, static_cast<std::uint32_t>(centralSize),
                          static_cast<std::uint32_t>(centralStart), "hello");
        const auto result = run(writer.bytes());
        expectations.expect(!result.succeeded() &&
                                result.error == ZipContainerPreflightError::ArchiveCommentForbidden,
                            "a nonzero, self-consistent archive comment is rejected distinctly");
    }
}

void testDiskNumbers(Expectations& expectations) {
    {
        ArchiveWriter writer;
        const auto manifest = manifestEntry();
        const auto document = documentEntry();
        const auto manifestOffset = writer.appendLocal(manifest);
        const auto documentOffset = writer.appendLocal(document);
        const auto centralStart = writer.size();
        writer.appendCentral(manifest, static_cast<std::uint32_t>(manifestOffset));
        writer.appendCentral(document, static_cast<std::uint32_t>(documentOffset));
        const auto centralSize = writer.size() - centralStart;
        writer.appendEocd(1, 0, 2, 2, static_cast<std::uint32_t>(centralSize),
                          static_cast<std::uint32_t>(centralStart));
        const auto result = run(writer.bytes());
        expectations.expect(!result.succeeded() &&
                                result.error == ZipContainerPreflightError::MultiDiskForbidden,
                            "a nonzero EOCD disk number is rejected");
    }
    {
        auto manifest = manifestEntry();
        manifest.diskNumberStart = 1;
        const auto archive = buildConformingArchive(manifest, documentEntry());
        const auto result = run(archive);
        expectations.expect(!result.succeeded() &&
                                result.error == ZipContainerPreflightError::MultiDiskForbidden,
                            "a nonzero per-entry central disk-number-start is rejected");
    }
}

void testEntryCount(Expectations& expectations) {
    for (const std::uint16_t count : {std::uint16_t{1}, std::uint16_t{3}}) {
        ArchiveWriter writer;
        const auto manifest = manifestEntry();
        const auto document = documentEntry();
        const auto manifestOffset = writer.appendLocal(manifest);
        const auto documentOffset = writer.appendLocal(document);
        const auto centralStart = writer.size();
        writer.appendCentral(manifest, static_cast<std::uint32_t>(manifestOffset));
        writer.appendCentral(document, static_cast<std::uint32_t>(documentOffset));
        const auto centralSize = writer.size() - centralStart;
        writer.appendEocd(0, 0, count, count, static_cast<std::uint32_t>(centralSize),
                          static_cast<std::uint32_t>(centralStart));
        const auto result = run(writer.bytes());
        expectations.expect(!result.succeeded() &&
                                result.error == ZipContainerPreflightError::WrongEntryCount,
                            "an EOCD entry count other than exactly two is rejected");
    }
}

void testEntryNames(Expectations& expectations) {
    const std::vector<std::string> badNames{"Manifest.json", std::string("manifest.json") + '\0',
                                            "./manifest.json", "manifest\\json", "/manifest.json"};
    for (const auto& badName : badNames) {
        auto manifest = manifestEntry();
        manifest.localName = badName;
        manifest.centralName = badName;
        const auto archive = buildConformingArchive(manifest, documentEntry());
        const auto result = run(archive);
        expectations.expect(!result.succeeded() &&
                                result.error == ZipContainerPreflightError::WrongEntryName,
                            "an exact-name deviation is rejected as wrong entry name: " + badName);
    }
    {
        // Reversed order in the central directory, local headers left untouched.
        ArchiveWriter writer;
        const auto manifest = manifestEntry();
        const auto document = documentEntry();
        const auto manifestOffset = writer.appendLocal(manifest);
        const auto documentOffset = writer.appendLocal(document);
        const auto centralStart = writer.size();
        writer.appendCentral(document, static_cast<std::uint32_t>(documentOffset));
        writer.appendCentral(manifest, static_cast<std::uint32_t>(manifestOffset));
        const auto centralSize = writer.size() - centralStart;
        writer.appendEocd(0, 0, 2, 2, static_cast<std::uint32_t>(centralSize),
                          static_cast<std::uint32_t>(centralStart));
        const auto result = run(writer.bytes());
        expectations.expect(
            !result.succeeded() && result.error == ZipContainerPreflightError::WrongEntryOrder,
            "a reversed central directory order is rejected distinctly from a wrong name");
    }
    {
        // Reversed order at the local-header level too (document physically first).
        auto manifest = manifestEntry();
        auto document = documentEntry();
        const auto archive = buildConformingArchive(document, manifest);
        const auto result = run(archive);
        expectations.expect(!result.succeeded() &&
                                result.error == ZipContainerPreflightError::WrongEntryOrder,
                            "swapping both local headers (and their matching central records) is "
                            "rejected as wrong order, detected at the local level");
    }
}

void testGeneralPurposeFlags(Expectations& expectations) {
    {
        auto manifest = manifestEntry();
        manifest.localFlags = manifest.centralFlags = 0;
        const auto archive = buildConformingArchive(manifest, documentEntry());
        const auto result = run(archive);
        expectations.expect(!result.succeeded() &&
                                result.error == ZipContainerPreflightError::Utf8FlagMissing,
                            "flags == 0 is reported as a missing UTF-8 flag");
    }
    {
        auto manifest = manifestEntry();
        manifest.localFlags = manifest.centralFlags = 0x0808; // UTF-8 + data descriptor.
        const auto archive = buildConformingArchive(manifest, documentEntry());
        const auto result = run(archive);
        expectations.expect(!result.succeeded() &&
                                result.error ==
                                    ZipContainerPreflightError::ForbiddenGeneralPurposeFlag,
                            "the data-descriptor bit is a forbidden general-purpose flag");
    }
    {
        auto manifest = manifestEntry();
        manifest.localFlags = manifest.centralFlags = 0x0801; // UTF-8 + encryption.
        const auto archive = buildConformingArchive(manifest, documentEntry());
        const auto result = run(archive);
        expectations.expect(!result.succeeded() &&
                                result.error ==
                                    ZipContainerPreflightError::ForbiddenGeneralPurposeFlag,
                            "the encryption bit is a forbidden general-purpose flag");
    }
}

void testCompressionMethod(Expectations& expectations) {
    for (const std::uint16_t method : {std::uint16_t{1}, std::uint16_t{12}}) {
        auto manifest = manifestEntry();
        manifest.localMethod = manifest.centralMethod = method;
        const auto archive = buildConformingArchive(manifest, documentEntry());
        const auto result = run(archive);
        expectations.expect(!result.succeeded() &&
                                result.error ==
                                    ZipContainerPreflightError::UnsupportedCompressionMethod,
                            "an unsupported compression method is rejected");
    }
}

void testExtraField(Expectations& expectations) {
    {
        auto manifest = manifestEntry();
        manifest.localExtra = {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}};
        const auto archive = buildConformingArchive(manifest, documentEntry());
        const auto result = run(archive);
        expectations.expect(!result.succeeded() &&
                                result.error == ZipContainerPreflightError::ExtraFieldForbidden,
                            "a local-only extra field is rejected");
    }
    {
        auto manifest = manifestEntry();
        manifest.centralExtra = {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}};
        const auto archive = buildConformingArchive(manifest, documentEntry());
        const auto result = run(archive);
        expectations.expect(!result.succeeded() &&
                                result.error == ZipContainerPreflightError::ExtraFieldForbidden,
                            "a central-only extra field is rejected");
    }
}

void testEntryComment(Expectations& expectations) {
    auto manifest = manifestEntry();
    manifest.centralComment = "hi";
    const auto archive = buildConformingArchive(manifest, documentEntry());
    const auto result = run(archive);
    expectations.expect(!result.succeeded() &&
                            result.error == ZipContainerPreflightError::EntryCommentForbidden,
                        "a per-entry central directory comment is rejected");
}

void testZip64Sentinels(Expectations& expectations) {
    auto manifest = manifestEntry();
    manifest.localUncompressedSize = manifest.centralUncompressedSize = 0xFFFFFFFFU;
    const auto archive = buildConformingArchive(manifest, documentEntry());
    const auto result = run(archive);
    expectations.expect(!result.succeeded() &&
                            result.error == ZipContainerPreflightError::Zip64Forbidden,
                        "a ZIP64 uncompressed-size sentinel is rejected");
}

void testLocalCentralDisagreement(Expectations& expectations) {
    {
        auto manifest = manifestEntry();
        manifest.centralMethod = 8; // local stays stored (0); central claims deflate.
        const auto archive = buildConformingArchive(manifest, documentEntry());
        const auto result = run(archive);
        expectations.expect(!result.succeeded() &&
                                result.error ==
                                    ZipContainerPreflightError::LocalCentralHeaderDisagreement,
                            "a local/central method disagreement is rejected");
    }
    {
        auto manifest = manifestEntry();
        manifest.centralCrc ^= 0xFFFFFFFFU;
        const auto archive = buildConformingArchive(manifest, documentEntry());
        const auto result = run(archive);
        expectations.expect(!result.succeeded() &&
                                result.error ==
                                    ZipContainerPreflightError::LocalCentralHeaderDisagreement,
                            "a local/central CRC disagreement is rejected");
    }
}

void testByteAccounting(Expectations& expectations) {
    {
        std::vector<std::byte> withPrefix{std::byte{'X'}, std::byte{'Y'}};
        const auto archive = buildConformingArchive(manifestEntry(), documentEntry());
        withPrefix.insert(withPrefix.end(), archive.begin(), archive.end());
        const auto result = run(withPrefix);
        expectations.expect(
            !result.succeeded() &&
                result.error == ZipContainerPreflightError::OverlappingOrUnaccountedByteRange,
            "prepended bytes shifting the manifest local header off offset 0 are rejected");
    }
    {
        ArchiveWriter writer;
        const auto manifest = manifestEntry();
        const auto document = documentEntry();
        const auto manifestOffset = writer.appendLocal(manifest);
        const auto documentOffset = writer.appendLocal(document);
        const std::vector<std::byte> gap{std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}};
        writer.appendRawBytes(gap);
        const auto centralStart = writer.size();
        writer.appendCentral(manifest, static_cast<std::uint32_t>(manifestOffset));
        writer.appendCentral(document, static_cast<std::uint32_t>(documentOffset));
        const auto centralSize = writer.size() - centralStart;
        writer.appendEocd(0, 0, 2, 2, static_cast<std::uint32_t>(centralSize),
                          static_cast<std::uint32_t>(centralStart));
        const auto result = run(writer.bytes());
        expectations.expect(
            !result.succeeded() &&
                result.error == ZipContainerPreflightError::OverlappingOrUnaccountedByteRange,
            "a gap between the document data and the central directory is rejected");
    }
    {
        // A deflate entry (so the stored-size-consistency check does not intervene) whose
        // declared compressed size claims fewer bytes than are physically written: the next
        // header is expected too early, landing inside the still-live payload bytes.
        auto manifest = makeDeflateEntry("manifest.json", toBytes(std::string(200, 'a')));
        const auto physicalSize = static_cast<std::uint32_t>(manifest.data.size());
        manifest.localCompressedSize = manifest.centralCompressedSize =
            physicalSize > 4 ? physicalSize - 4 : 1;
        const auto archive = buildConformingArchive(manifest, documentEntry());
        const auto result = run(archive);
        expectations.expect(
            !result.succeeded() &&
                result.error == ZipContainerPreflightError::OverlappingOrUnaccountedByteRange,
            "a local range shorter than its physical data overlaps the next header");
    }
}

void testAttributes(Expectations& expectations) {
    {
        auto manifest = manifestEntry();
        manifest.externalAttrs = (0040755U) << 16U; // Unix directory type + mode.
        const auto archive = buildConformingArchive(manifest, documentEntry());
        const auto result = run(archive);
        expectations.expect(!result.succeeded() &&
                                result.error == ZipContainerPreflightError::NonRegularEntry,
                            "a directory external-attribute bit is rejected");
    }
    {
        auto manifest = manifestEntry();
        manifest.externalAttrs = (0120644U) << 16U; // Unix symlink type.
        const auto archive = buildConformingArchive(manifest, documentEntry());
        const auto result = run(archive);
        expectations.expect(!result.succeeded() &&
                                result.error == ZipContainerPreflightError::NonRegularEntry,
                            "Unix symlink file-type bits are rejected");
    }
    {
        auto manifest = manifestEntry();
        manifest.externalAttrs = (0100755U) << 16U; // regular file, executable.
        const auto archive = buildConformingArchive(manifest, documentEntry());
        const auto result = run(archive);
        expectations.expect(!result.succeeded() &&
                                result.error == ZipContainerPreflightError::ExecutableEntry,
                            "an executable mode bit is rejected");
    }
    {
        // DOS host (versionMadeBy high byte 0) with the DOS directory bit set in the low
        // attribute byte and no Unix mode bits present at all.
        auto manifest = manifestEntry();
        manifest.versionMadeBy = 0x0014U; // host 0 (DOS/FAT), version 20.
        manifest.externalAttrs = 0x10U;   // DOS directory attribute bit.
        const auto archive = buildConformingArchive(manifest, documentEntry());
        const auto result = run(archive);
        expectations.expect(!result.succeeded() &&
                                result.error == ZipContainerPreflightError::NonRegularEntry,
                            "a DOS-host directory attribute bit is rejected");
    }
    {
        // The DOS directory bit is checked unconditionally: a crafted entry that declares a Unix
        // host with clean Unix mode type/execute bits must not be able to smuggle the DOS
        // directory bit through in the low byte of the same external-attributes field.
        auto manifest = manifestEntry();
        manifest.versionMadeBy = 0x0314U; // host 3 (Unix), version 20.
        manifest.externalAttrs =
            (0100644U << 16U) | 0x10U; // regular Unix mode + DOS directory bit.
        const auto archive = buildConformingArchive(manifest, documentEntry());
        const auto result = run(archive);
        expectations.expect(!result.succeeded() &&
                                result.error == ZipContainerPreflightError::NonRegularEntry,
                            "the DOS directory bit is rejected even under a declared Unix host "
                            "with otherwise-clean mode bits");
    }
}

void testStoredSizeMismatch(Expectations& expectations) {
    auto manifest = manifestEntry("{\"m\":1}");
    manifest.localUncompressedSize = manifest.centralUncompressedSize =
        manifest.localCompressedSize + 1;
    const auto archive = buildConformingArchive(manifest, documentEntry());
    const auto result = run(archive);
    expectations.expect(!result.succeeded() &&
                            result.error == ZipContainerPreflightError::StoredSizeMismatch,
                        "a stored entry with compressed != uncompressed size is rejected");
}

void testLimitsExceeded(Expectations& expectations) {
    {
        ZipContainerPreflightLimits limits{};
        limits.maxManifestBytes = 3;
        const auto archive = buildConformingArchive(manifestEntry("{\"m\":1}"), documentEntry());
        const auto result = run(archive, limits);
        expectations.expect(!result.succeeded() &&
                                result.error ==
                                    ZipContainerPreflightError::ExpandedSizeLimitExceeded,
                            "a per-entry manifest size above its limit is rejected");
    }
    {
        ZipContainerPreflightLimits limits{};
        limits.maxDocumentBytes = 3;
        const auto archive = buildConformingArchive(manifestEntry(), documentEntry("{\"d\":1}"));
        const auto result = run(archive, limits);
        expectations.expect(!result.succeeded() &&
                                result.error ==
                                    ZipContainerPreflightError::ExpandedSizeLimitExceeded,
                            "a per-entry document size above its limit is rejected");
    }
    {
        ZipContainerPreflightLimits limits{};
        limits.maxTotalExpandedBytes = 10;
        const auto archive =
            buildConformingArchive(manifestEntry("{\"m\":1}"), documentEntry("{\"d\":1}"));
        const auto result = run(archive, limits);
        expectations.expect(
            !result.succeeded() &&
                result.error == ZipContainerPreflightError::ExpandedSizeLimitExceeded,
            "a total expanded size above its limit is rejected even when each entry alone fits");
    }
    {
        // A tiny deflate payload whose declared (but not actually inflated -- preflight never
        // decompresses) uncompressed size claims far more than maxExpansionRatio permits for its
        // real compressed size.
        auto manifest = makeDeflateEntry("manifest.json", toBytes(std::string(10, 'a')));
        manifest.localUncompressedSize = manifest.centralUncompressedSize = 1'000'000;
        const ZipContainerPreflightLimits limits{};
        const auto archive = buildConformingArchive(manifest, documentEntry());
        const auto result = run(archive, limits);
        expectations.expect(!result.succeeded() &&
                                result.error == ZipContainerPreflightError::ExpansionRatioExceeded,
                            "an expansion ratio above its limit is rejected");
    }
}

void testSizeOverflow(Expectations& expectations) {
    ZipContainerPreflightLimits limits{};
    limits.maxExpansionRatio = std::numeric_limits<std::uint64_t>::max();
    auto manifest = manifestEntry("{\"m\":1}"); // compressed size 7, so ratio*7 overflows uint64.
    const auto archive = buildConformingArchive(manifest, documentEntry());
    const auto result = run(archive, limits);
    expectations.expect(
        !result.succeeded() && result.error == ZipContainerPreflightError::SizeOverflow,
        "an expansion-ratio multiply that overflows uint64 is reported as size overflow, "
        "never a wrapped smaller number");
}

} // namespace

int main() {
    Expectations expectations;
    try {
        testConformingArchives(expectations);
        testEmptyPayloadEntry(expectations);
        testInvalidLimits(expectations);
        testArchiveTooLarge(expectations);
        testTruncation(expectations);
        testEocdLocationAndComment(expectations);
        testDiskNumbers(expectations);
        testEntryCount(expectations);
        testEntryNames(expectations);
        testGeneralPurposeFlags(expectations);
        testCompressionMethod(expectations);
        testExtraField(expectations);
        testEntryComment(expectations);
        testZip64Sentinels(expectations);
        testLocalCentralDisagreement(expectations);
        testByteAccounting(expectations);
        testAttributes(expectations);
        testStoredSizeMismatch(expectations);
        testLimitsExceeded(expectations);
        testSizeOverflow(expectations);
    } catch (const std::exception& error) {
        std::cerr << "FAILED: test fixture exception: " << error.what() << '\n';
        return 1;
    }
    return expectations.failures() == 0 ? 0 : 1;
}
