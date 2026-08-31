#pragma once

#include <bloom/color/display_processor_identity.hpp>
#include <bloom/core/sha256.hpp>
#include <bloom/output/output_analysis.hpp>
#include <bloom/output/png_output_adapter.hpp>
#include <bloom/output/process_frame_semantic_identity.hpp>
#include <bloom/runtime/cancellation.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace bloom::output {

// Reported between bounded row-chunks during IDAT decode/comparison. Lets a caller observe
// monotonic verify progress and, in tests, deterministically synchronize a mid-verify cancellation
// instead of racing a background thread against an unobserved internal loop.
struct PngVerifyProgressV1 final {
    std::uint64_t completedRows = 0;
    std::uint64_t totalRows = 0;

    friend bool operator==(const PngVerifyProgressV1&,
                           const PngVerifyProgressV1&) noexcept = default;
};

using PngVerifyProgressCallbackV1 = std::function<void(const PngVerifyProgressV1&)>;

enum class PngVerifyStatusV1 : std::uint8_t {
    Verified,
    Cancelled,
    Failed,
};

// Every code below is a typed diagnosis of a specific reopen-verification step, in the order
// docs/architecture/frame-output.md's "PNG Preset Version 1" and "Determinism And Portable Output
// Identity" sections require and design decision 3 restates: signature, then the exact chunk
// sequence with every CRC valid, then IHDR/sRGB field values, then the IDAT zlib stream decoded
// within checked ceilings with every row filter byte 0, then (only once every prior check passes)
// bit-exact sample comparison against the caller-supplied prepared stream, then kind-1 semantic
// identity issuance.
enum class PngVerifyErrorCodeV1 : std::uint8_t {
    None,
    // The staged path could not be opened/read at all before signature parsing began.
    SourceUnavailable,
    // Fewer than 8 bytes, or a chunk header/data/CRC was cut off mid-chunk.
    TruncatedFile,
    // The first 8 bytes are not the exact PNG signature.
    InvalidSignature,
    // A declared chunk length exceeds the checked per-chunk ceiling before its data is read.
    ChunkLengthExceedsLimit,
    // A chunk's type is not the one required at this point in the closed sequence
    // IHDR -> sRGB -> IDAT+ -> IEND -- names the unexpected type (an inserted foreign ancillary or
    // critical chunk, a chunk out of order, or IDAT/IEND appearing before any IDAT was seen).
    UnexpectedChunkType,
    // IHDR/sRGB/IEND's declared length is not its exact required fixed size (13/1/0 bytes).
    FixedChunkLengthMismatch,
    // End of file reached before at least one IDAT, or before IEND.
    MissingChunk,
    // A chunk's stored CRC-32 does not match the CRC-32 computed over its type and data.
    ChunkCrcMismatch,
    // A decoded IHDR field (width, height, bit depth, color type, compression method, filter
    // method, or interlace method) does not equal the required/expected value.
    IhdrFieldMismatch,
    // The decoded sRGB rendering-intent byte is not exactly 0.
    SrgbIntentMismatch,
    // The concatenated IDAT payload is not a well-formed zlib (deflate) stream (bad header,
    // corrupt compressed data, or an incomplete stream).
    IdatZlibStreamInvalid,
    // The IDAT stream would inflate to more bytes than the checked expected ceiling
    // (width*height*4 + height filter bytes) -- a zlib-bomb shape, rejected as a resource failure
    // before decoding past that bound.
    IdatExpandedSizeExceeded,
    // A decoded scanline's leading filter byte is not exactly 0 (None).
    RowFilterByteNonzero,
    // A decoded RGBA8 sample differs from the caller-supplied prepared stream.
    SampleMismatch,
    // Bytes remain in the file after the IEND chunk.
    TrailingBytesAfterIend,
    // Dimensions/pixel/byte counts exceed output_limits.hpp's closed version 1 ceilings.
    ResourceLimitExceeded,
    // Every structural and sample check passed, but binding or issuing the kind-1
    // `OutputSemanticIdentity` from the decoded values failed -- an input-construction problem
    // (e.g. a null/mismatched processIdentity/report/displayProcessorIdentity) rather than a file-
    // content defect.
    IdentityIssuanceFailed,
    AllocationFailure,
    InternalInvariant,
};

