#include <bloom/project/zip_container.hpp>

#include "zip_container_test_support.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <source_location>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using bloom::project::ProjectIoMemoryCoordinator;
using bloom::project::ProjectIoOperationMemory;
using bloom::project::readZipContainer;
using bloom::project::ZipContainerError;
using bloom::project::ZipContainerLimits;
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

constexpr std::uint64_t kGenerousOperationBudget = 32ULL << 20U; // 32 MiB: ample for every fixture.

[[nodiscard]] ProjectIoOperationMemory
makeOperation(const std::uint64_t limitBytes = kGenerousOperationBudget) {
    auto coordinator = ProjectIoMemoryCoordinator::create(limitBytes);
    if (!coordinator.has_value()) {
        throw std::runtime_error("failed to create test memory coordinator");
    }
    auto operation = coordinator->createOperation(limitBytes, limitBytes);
    if (!operation.has_value()) {
        throw std::runtime_error("failed to create test operation memory");
    }
    return std::move(*operation);
}

[[nodiscard]] EntrySpec manifestEntry(const std::string_view payload = "{}") {
    return makeStoredEntry("manifest.json", toBytes(payload));
}

[[nodiscard]] EntrySpec documentEntry(const std::string_view payload = "{}") {
    return makeStoredEntry("document.json", toBytes(payload));
}

[[nodiscard]] std::span<const std::byte> asSpan(const std::vector<std::byte>& bytes) noexcept {
    return {bytes.data(), bytes.size()};
}

void testConformingStoredExtraction(Expectations& expectations) {
    const std::string manifestPayload = "{\"manifest\":true}";
    const std::string documentPayload = "{\"document\":true}";
    const auto archive =
        buildConformingArchive(manifestEntry(manifestPayload), documentEntry(documentPayload));

    const auto operation = makeOperation();
    auto result = readZipContainer(asSpan(archive), ZipContainerLimits{}, operation);
    expectations.expect(static_cast<bool>(result), "a conforming stored archive is accepted");
    if (!result) {
        return;
    }
    const auto* document = result.document();
    expectations.expect(document != nullptr, "a successful result exposes its document");
    if (document == nullptr) {
        return;
    }
    const std::string_view manifestOut(
        reinterpret_cast<const char*>(document->manifestBytes().data()),
        document->manifestBytes().size());
    const std::string_view documentOut(
        reinterpret_cast<const char*>(document->documentBytes().data()),
        document->documentBytes().size());
    expectations.expect(manifestOut == manifestPayload, "manifest bytes are extracted byte-exact");
    expectations.expect(documentOut == documentPayload, "document bytes are extracted byte-exact");

    auto second = readZipContainer(asSpan(archive), ZipContainerLimits{}, makeOperation());
    expectations.expect(static_cast<bool>(second) && second.document() != nullptr,
                        "extracting the same archive twice succeeds both times");
    if (second.document() != nullptr) {
        const std::string_view secondManifest(
            reinterpret_cast<const char*>(second.document()->manifestBytes().data()),
            second.document()->manifestBytes().size());
        expectations.expect(secondManifest == manifestPayload,
                            "extraction is byte-for-byte deterministic across two runs");
    }
}

void testConformingDeflateAndMixedExtraction(Expectations& expectations) {
    {
        const std::string manifestPayload(4000, 'm');
        const std::string documentPayload(6000, 'd');
        const auto archive =
            buildConformingArchive(makeDeflateEntry("manifest.json", toBytes(manifestPayload)),
                                   makeDeflateEntry("document.json", toBytes(documentPayload)));
        auto result = readZipContainer(asSpan(archive), ZipContainerLimits{}, makeOperation());
        expectations.expect(static_cast<bool>(result),
                            "a conforming deflate-only archive is accepted");
        if (result && result.document() != nullptr) {
            const std::string_view manifestOut(
                reinterpret_cast<const char*>(result.document()->manifestBytes().data()),
                result.document()->manifestBytes().size());
            expectations.expect(manifestOut == manifestPayload,
                                "a deflated manifest entry inflates to its exact original bytes");
        }
    }
    {
        const std::string documentPayload(2500, 'x');
        const auto archive = buildConformingArchive(
            manifestEntry("{}"), makeDeflateEntry("document.json", toBytes(documentPayload)));
        auto result = readZipContainer(asSpan(archive), ZipContainerLimits{}, makeOperation());
        expectations.expect(static_cast<bool>(result),
                            "a mixed stored/deflate archive is accepted");
        if (result && result.document() != nullptr) {
            const std::string_view documentOut(
                reinterpret_cast<const char*>(result.document()->documentBytes().data()),
                result.document()->documentBytes().size());
            expectations.expect(documentOut == documentPayload,
                                "the deflated entry in a mixed archive still inflates exactly");
        }
    }
}

