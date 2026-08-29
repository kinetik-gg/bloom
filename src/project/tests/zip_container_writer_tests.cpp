#include <bloom/project/zip_container.hpp>
#include <bloom/project/zip_container_writer.hpp>

#include "zip_container_preflight.hpp"
#include "zip_container_test_support.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
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
using bloom::project::writeZipContainer;
using bloom::project::ZipContainerLimits;
using bloom::project::ZipContainerWriteEntry;
using bloom::project::ZipContainerWriteError;
using bloom::project::detail::preflightZipContainer;
using bloom::project::detail::ZipContainerPreflightLimits;
using bloom::project::test::crc32Of;
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

constexpr std::uint64_t kGenerousOperationBudget = 16ULL << 20U; // 16 MiB: ample for every fixture.

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

[[nodiscard]] std::span<const std::byte> asSpan(const std::vector<std::byte>& bytes) noexcept {
    return {bytes.data(), bytes.size()};
}

// A deterministic, high-avalanche byte stream (splitmix64) standing in for "incompressible data"
// -- no std::random_device or time-based seed, so the fixture is exactly reproducible.
[[nodiscard]] std::uint64_t splitmix64Next(std::uint64_t& state) noexcept {
    state += 0x9E3779B97F4A7C15ULL;
    std::uint64_t mixed = state;
    mixed = (mixed ^ (mixed >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    mixed = (mixed ^ (mixed >> 27U)) * 0x94D049BB133111EBULL;
    return mixed ^ (mixed >> 31U);
}

[[nodiscard]] std::vector<std::byte> pseudoRandomBytes(const std::size_t count,
                                                       const std::uint64_t seed) {
    std::vector<std::byte> out;
    out.reserve(count);
    std::uint64_t state = seed;
    while (out.size() < count) {
        const auto value = splitmix64Next(state);
        for (unsigned shift = 0; shift < 64U && out.size() < count; shift += 8U) {
            out.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
        }
    }
    return out;
}

// Hand-derives the exact expected bytes of a two-entry, both-stored Constrained ZIP Profile
// archive directly from the layout rules in docs/architecture/project-format.md, independent of
// the production writer, so the golden-bytes test proves the writer's actual field values rather
// than its own logic against itself.
//
// Field map (identical for both entries' local and central headers):
//   local header (30 bytes fixed + name):
//     signature(4)=0x04034b50 | versionNeeded(2)=20 | flags(2)=0x0800 (UTF-8, exact)
//     method(2)=0 (stored) | modTime(2)=0x0000 | modDate(2)=0x0021 (DOS epoch 1980-01-01)
//     crc32(4) | compressedSize(4)=payload size | uncompressedSize(4)=payload size
//     nameLength(2) | extraLength(2)=0 | name | payload bytes
//   central header (46 bytes fixed + name):
//     signature(4)=0x02014b50 | versionMadeBy(2)=0x0314 (Unix host 3, version 20)
//     versionNeeded(2)=20 | flags(2)=0x0800 | method(2)=0 | modTime/modDate as above
//     crc32(4) | compressedSize(4) | uncompressedSize(4) | nameLength(2)
//     extraLength(2)=0 | commentLength(2)=0 | diskNumberStart(2)=0 | internalAttrs(2)=0
//     externalAttrs(4)=0100644<<16 (regular, non-executable) | localHeaderOffset(4) | name
//   EOCD (22 bytes fixed): signature(4)=0x06054b50 | diskNumber/diskWithCd(2+2)=0
//     entriesThisDisk/entriesTotal(2+2)=2/2 | cdSize(4) | cdOffset(4) | commentLength(2)=0
[[nodiscard]] std::vector<std::byte>
buildExpectedStoredArchive(const std::string_view manifestPayload,
                           const std::string_view documentPayload) {
    constexpr std::string_view kManifestName = "manifest.json";
    constexpr std::string_view kDocumentName = "document.json";
    constexpr std::uint16_t kVersionNeeded = 20;
    constexpr std::uint16_t kFlags = 0x0800;
    constexpr std::uint16_t kMethodStored = 0;
    constexpr std::uint16_t kModTime = 0x0000;
    constexpr std::uint16_t kModDate = 0x0021;
    constexpr std::uint16_t kVersionMadeBy = 0x0314;
    constexpr std::uint32_t kExternalAttrs = 0100644U << 16U;

    std::vector<std::byte> out;
    const auto putU16 = [&out](const std::uint16_t value) {
        out.push_back(static_cast<std::byte>(value & 0xFFU));
        out.push_back(static_cast<std::byte>((value >> 8U) & 0xFFU));
    };
    const auto putU32 = [&out](const std::uint32_t value) {
        for (unsigned shift = 0; shift < 32U; shift += 8U) {
            out.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
        }
    };
    const auto putText = [&out](const std::string_view text) {
        for (const char character : text) {
            out.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
        }
    };
    const auto putLocalHeader = [&](const std::string_view name, const std::string_view payload,
                                    const std::uint32_t crc) {
        putU32(0x04034b50U);
        putU16(kVersionNeeded);
        putU16(kFlags);
        putU16(kMethodStored);
        putU16(kModTime);
        putU16(kModDate);
        putU32(crc);
        putU32(static_cast<std::uint32_t>(payload.size()));
        putU32(static_cast<std::uint32_t>(payload.size()));
        putU16(static_cast<std::uint16_t>(name.size()));
        putU16(0);
        putText(name);
        putText(payload);
    };
    const auto putCentralHeader = [&](const std::string_view name, const std::string_view payload,
                                      const std::uint32_t crc,
                                      const std::uint32_t localHeaderOffset) {
        putU32(0x02014b50U);
        putU16(kVersionMadeBy);
        putU16(kVersionNeeded);
        putU16(kFlags);
        putU16(kMethodStored);
        putU16(kModTime);
        putU16(kModDate);
        putU32(crc);
        putU32(static_cast<std::uint32_t>(payload.size()));
        putU32(static_cast<std::uint32_t>(payload.size()));
        putU16(static_cast<std::uint16_t>(name.size()));
        putU16(0);
        putU16(0);
        putU16(0);
        putU16(0);
        putU32(kExternalAttrs);
        putU32(localHeaderOffset);
        putText(name);
    };

    const auto manifestCrc = crc32Of(toBytes(manifestPayload));
    const auto documentCrc = crc32Of(toBytes(documentPayload));

    const auto manifestLocalOffset = static_cast<std::uint32_t>(out.size());
    putLocalHeader(kManifestName, manifestPayload, manifestCrc);
    const auto documentLocalOffset = static_cast<std::uint32_t>(out.size());
    putLocalHeader(kDocumentName, documentPayload, documentCrc);

    const auto centralStart = static_cast<std::uint32_t>(out.size());
    putCentralHeader(kManifestName, manifestPayload, manifestCrc, manifestLocalOffset);
    putCentralHeader(kDocumentName, documentPayload, documentCrc, documentLocalOffset);
    const auto centralEnd = static_cast<std::uint32_t>(out.size());

    putU32(0x06054b50U);
    putU16(0);
    putU16(0);
    putU16(2);
    putU16(2);
    putU32(centralEnd - centralStart);
    putU32(centralStart);
    putU16(0);

    return out;
}

void testGoldenBytesAndDeterminism(Expectations& expectations) {
    // "{}" deflates to more bytes than it is (raw-deflate overhead exceeds two source bytes), so
    // the stored-fallback rule naturally picks method 0 for both entries -- a genuine exercise of
    // the fallback logic, not a hand-picked stored-only shortcut.
    const std::string manifestPayload = "{}";
    const std::string documentPayload = "{}";
    const auto expected = buildExpectedStoredArchive(manifestPayload, documentPayload);

    auto first = writeZipContainer(toBytes(manifestPayload), toBytes(documentPayload),
                                   ZipContainerLimits{}, makeOperation());
    expectations.expect(static_cast<bool>(first), "a minimal two-entry archive is written");
    if (first && first.archive() != nullptr) {
        const auto bytes = first.archive()->bytes();
        expectations.expect(
            std::equal(bytes.begin(), bytes.end(), expected.begin(), expected.end()),
            "the minimal archive matches the hand-derived golden bytes exactly");
    }

    auto second = writeZipContainer(toBytes(manifestPayload), toBytes(documentPayload),
                                    ZipContainerLimits{}, makeOperation());
    if (first.archive() != nullptr && second.archive() != nullptr) {
        const auto bytes1 = first.archive()->bytes();
        const auto bytes2 = second.archive()->bytes();
        expectations.expect(std::equal(bytes1.begin(), bytes1.end(), bytes2.begin(), bytes2.end()),
                            "two stored-entry writes of the same input produce byte-identical "
                            "archives");
    }

    // Determinism also holds for deflate output: same qualified zlib, same level, same input.
    const std::string compressible(4000, 'z');
    auto deflateFirst = writeZipContainer(toBytes(compressible), toBytes(compressible),
                                          ZipContainerLimits{}, makeOperation());
    auto deflateSecond = writeZipContainer(toBytes(compressible), toBytes(compressible),
                                           ZipContainerLimits{}, makeOperation());
    if (deflateFirst && deflateSecond && deflateFirst.archive() != nullptr &&
        deflateSecond.archive() != nullptr) {
        const auto bytes1 = deflateFirst.archive()->bytes();
        const auto bytes2 = deflateSecond.archive()->bytes();
        expectations.expect(std::equal(bytes1.begin(), bytes1.end(), bytes2.begin(), bytes2.end()),
                            "two deflate-entry writes of the same input produce byte-identical "
                            "archives");
    }
}

void expectRoundTrip(Expectations& expectations, const std::span<const std::byte> manifest,
                     const std::span<const std::byte> document, const std::string_view writeMessage,
                     const std::string_view readMessage, const std::string_view manifestMessage,
                     const std::string_view documentMessage) {
    auto writeResult = writeZipContainer(manifest, document, ZipContainerLimits{}, makeOperation());
    expectations.expect(static_cast<bool>(writeResult), writeMessage);
    if (!writeResult || writeResult.archive() == nullptr) {
        return;
    }
    const auto archiveBytes = writeResult.archive()->bytes();
    auto readResult = readZipContainer(archiveBytes, ZipContainerLimits{}, makeOperation());
    expectations.expect(static_cast<bool>(readResult), readMessage);
    if (!readResult || readResult.document() == nullptr) {
        return;
    }
    const auto readManifest = readResult.document()->manifestBytes();
    const auto readDocument = readResult.document()->documentBytes();
    expectations.expect(
        std::equal(readManifest.begin(), readManifest.end(), manifest.begin(), manifest.end()),
        manifestMessage);
    expectations.expect(
        std::equal(readDocument.begin(), readDocument.end(), document.begin(), document.end()),
        documentMessage);
}

void testRoundTripStored(Expectations& expectations) {
    const std::string manifestPayload = "{\"manifest\":true}";
    const std::string documentPayload = "{\"document\":true}";
    expectRoundTrip(expectations, toBytes(manifestPayload), toBytes(documentPayload),
                    "a small stored-eligible archive is written",
                    "the stored archive is accepted by the reader",
                    "the stored manifest round-trips byte-exact",
                    "the stored document round-trips byte-exact");
}

void testRoundTripDeflate(Expectations& expectations) {
    // Moderately compressible JSON-shaped text: deflate shrinks it well under the 1000:1 ceiling,
    // so this exercises the ordinary (non-fallback) deflate path.
    std::string payload;
    payload.reserve(6000);
    for (int index = 0; index < 200; ++index) {
        payload += "{\"key\":\"value\",\"index\":" + std::to_string(index % 37) + "},";
    }
    expectRoundTrip(expectations, toBytes(payload), toBytes(payload),
                    "a compressible deflate-eligible archive is written",
                    "the deflate archive is accepted by the reader",
                    "the deflated manifest round-trips byte-exact",
                    "the deflated document round-trips byte-exact");
}

void testRoundTripMixed(Expectations& expectations) {
    const std::string manifestPayload = "{}"; // stores naturally.
    std::string documentPayload;
    documentPayload.reserve(5000);
    for (int index = 0; index < 150; ++index) {
        documentPayload += "{\"layer\":\"solid\",\"n\":" + std::to_string(index % 11) + "},";
    }
    expectRoundTrip(expectations, toBytes(manifestPayload), toBytes(documentPayload),
                    "a mixed stored/deflate archive is written",
                    "the mixed archive is accepted by the reader",
                    "the stored manifest in a mixed archive round-trips byte-exact",
                    "the deflated document in a mixed archive round-trips byte-exact");
}

void testRoundTripEmptyPayload(Expectations& expectations) {
    expectRoundTrip(expectations, toBytes(""), toBytes("{}"),
                    "an archive with a zero-byte manifest is written",
                    "the empty-manifest archive is accepted by the reader",
                    "the zero-byte manifest round-trips to an empty buffer",
                    "the document alongside an empty manifest round-trips byte-exact");
    expectRoundTrip(expectations, toBytes("{}"), toBytes(""),
                    "an archive with a zero-byte document is written",
                    "the empty-document archive is accepted by the reader",
                    "the manifest alongside an empty document round-trips byte-exact",
                    "the zero-byte document round-trips to an empty buffer");
}

void expectStoredFallback(Expectations& expectations, const std::span<const std::byte> manifest,
                          const std::span<const std::byte> document,
                          const bool expectManifestStored, const bool expectDocumentStored,
                          const std::string_view writeMessage, const std::string_view methodMessage,
                          const std::string_view readMessage) {
    auto writeResult = writeZipContainer(manifest, document, ZipContainerLimits{}, makeOperation());
    expectations.expect(static_cast<bool>(writeResult), writeMessage);
    if (!writeResult || writeResult.archive() == nullptr) {
        return;
    }
    const auto archiveBytes = writeResult.archive()->bytes();

    const auto preflight = preflightZipContainer(archiveBytes, ZipContainerPreflightLimits{});
    expectations.expect(preflight.succeeded(), "the fallback archive's own header layout is well "
                                               "formed");
    if (preflight.succeeded()) {
        if (expectManifestStored) {
            expectations.expect(preflight.manifest.method == 0, methodMessage);
        }
        if (expectDocumentStored) {
            expectations.expect(preflight.document.method == 0, methodMessage);
        }
    }

    auto readResult = readZipContainer(archiveBytes, ZipContainerLimits{}, makeOperation());
    expectations.expect(static_cast<bool>(readResult), readMessage);
}

void testStoredFallbackIncompressiblePayload(Expectations& expectations) {
    // Trigger (a): deflate does not reduce a genuinely incompressible payload, so the manifest
    // entry stores instead (well under the default 1 MiB manifest cap).
    const auto manifest = pseudoRandomBytes(65536, 0x1234'5678'9ABC'DEF0ULL);
    const std::string document = "{}";
    expectStoredFallback(
        expectations, asSpan(manifest), toBytes(document), /*expectManifestStored=*/true,
        /*expectDocumentStored=*/false,
        "an archive with an incompressible manifest payload is written",
        "the incompressible manifest chose stored (method 0) because deflate did not reduce it",
        "the incompressible-manifest archive is accepted by the reader");
}

void testStoredFallbackHyperCompressiblePayload(Expectations& expectations) {
    // Trigger (b): 2 MiB of one repeated byte deflates far below the 1000:1 ceiling, so the
    // document entry stores instead even though deflate would shrink it. Placed in the document
    // entry because the default manifest cap (1 MiB) is smaller than this payload.
    const std::string manifest = "{}";
    const std::vector<std::byte> document(2ULL * 1024 * 1024, std::byte{'A'});
    expectStoredFallback(
        expectations, toBytes(manifest), asSpan(document), /*expectManifestStored=*/false,
        /*expectDocumentStored=*/true,
        "an archive with a hyper-compressible document payload is written",
        "the hyper-compressible document chose stored (method 0) because deflate would exceed "
        "the expansion-ratio ceiling",
        "the hyper-compressible-document archive is accepted by the reader");
}

void testRefusalInvalidLimits(Expectations& expectations) {
    ZipContainerLimits limits{};
    limits.maxExpansionRatio = 0;
    auto result = writeZipContainer(toBytes("{}"), toBytes("{}"), limits, makeOperation());
    expectations.expect(!result && result.error() == ZipContainerWriteError::InvalidLimits,
                        "a zeroed limit field is rejected before any work begins");
}

void testRefusalEntrySizeLimitExceeded(Expectations& expectations) {
    {
        ZipContainerLimits limits{};
        limits.maxManifestBytes = 4;
        const std::string manifestPayload = "12345";
        auto result =
            writeZipContainer(toBytes(manifestPayload), toBytes("{}"), limits, makeOperation());
        expectations.expect(
            !result && result.error() == ZipContainerWriteError::EntrySizeLimitExceeded &&
                result.entryInError() == ZipContainerWriteEntry::Manifest,
            "a manifest payload over its per-entry cap is refused and names the manifest entry");
    }
    {
        ZipContainerLimits limits{};
        limits.maxDocumentBytes = 4;
        const std::string documentPayload = "12345";
        auto result =
            writeZipContainer(toBytes("{}"), toBytes(documentPayload), limits, makeOperation());
        expectations.expect(
            !result && result.error() == ZipContainerWriteError::EntrySizeLimitExceeded &&
                result.entryInError() == ZipContainerWriteEntry::Document,
            "a document payload over its per-entry cap is refused and names the document entry");
    }
}

void testRefusalTotalExpandedLimitExceeded(Expectations& expectations) {
    ZipContainerLimits limits{};
    limits.maxManifestBytes = 10;
    limits.maxDocumentBytes = 10;
    limits.maxTotalExpandedBytes = 15; // below 10 + 10, above either individual cap.
    const std::string manifestPayload(10, 'm');
    const std::string documentPayload(10, 'd');
    auto result = writeZipContainer(toBytes(manifestPayload), toBytes(documentPayload), limits,
                                    makeOperation());
    expectations.expect(
        !result && result.error() == ZipContainerWriteError::TotalExpandedLimitExceeded,
        "a sum over the total-expanded cap is refused even though each entry fits its own cap");
}

void testRefusalArchiveSizeLimitExceeded(Expectations& expectations) {
    ZipContainerLimits limits{};
    limits.maxArchiveBytes = 50; // well under the ~230-byte minimum two-entry archive overhead.
    auto result = writeZipContainer(toBytes("{}"), toBytes("{}"), limits, makeOperation());
    expectations.expect(!result &&
                            result.error() == ZipContainerWriteError::ArchiveSizeLimitExceeded,
                        "an assembled archive over the archive-size cap is refused");
}

void testRefusalSizeOverflow(Expectations& expectations) {
    ZipContainerLimits limits{};
    limits.maxManifestBytes = std::numeric_limits<std::uint64_t>::max();
    // A span reporting a size past the 32-bit ZIP field ceiling, backed by a tiny real buffer.
    // The writer's 32-bit-fit check is the very first thing it does with this size and never
    // dereferences payload bytes before returning, so this never reads past the real allocation.
    constexpr std::array<std::byte, 4> tinyBacking{};
    const std::span<const std::byte> oversizedManifest(
        tinyBacking.data(), std::uint64_t{std::numeric_limits<std::uint32_t>::max()} + 1);
    auto result = writeZipContainer(oversizedManifest, toBytes("{}"), limits, makeOperation());
    expectations.expect(
        !result && result.error() == ZipContainerWriteError::SizeOverflow,
        "a declared payload size that cannot fit the 32-bit ZIP size fields is refused as "
        "overflow rather than silently truncated");
}

void testBudgetExhaustion(Expectations& expectations) {
    const std::string manifestPayload = "{\"m\":1}";
    const std::string documentPayload = "{\"d\":1}";
    {
        const auto operation = makeOperation(1024); // below even the working reservation.
        auto result = writeZipContainer(toBytes(manifestPayload), toBytes(documentPayload),
                                        ZipContainerLimits{}, operation);
        expectations.expect(!result && result.error() == ZipContainerWriteError::ResourceExhausted,
                            "a budget far below the required working reservation is rejected");
        expectations.expect(operation.snapshot().currentBytes == 0,
                            "a rejected operation leaves no leaked charge behind");
    }
    {
        // Large enough for the working reservation to succeed, too small for the destination
        // buffers: exercises the reservation being released again after a later allocation
        // fails.
        constexpr std::uint64_t kJustOverWorkingReservation = (4ULL << 20U) + 8;
        const auto operation = makeOperation(kJustOverWorkingReservation);
        auto result = writeZipContainer(toBytes(manifestPayload), toBytes(documentPayload),
                                        ZipContainerLimits{}, operation);
        expectations.expect(!result && result.error() == ZipContainerWriteError::ResourceExhausted,
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
        testGoldenBytesAndDeterminism(expectations);
        testRoundTripStored(expectations);
        testRoundTripDeflate(expectations);
        testRoundTripMixed(expectations);
        testRoundTripEmptyPayload(expectations);
        testStoredFallbackIncompressiblePayload(expectations);
        testStoredFallbackHyperCompressiblePayload(expectations);
        testRefusalInvalidLimits(expectations);
        testRefusalEntrySizeLimitExceeded(expectations);
        testRefusalTotalExpandedLimitExceeded(expectations);
        testRefusalArchiveSizeLimitExceeded(expectations);
        testRefusalSizeOverflow(expectations);
        testBudgetExhaustion(expectations);
    } catch (const std::exception& error) {
        std::cerr << "FAILED: test fixture exception: " << error.what() << '\n';
        return 1;
    }
    return expectations.failures() == 0 ? 0 : 1;
}