// Names the exact location of a Failed diagnosis. Fields are populated only for the codes that
// name a location; an unused field stays at its default.
struct PngVerifyDiagnosticV1 final {
    PngVerifyErrorCodeV1 code = PngVerifyErrorCodeV1::None;
    std::string chunkType;                   // the 4-byte ASCII chunk type, when applicable
    std::optional<std::uint64_t> chunkIndex; // zero-based chunk occurrence, when applicable
    std::optional<std::uint64_t> byteOffset; // file offset the defective chunk started at
    std::optional<std::uint64_t> row;        // zero-based scanline, when applicable

    friend bool operator==(const PngVerifyDiagnosticV1&, const PngVerifyDiagnosticV1&) = default;
};

// `OutputSemanticIdentityV1` itself is module-private (docs/architecture/frame-output.md: "the
// module-private Output Semantic Identity version 1 streaming serializer/preparer"), so this
// public result surfaces only the one thing design decision 4 asks for: the resulting digest.
class [[nodiscard]] PngVerifyResultV1 final {
  public:
    [[nodiscard]] static PngVerifyResultV1 verified(core::Sha256Digest digest) noexcept;
    [[nodiscard]] static PngVerifyResultV1 cancelled() noexcept;
    [[nodiscard]] static PngVerifyResultV1 failed(PngVerifyDiagnosticV1 diagnostic) noexcept;

    [[nodiscard]] PngVerifyStatusV1 status() const noexcept { return status_; }
    // Valid only when status() is Verified.
    [[nodiscard]] const core::Sha256Digest& digest() const& noexcept { return digest_; }
    [[nodiscard]] const core::Sha256Digest& digest() const&& = delete;
    [[nodiscard]] const PngVerifyDiagnosticV1& diagnostic() const& noexcept { return diagnostic_; }
    [[nodiscard]] const PngVerifyDiagnosticV1& diagnostic() const&& = delete;

  private:
    PngVerifyResultV1(PngVerifyStatusV1 status, core::Sha256Digest digest,
                      PngVerifyDiagnosticV1 diagnostic) noexcept;

    PngVerifyStatusV1 status_ = PngVerifyStatusV1::Failed;
    core::Sha256Digest digest_{};
    PngVerifyDiagnosticV1 diagnostic_{};
};

// Reopens `stagedPath` (a path/handle a future publication slice will own), proves the closed
// version 1 chunk/IHDR/sRGB/IDAT contract independently of any writer state, then compares the
// decoded RGBA8 samples bit-exact against `prepared` (this preset's own encoder input surface --
// design decision 3). Only once every check passes does it feed the DECODED reopened values (not
// `prepared`) into the existing kind-1 output-semantic-identity serializer, binding them through
// the same module-private seam the pre-approval analysis pipeline already produces
// processIdentity/report/displayProcessorIdentity for
// (`bloom::output::analyzePngRgba8SrgbV1`/`bloom::color::DisplayProcessorIdentityV1`), and
// surfacing the resulting digest as this result's `digest()`. A Failed or Cancelled result never
// surfaces a digest. `expectedOcioRevision` and `displayProcessorIdentity` are the caller-supplied
// products the PNG kind-1 payload's identity-issuance seam requires (design decision 4) -- the
// same two inputs `bindPngRgba8SrgbOutputAnalysisV1` itself takes, alongside `processIdentity` and
// `report`. Bounded memory (row-chunked reads/inflate), cancellation-checked between chunks. No
// zlib type appears in this public header.
class PngRgba8SrgbReopenVerifierV1 final {
  public:
    [[nodiscard]] PngVerifyResultV1
    verify(const std::filesystem::path& stagedPath, const PngRgba8SrgbPreparedStreamV1& prepared,
           const std::shared_ptr<const ProcessFrameSemanticIdentityV1>& processIdentity,
           const std::shared_ptr<const OutputAnalysisReportV1>& report,
           const core::Sha256Digest& expectedOcioRevision,
           const std::shared_ptr<const color::DisplayProcessorIdentityV1>& displayProcessorIdentity,
           const runtime::CancellationToken& cancellation,
           const PngVerifyProgressCallbackV1& progress = {}) const noexcept;
};

} // namespace bloom::output