void testEmptyPayloadExtraction(Expectations& expectations) {
    const auto archive = buildConformingArchive(manifestEntry(""), documentEntry("{}"));
    auto result = readZipContainer(asSpan(archive), ZipContainerLimits{}, makeOperation());
    expectations.expect(static_cast<bool>(result),
                        "an archive with a zero-byte manifest is accepted");
    if (result && result.document() != nullptr) {
        expectations.expect(result.document()->manifestBytes().empty(),
                            "the zero-byte manifest entry extracts to an empty buffer");
    }
}

void testPreflightRejectionPropagates(Expectations& expectations) {
    auto manifest = manifestEntry();
    manifest.localName = manifest.centralName = "Manifest.json";
    const auto archive = buildConformingArchive(manifest, documentEntry());
    const auto result = readZipContainer(asSpan(archive), ZipContainerLimits{}, makeOperation());
    expectations.expect(
        !result && result.error() == ZipContainerError::WrongEntryName,
        "a preflight-stage rejection surfaces through the public API with its exact "
        "translated error");
}

void testInvalidLimits(Expectations& expectations) {
    ZipContainerLimits limits{};
    limits.maxExpansionRatio = 0;
    const auto archive = buildConformingArchive(manifestEntry(), documentEntry());
    const auto result = readZipContainer(asSpan(archive), limits, makeOperation());
    expectations.expect(!result && result.error() == ZipContainerError::InvalidLimits,
                        "a zeroed public limit field is rejected before preflight runs");
}

void testExpandedSizeMismatchOnBomb(Expectations& expectations) {
    const std::string realPayload(20000, 'a'); // deflates to a tiny stream.
    auto manifest = makeDeflateEntry("manifest.json", toBytes(realPayload));
    // Declare fewer bytes than the stream actually inflates to; the destination buffer is sized
    // to this declared (smaller) value and can never be overrun, but the extra decompressed byte
    // must still be caught.
    const auto declared = static_cast<std::uint32_t>(realPayload.size() - 100);
    manifest.localUncompressedSize = manifest.centralUncompressedSize = declared;
    const auto archive = buildConformingArchive(manifest, documentEntry());

    ZipContainerLimits limits{};
    const auto result = readZipContainer(asSpan(archive), limits, makeOperation());
    expectations.expect(
        !result && result.error() == ZipContainerError::ExpandedSizeMismatch,
        "a stream that inflates past its declared size is rejected as an expanded-size "
        "mismatch rather than overrunning the destination buffer");
}

void testUnderProductionMismatch(Expectations& expectations) {
    // The mirror of the bomb case: a genuinely valid raw-deflate stream that inflates to FEWER
    // bytes than declared. The preflight cannot see this (it never decompresses); the ratio check
    // stays legal (200 declared / a handful of compressed bytes is far under the default 1000:1
    // ceiling); local and central headers agree; CRC names the real 100 produced bytes.
    // Empirically observed: libzip's zip_fread() for the main exact-size read returns the true
    // available byte count (100) rather than the requested 200, which this code reports as a
    // short read -- ExpandedSizeMismatch, deterministically, before any independent CRC check
    // runs (there is no complete buffer to check yet).
    const std::string realPayload(100, 'a');
    auto manifest = makeDeflateEntry("manifest.json", toBytes(realPayload));
    manifest.localUncompressedSize = manifest.centralUncompressedSize = 200;
    const auto archive = buildConformingArchive(manifest, documentEntry());

    const auto result = readZipContainer(asSpan(archive), ZipContainerLimits{}, makeOperation());
    expectations.expect(
        !result && result.error() == ZipContainerError::ExpandedSizeMismatch,
        "a valid deflate stream that inflates to fewer bytes than declared is rejected as an "
        "expanded-size mismatch via the short main read");
}

void testQualifiedReaderDisagreementOnInvalidDeflateStream(Expectations& expectations) {
    // A method-8 entry whose "compressed" bytes are not a valid raw-deflate stream at all. The
    // preflight only inspects header fields (name/flags/method/sizes/CRC), never entry payload
    // bytes, so a self-consistent local/central header pair with a legal declared ratio passes it
    // cleanly; the corruption can only be discovered once the qualified reader actually attempts
    // to decompress. Empirically observed: libzip's zip_fread() for the main exact-size read
    // returns -1 (a genuine read error, not a short read) because the garbage bytes cannot be
    // inflated at all -- this code reports that as QualifiedReaderDisagreement, deterministically,
    // since it is libzip failing outright rather than the buffer-size accounting disagreeing.
    EntrySpec manifest;
    manifest.localName = "manifest.json";
    manifest.centralName = "manifest.json";
    manifest.localMethod = manifest.centralMethod = 8;
    // Fixed, deterministic "garbage" payload: not a valid raw-deflate stream, but a legal byte
    // sequence to write and account for structurally.
    std::vector<std::byte> garbage(64);
    for (std::size_t index = 0; index < garbage.size(); ++index) {
        garbage[index] = static_cast<std::byte>((index * 97U + 13U) & 0xFFU);
    }
    manifest.data = garbage;
    manifest.localCompressedSize = manifest.centralCompressedSize = 64;
    // Declared uncompressed size passes the default 1000:1 ratio ceiling (100 / 64 is nowhere
    // close) and both header limits comfortably.
    manifest.localUncompressedSize = manifest.centralUncompressedSize = 100;
    manifest.localCrc = manifest.centralCrc = 0xDEADBEEFU;
    const auto archive = buildConformingArchive(manifest, documentEntry());

    const auto result = readZipContainer(asSpan(archive), ZipContainerLimits{}, makeOperation());
    expectations.expect(
        !result && result.error() == ZipContainerError::QualifiedReaderDisagreement,
        "a method-8 entry whose data is not a valid deflate stream is rejected as a qualified "
        "reader disagreement once libzip's own decompression fails");
}

void testCrcMismatch(Expectations& expectations) {
    {
        auto manifest = manifestEntry("{\"m\":1}");
        manifest.localCrc = manifest.centralCrc ^= 0xFFFFFFFFU;
        const auto archive = buildConformingArchive(manifest, documentEntry());
        const auto result =
            readZipContainer(asSpan(archive), ZipContainerLimits{}, makeOperation());
        expectations.expect(!result && result.error() == ZipContainerError::CrcMismatch,
                            "a wrong declared CRC value is rejected");
    }
    {
        auto manifest = manifestEntry("{\"m\":1}");
        ArchiveWriter writer;
        const auto manifestOffset = writer.appendLocal(manifest);
        const auto document = documentEntry();
        const auto documentOffset = writer.appendLocal(document);
        const auto centralStart = writer.size();
        writer.appendCentral(manifest, static_cast<std::uint32_t>(manifestOffset));
        writer.appendCentral(document, static_cast<std::uint32_t>(documentOffset));
        const auto centralSize = writer.size() - centralStart;
        writer.appendEocd(0, 0, 2, 2, static_cast<std::uint32_t>(centralSize),
                          static_cast<std::uint32_t>(centralStart));
        auto bytes = writer.bytes();
        // Corrupt one payload byte in place (stored: compressed bytes equal the payload bytes)
        // while the declared CRC keeps naming the original, uncorrupted content. The data region
        // starts right after the fixed 30-byte local header, the name, and the (empty) extra field.
        const auto manifestDataOffset =
            manifestOffset + 30 + manifest.localName.size() + manifest.localExtra.size();
        bytes[manifestDataOffset] ^= std::byte{0x01};
        const auto result = readZipContainer(asSpan(bytes), ZipContainerLimits{}, makeOperation());
        expectations.expect(!result && result.error() == ZipContainerError::CrcMismatch,
                            "corrupted payload bytes are rejected even when every header field is "
                            "internally consistent");
    }
}

void testBudgetExhaustion(Expectations& expectations) {
    const auto archive =
        buildConformingArchive(manifestEntry("{\"m\":1}"), documentEntry("{\"d\":1}"));
    {
        const auto operation = makeOperation(1024); // below even the working reservation.
        auto result = readZipContainer(asSpan(archive), ZipContainerLimits{}, operation);
        expectations.expect(!result && result.error() == ZipContainerError::ResourceExhausted,
                            "a budget far below the required working reservation is rejected");
        expectations.expect(operation.snapshot().currentBytes == 0,
                            "a rejected operation leaves no leaked charge behind");
    }
    {
        // Large enough for the working reservation to succeed, too small for the destination
        // buffers: exercises the reservation being released again after a later allocation fails.
        constexpr std::uint64_t kJustOverWorkingReservation = (4ULL << 20U) + 8;
        const auto operation = makeOperation(kJustOverWorkingReservation);
        auto result = readZipContainer(asSpan(archive), ZipContainerLimits{}, operation);
        expectations.expect(!result && result.error() == ZipContainerError::ResourceExhausted,
                            "a budget that only covers the working reservation still fails on the "
                            "destination buffers");
        expectations.expect(operation.snapshot().currentBytes == 0,
                            "the working reservation is released even after it succeeded first");
    }
}

} // namespace

int main() {
    Expectations expectations;
    try {
        testConformingStoredExtraction(expectations);
        testConformingDeflateAndMixedExtraction(expectations);
        testEmptyPayloadExtraction(expectations);
        testPreflightRejectionPropagates(expectations);
        testInvalidLimits(expectations);
        testExpandedSizeMismatchOnBomb(expectations);
        testUnderProductionMismatch(expectations);
        testQualifiedReaderDisagreementOnInvalidDeflateStream(expectations);
        testCrcMismatch(expectations);
        testBudgetExhaustion(expectations);
    } catch (const std::exception& error) {
        std::cerr << "FAILED: test fixture exception: " << error.what() << '\n';
        return 1;
    }
    return expectations.failures() == 0 ? 0 : 1;
}
