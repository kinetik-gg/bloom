// output_semantic_identity.hpp is module-private (no public bloom/output/ path exists for it --
// docs/architecture/frame-output.md calls it "the module-private Output Semantic Identity version
// 1 streaming serializer/preparer"); this translation unit lives inside src/output/ itself, so the
// quoted include resolves relative to this file, matching every other src/output/*.cpp that
// touches it. This is also where the friend seam class below (forward-declared in that header as
// `detail::PngRgba8SrgbSemanticPayloadVerifierV1`) is defined -- the PNG counterpart of
// flat_exr_reopen_verifier.cpp's identically-shaped `FlatExrRgba32fLinRec709SceneSemanticPayload
// VerifierV1`.
#include "output_semantic_identity.hpp"

#include "png_preset_contract.hpp"

#include <bloom/output/output_limits.hpp>
#include <bloom/output/png_reopen_verifier.hpp>

// zlib is this Bloom-owned codec's only dependency (design decision 1): inflate for the IDAT
// stream, crc32_z for every chunk's CRC-32 -- the read-back counterpart of
// png_output_adapter.cpp's deflate/crc32_z usage. No OIIO/libpng anywhere.
#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fstream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

namespace color = bloom::color;
namespace core = bloom::core;
namespace output = bloom::output;
namespace render = bloom::render;
namespace runtime = bloom::runtime;

// Positional aggregate construction (never designated) so every PngVerifyDiagnosticV1 site stays a
// one-line call regardless of how many trailing fields default -- mirrors
// flat_exr_reopen_verifier.cpp's `diag()` helper.
[[nodiscard]] output::PngVerifyDiagnosticV1
diag(const output::PngVerifyErrorCodeV1 code, std::string chunkType = {},
     const std::optional<std::uint64_t> chunkIndex = std::nullopt,
     const std::optional<std::uint64_t> byteOffset = std::nullopt,
     const std::optional<std::uint64_t> row = std::nullopt) noexcept {
    return output::PngVerifyDiagnosticV1{code, std::move(chunkType), chunkIndex, byteOffset, row};
}

void reportProgress(const output::PngVerifyProgressCallbackV1& callback,
                    const output::PngVerifyProgressV1& progress) noexcept {
    if (!callback) {
        return;
    }
    try {
        callback(progress);
    } catch (...) {
        // Monitoring cannot change a portable verification outcome.
        return;
    }
}

[[nodiscard]] constexpr std::optional<std::uint64_t>
multiplyChecked(const std::uint64_t left, const std::uint64_t right) noexcept {
    if (right != 0 && left > std::numeric_limits<std::uint64_t>::max() / right) {
        return std::nullopt;
    }
    return left * right;
}

[[nodiscard]] constexpr std::optional<std::uint64_t>
addChecked(const std::uint64_t left, const std::uint64_t right) noexcept {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        return std::nullopt;
    }
    return left + right;
}

struct GeometryExpectation final {
    std::uint32_t width;
    std::uint32_t height;
    std::uint64_t rowRgbaBytes;     // width * 4
    std::uint64_t rowOutputBytes;   // 1 (filter byte) + rowRgbaBytes
    std::uint64_t totalOutputBytes; // height * rowOutputBytes -- the checked inflate ceiling
};

// Same checked-arithmetic preflight as png_output_adapter.cpp's validateGeometry(): the caller-
// supplied `prepared` stream's own internal consistency plus output_limits.hpp's closed version 1
// dimension/pixel-count/byte ceilings (reused, not duplicated with new PNG-specific numbers --
// design decision 5).
[[nodiscard]] std::optional<GeometryExpectation>
validateGeometry(const output::PngRgba8SrgbPreparedStreamV1& prepared) noexcept {
    const auto width = prepared.dimensions.width();
    const auto height = prepared.dimensions.height();
    if (width == 0 || height == 0) {
        return std::nullopt;
    }
    if (width > output::kOutputAnalysisMaximumDimensionV1 ||
        height > output::kOutputAnalysisMaximumDimensionV1) {
        return std::nullopt;
    }
    const auto pixelCount = multiplyChecked(width, height);
    if (!pixelCount || *pixelCount > output::kOutputAnalysisMaximumPixelCountV1 ||
        prepared.pixels.size() != *pixelCount) {
        return std::nullopt;
    }
    const auto rowRgbaBytes = multiplyChecked(width, 4U);
    const auto rowOutputBytes = rowRgbaBytes ? addChecked(*rowRgbaBytes, 1U) : std::nullopt;
    const auto totalOutputBytes =
        rowOutputBytes ? multiplyChecked(*rowOutputBytes, height) : std::nullopt;
    if (!rowRgbaBytes || !rowOutputBytes || !totalOutputBytes ||
        *totalOutputBytes > output::kOutputAnalysisMaximumProcessPixelBytesV1) {
        return std::nullopt;
    }
    return GeometryExpectation{width, height, *rowRgbaBytes, *rowOutputBytes, *totalOutputBytes};
}

[[nodiscard]] std::uint32_t readBigEndianU32(const std::span<const std::byte> bytes) noexcept {
    return (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[0])) << 24U) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[1])) << 16U) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[2])) << 8U) |
           static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[3]));
}

[[nodiscard]] std::uint32_t crc32Of(const std::array<char, 4>& type,
                                    const std::span<const std::byte> data) noexcept {
    auto crc = crc32_z(0L, reinterpret_cast<const Bytef*>(type.data()), type.size());
    if (!data.empty()) {
        crc = crc32_z(crc, reinterpret_cast<const Bytef*>(data.data()), data.size());
    }
    return static_cast<std::uint32_t>(crc);
}

// A minimal buffered raw-file reader that tracks its own cursor for diagnostics. `Eof` means zero
// bytes were available (a clean end of file exactly at a chunk boundary); `Short` means some but
// not all of the requested bytes were available (a cut-off mid-chunk).
enum class ReadStatus : std::uint8_t { Complete, Eof, Short };

class RawFileReader final {
  public:
    explicit RawFileReader(const std::filesystem::path& path) : stream_(path, std::ios::binary) {}

    [[nodiscard]] bool isOpen() const noexcept { return static_cast<bool>(stream_); }
    [[nodiscard]] std::uint64_t position() const noexcept { return position_; }

    [[nodiscard]] ReadStatus readExact(const std::span<std::byte> destination) noexcept {
        stream_.read(reinterpret_cast<char*>(destination.data()),
                     static_cast<std::streamsize>(destination.size()));
        const auto got = stream_.gcount();
        stream_.clear(stream_.rdstate() & ~std::ios::failbit);
        position_ += static_cast<std::uint64_t>(got);
        if (got == static_cast<std::streamsize>(destination.size())) {
            return ReadStatus::Complete;
        }
        return got == 0 ? ReadStatus::Eof : ReadStatus::Short;
    }

    // True when no byte remains (used only after the closed sequence's IEND is consumed).
    [[nodiscard]] bool atEof() noexcept { return stream_.peek() == std::char_traits<char>::eof(); }

  private:
    std::ifstream stream_;
    std::uint64_t position_ = 0;
};

struct InflateStreamGuard final {
    z_stream* handle;
    bool initialized;
    ~InflateStreamGuard() {
        if (initialized) {
            inflateEnd(handle);
        }
    }
};

} // namespace

namespace bloom::output::detail {

// The friend seam `output_semantic_identity.hpp` forward-declares
// (`friend class detail::PngRgba8SrgbSemanticPayloadVerifierV1;`) for exactly one purpose: only
// this reopen verifier -- never the writer, never a test -- may construct a
// `PngRgba8SrgbVerifiedSemanticProductV1` from decoded reopened values.
class PngRgba8SrgbSemanticPayloadVerifierV1 final {
  public:
    [[nodiscard]] static PngRgba8SrgbVerifiedSemanticProductV1 buildVerifiedProduct(
        const std::shared_ptr<const PngRgba8SrgbBoundOutputAnalysisV1>& boundAnalysis,
        const render::ImageExtent dimensions, std::vector<std::uint8_t>&& rgbaBytes) {
        return PngRgba8SrgbVerifiedSemanticProductV1(boundAnalysis, dimensions,
                                                     std::move(rgbaBytes));
    }
};

} // namespace bloom::output::detail

namespace bloom::output {

PngVerifyResultV1::PngVerifyResultV1(const PngVerifyStatusV1 status,
                                     const core::Sha256Digest digest,
                                     PngVerifyDiagnosticV1 diagnostic) noexcept
    : status_(status), digest_(digest), diagnostic_(std::move(diagnostic)) {}

PngVerifyResultV1 PngVerifyResultV1::verified(const core::Sha256Digest digest) noexcept {
    return {PngVerifyStatusV1::Verified, digest, {}};
}

PngVerifyResultV1 PngVerifyResultV1::cancelled() noexcept {
    return {PngVerifyStatusV1::Cancelled, {}, {}};
}

PngVerifyResultV1 PngVerifyResultV1::failed(PngVerifyDiagnosticV1 diagnostic) noexcept {
    if (diagnostic.code == PngVerifyErrorCodeV1::None) {
        diagnostic.code = PngVerifyErrorCodeV1::InternalInvariant;
    }
    return {PngVerifyStatusV1::Failed, {}, std::move(diagnostic)};
}

PngVerifyResultV1 PngRgba8SrgbReopenVerifierV1::verify(
    const std::filesystem::path& stagedPath, const PngRgba8SrgbPreparedStreamV1& prepared,
    const std::shared_ptr<const ProcessFrameSemanticIdentityV1>& processIdentity,
    const std::shared_ptr<const OutputAnalysisReportV1>& report,
    const core::Sha256Digest& expectedOcioRevision,
    const std::shared_ptr<const color::DisplayProcessorIdentityV1>& displayProcessorIdentity,
    const runtime::CancellationToken& cancellation,
    const PngVerifyProgressCallbackV1& progress) const noexcept {
    if (processIdentity == nullptr || report == nullptr || displayProcessorIdentity == nullptr) {
        return PngVerifyResultV1::failed(::diag(PngVerifyErrorCodeV1::IdentityIssuanceFailed));
    }
    const auto geometry = ::validateGeometry(prepared);
    if (!geometry) {
        return PngVerifyResultV1::failed(::diag(PngVerifyErrorCodeV1::InternalInvariant));
    }
    if (cancellation.isCancellationRequested()) {
        return PngVerifyResultV1::cancelled();
    }

    try {
        ::RawFileReader reader(stagedPath);
        if (!reader.isOpen()) {
            return PngVerifyResultV1::failed(::diag(PngVerifyErrorCodeV1::SourceUnavailable));
        }

        std::array<std::byte, 8> signature{};
        if (reader.readExact(signature) != ::ReadStatus::Complete) {
            return PngVerifyResultV1::failed(::diag(PngVerifyErrorCodeV1::TruncatedFile));
        }
        for (std::size_t index = 0; index < signature.size(); ++index) {
            if (signature[index] != static_cast<std::byte>(detail::kPngSignatureV1[index])) {
                return PngVerifyResultV1::failed(::diag(PngVerifyErrorCodeV1::InvalidSignature));
            }
        }

        enum class State : std::uint8_t { NeedIhdr, NeedSrgb, NeedIdat, Done };
        auto state = State::NeedIhdr;
        bool sawIdat = false;
        std::uint64_t chunkIndex = 0;

        z_stream inflateStream{};
        bool inflateInitialized = false;
        ::InflateStreamGuard inflateGuard{&inflateStream, false};

        std::vector<std::byte> decoded(static_cast<std::size_t>(geometry->totalOutputBytes));
        std::size_t decodedFilled = 0;
        bool sawStreamEnd = false;

        while (state != State::Done) {
            if (cancellation.isCancellationRequested()) {
                return PngVerifyResultV1::cancelled();
            }
            const auto chunkOffset = reader.position();
            std::array<std::byte, 8> chunkHeader{};
            const auto headerStatus = reader.readExact(chunkHeader);
            if (headerStatus == ::ReadStatus::Eof) {
                return PngVerifyResultV1::failed(
                    ::diag(PngVerifyErrorCodeV1::MissingChunk, {}, chunkIndex, chunkOffset));
            }
            if (headerStatus == ::ReadStatus::Short) {
                return PngVerifyResultV1::failed(
                    ::diag(PngVerifyErrorCodeV1::TruncatedFile, {}, chunkIndex, chunkOffset));
            }
            const auto length =
                ::readBigEndianU32(std::span<const std::byte>(chunkHeader).first(4));
            const std::array<char, 4> type{
                static_cast<char>(std::to_integer<unsigned char>(chunkHeader[4])),
                static_cast<char>(std::to_integer<unsigned char>(chunkHeader[5])),
                static_cast<char>(std::to_integer<unsigned char>(chunkHeader[6])),
                static_cast<char>(std::to_integer<unsigned char>(chunkHeader[7]))};
            const std::string typeName(type.begin(), type.end());

            if (state == State::NeedIhdr) {
                if (type != detail::kPngChunkTypeIhdrV1) {
                    return PngVerifyResultV1::failed(
                        ::diag(PngVerifyErrorCodeV1::UnexpectedChunkType, typeName, chunkIndex,
                               chunkOffset));
                }
                if (length != detail::kPngIhdrDataBytesV1) {
                    return PngVerifyResultV1::failed(
                        ::diag(PngVerifyErrorCodeV1::FixedChunkLengthMismatch, typeName, chunkIndex,
                               chunkOffset));
                }
                std::array<std::byte, 13> data{};
                if (reader.readExact(data) != ::ReadStatus::Complete) {
                    return PngVerifyResultV1::failed(::diag(PngVerifyErrorCodeV1::TruncatedFile,
                                                            typeName, chunkIndex, chunkOffset));
                }
                std::array<std::byte, 4> crcBytes{};
                if (reader.readExact(crcBytes) != ::ReadStatus::Complete) {
                    return PngVerifyResultV1::failed(::diag(PngVerifyErrorCodeV1::TruncatedFile,
                                                            typeName, chunkIndex, chunkOffset));
                }
                if (::crc32Of(type, data) != ::readBigEndianU32(crcBytes)) {
                    return PngVerifyResultV1::failed(::diag(PngVerifyErrorCodeV1::ChunkCrcMismatch,
                                                            typeName, chunkIndex, chunkOffset));
                }
                const auto width = ::readBigEndianU32(std::span<const std::byte>(data).first(4));
                const auto height =
                    ::readBigEndianU32(std::span<const std::byte>(data).subspan(4, 4));
                const auto bitDepth = std::to_integer<std::uint8_t>(data[8]);
                const auto colorType = std::to_integer<std::uint8_t>(data[9]);
                const auto compressionMethod = std::to_integer<std::uint8_t>(data[10]);
                const auto filterMethod = std::to_integer<std::uint8_t>(data[11]);
                const auto interlaceMethod = std::to_integer<std::uint8_t>(data[12]);
                if (width != geometry->width || height != geometry->height ||
                    bitDepth != detail::kPngBitDepthV1 ||
                    colorType != detail::kPngColorTypeRgbaV1 ||
                    compressionMethod != detail::kPngCompressionMethodV1 ||
                    filterMethod != detail::kPngFilterMethodV1 ||
                    interlaceMethod != detail::kPngInterlaceMethodV1) {
                    return PngVerifyResultV1::failed(::diag(PngVerifyErrorCodeV1::IhdrFieldMismatch,
                                                            typeName, chunkIndex, chunkOffset));
                }
                state = State::NeedSrgb;
            } else if (state == State::NeedSrgb) {
                if (type != detail::kPngChunkTypeSrgbV1) {
                    return PngVerifyResultV1::failed(
                        ::diag(PngVerifyErrorCodeV1::UnexpectedChunkType, typeName, chunkIndex,
                               chunkOffset));
                }
                if (length != detail::kPngSrgbDataBytesV1) {
                    return PngVerifyResultV1::failed(
                        ::diag(PngVerifyErrorCodeV1::FixedChunkLengthMismatch, typeName, chunkIndex,
                               chunkOffset));
                }
                std::array<std::byte, 1> data{};
                if (reader.readExact(data) != ::ReadStatus::Complete) {
                    return PngVerifyResultV1::failed(::diag(PngVerifyErrorCodeV1::TruncatedFile,
                                                            typeName, chunkIndex, chunkOffset));
                }
                std::array<std::byte, 4> crcBytes{};
                if (reader.readExact(crcBytes) != ::ReadStatus::Complete) {
                    return PngVerifyResultV1::failed(::diag(PngVerifyErrorCodeV1::TruncatedFile,
                                                            typeName, chunkIndex, chunkOffset));
                }
                if (::crc32Of(type, data) != ::readBigEndianU32(crcBytes)) {
                    return PngVerifyResultV1::failed(::diag(PngVerifyErrorCodeV1::ChunkCrcMismatch,
                                                            typeName, chunkIndex, chunkOffset));
                }
                if (std::to_integer<std::uint8_t>(data[0]) != detail::kPngSrgbRenderingIntentV1) {
                    return PngVerifyResultV1::failed(
                        ::diag(PngVerifyErrorCodeV1::SrgbIntentMismatch, typeName, chunkIndex,
                               chunkOffset));
                }
                state = State::NeedIdat;
            } else { // State::NeedIdat
                if (type == detail::kPngChunkTypeIdatV1) {
                    if (length > kOutputAdapterMaximumStreamingChunkBytesV1) {
                        return PngVerifyResultV1::failed(
                            ::diag(PngVerifyErrorCodeV1::ChunkLengthExceedsLimit, typeName,
                                   chunkIndex, chunkOffset));
                    }
                    std::vector<std::byte> data(length);
                    if (reader.readExact(data) != ::ReadStatus::Complete) {
                        return PngVerifyResultV1::failed(::diag(PngVerifyErrorCodeV1::TruncatedFile,
                                                                typeName, chunkIndex, chunkOffset));
                    }
                    std::array<std::byte, 4> crcBytes{};
                    if (reader.readExact(crcBytes) != ::ReadStatus::Complete) {
                        return PngVerifyResultV1::failed(::diag(PngVerifyErrorCodeV1::TruncatedFile,
                                                                typeName, chunkIndex, chunkOffset));
                    }
                    if (::crc32Of(type, data) != ::readBigEndianU32(crcBytes)) {
                        return PngVerifyResultV1::failed(
                            ::diag(PngVerifyErrorCodeV1::ChunkCrcMismatch, typeName, chunkIndex,
                                   chunkOffset));
                    }
                    sawIdat = true;

                    if (!inflateInitialized) {
                        if (inflateInit(&inflateStream) != Z_OK) {
                            return PngVerifyResultV1::failed(
                                ::diag(PngVerifyErrorCodeV1::AllocationFailure, typeName,
                                       chunkIndex, chunkOffset));
                        }
                        inflateInitialized = true;
                        inflateGuard.initialized = true;
                    }
                    if (sawStreamEnd && !data.empty()) {
                        // Trailing IDAT bytes after the zlib stream already finished.
                        return PngVerifyResultV1::failed(
                            ::diag(PngVerifyErrorCodeV1::IdatZlibStreamInvalid, typeName,
                                   chunkIndex, chunkOffset));
                    }
                    inflateStream.next_in = reinterpret_cast<Bytef*>(data.data());
                    inflateStream.avail_in = static_cast<uInt>(data.size());
                    bool exceeded = false;
                    bool zlibError = false;
                    while (inflateStream.avail_in > 0 && !sawStreamEnd) {
                        const auto capacity = decoded.size() - decodedFilled;
                        if (capacity == 0) {
                            exceeded = true;
                            break;
                        }
                        inflateStream.next_out =
                            reinterpret_cast<Bytef*>(decoded.data() + decodedFilled);
                        inflateStream.avail_out = static_cast<uInt>(
                            std::min<std::size_t>(capacity, std::numeric_limits<uInt>::max()));
                        const auto beforeOut = inflateStream.avail_out;
                        const auto ret = inflate(&inflateStream, Z_NO_FLUSH);
                        const auto produced = beforeOut - inflateStream.avail_out;
                        decodedFilled += produced;
                        if (ret == Z_STREAM_END) {
                            sawStreamEnd = true;
                            break;
                        }
                        if (ret == Z_OK) {
                            continue;
                        }
                        if (ret == Z_BUF_ERROR && inflateStream.avail_out == 0) {
                            exceeded = true;
                            break;
                        }
                        zlibError = true;
                        break;
                    }
                    if (exceeded) {
                        return PngVerifyResultV1::failed(
                            ::diag(PngVerifyErrorCodeV1::IdatExpandedSizeExceeded, typeName,
                                   chunkIndex, chunkOffset));
                    }
                    if (zlibError) {
                        return PngVerifyResultV1::failed(
                            ::diag(PngVerifyErrorCodeV1::IdatZlibStreamInvalid, typeName,
                                   chunkIndex, chunkOffset));
                    }
                } else if (type == detail::kPngChunkTypeIendV1 && sawIdat) {
                    if (length != detail::kPngIendDataBytesV1) {
                        return PngVerifyResultV1::failed(
                            ::diag(PngVerifyErrorCodeV1::FixedChunkLengthMismatch, typeName,
                                   chunkIndex, chunkOffset));
                    }
                    std::array<std::byte, 4> crcBytes{};
                    if (reader.readExact(crcBytes) != ::ReadStatus::Complete) {
                        return PngVerifyResultV1::failed(::diag(PngVerifyErrorCodeV1::TruncatedFile,
                                                                typeName, chunkIndex, chunkOffset));
                    }
                    if (::crc32Of(type, std::span<const std::byte>{}) !=
                        ::readBigEndianU32(crcBytes)) {
                        return PngVerifyResultV1::failed(
                            ::diag(PngVerifyErrorCodeV1::ChunkCrcMismatch, typeName, chunkIndex,
                                   chunkOffset));
                    }
                    if (!sawStreamEnd || decodedFilled != decoded.size()) {
                        return PngVerifyResultV1::failed(
                            ::diag(PngVerifyErrorCodeV1::IdatZlibStreamInvalid, typeName,
                                   chunkIndex, chunkOffset));
                    }
                    state = State::Done;
                } else {
                    return PngVerifyResultV1::failed(
                        ::diag(PngVerifyErrorCodeV1::UnexpectedChunkType, typeName, chunkIndex,
                               chunkOffset));
                }
            }
            ++chunkIndex;
        }

        if (!reader.atEof()) {
            return PngVerifyResultV1::failed(::diag(PngVerifyErrorCodeV1::TrailingBytesAfterIend));
        }

        // Every scanline's leading filter byte must be exactly 0 (None); the remaining
        // width * 4 bytes must bit-exact match `prepared`'s own row. Processed in bounded row
        // chunks -- the same kOutputAdapterMaximumStreamingChunkBytesV1 cadence the writer uses --
        // with cancellation checked and progress reported between chunks.
        const auto rowsPerChunk =
            std::max<std::uint64_t>(1, kOutputAdapterMaximumStreamingChunkBytesV1 /
                                           std::max<std::uint64_t>(geometry->rowOutputBytes, 1));
        std::uint64_t completedRows = 0;
        ::reportProgress(progress, {0, geometry->height});
        while (completedRows < geometry->height) {
            if (cancellation.isCancellationRequested()) {
                return PngVerifyResultV1::cancelled();
            }
            const auto chunkRows =
                std::min<std::uint64_t>(rowsPerChunk, geometry->height - completedRows);
            for (std::uint64_t row = 0; row < chunkRows; ++row) {
                const auto rowIndex = completedRows + row;
                const auto rowOffset =
                    static_cast<std::size_t>(rowIndex * geometry->rowOutputBytes);
                if (decoded[rowOffset] != static_cast<std::byte>(detail::kPngFilterTypeNoneV1)) {
                    return PngVerifyResultV1::failed(
                        ::diag(PngVerifyErrorCodeV1::RowFilterByteNonzero, {}, std::nullopt,
                               rowOffset, rowIndex));
                }
                const auto* const decodedRow =
                    decoded.data() + static_cast<std::ptrdiff_t>(rowOffset) + 1;
                const auto* const expectedRow = reinterpret_cast<const std::byte*>(
                    prepared.pixels.data() +
                    static_cast<std::ptrdiff_t>(rowIndex * geometry->width));
                if (std::memcmp(decodedRow, expectedRow,
                                static_cast<std::size_t>(geometry->rowRgbaBytes)) != 0) {
                    return PngVerifyResultV1::failed(::diag(PngVerifyErrorCodeV1::SampleMismatch,
                                                            {}, std::nullopt, std::nullopt,
                                                            rowIndex));
                }
            }
            completedRows += chunkRows;
            ::reportProgress(progress, {completedRows, geometry->height});
        }

        // Feed the DECODED reopened values (design decision 4), not `prepared`, into the kind-1
        // identity seam.
        std::vector<std::uint8_t> rgbaBytes(static_cast<std::size_t>(geometry->rowRgbaBytes) *
                                            geometry->height);
        for (std::uint64_t row = 0; row < geometry->height; ++row) {
            const auto sourceOffset = static_cast<std::size_t>(row * geometry->rowOutputBytes) + 1;
            const auto destOffset = static_cast<std::size_t>(row * geometry->rowRgbaBytes);
            std::memcpy(rgbaBytes.data() + destOffset, decoded.data() + sourceOffset,
                        static_cast<std::size_t>(geometry->rowRgbaBytes));
        }

        auto bound = bindPngRgba8SrgbOutputAnalysisV1(processIdentity, report, expectedOcioRevision,
                                                      displayProcessorIdentity);
        if (bound.analysis() == nullptr) {
            return PngVerifyResultV1::failed(::diag(PngVerifyErrorCodeV1::IdentityIssuanceFailed));
        }

        auto verifiedProduct = detail::PngRgba8SrgbSemanticPayloadVerifierV1::buildVerifiedProduct(
            bound.analysis(), prepared.dimensions, std::move(rgbaBytes));

        const OutputSemanticIdentityV1Preparer identityPreparer;
        const auto issuance = identityPreparer.preparePngRgba8SrgbV1(
            PngRgba8SrgbOutputSemanticIdentityInputV1{std::move(verifiedProduct)}, cancellation);

        switch (issuance.status()) {
        case OutputSemanticIdentityPreparationStatusV1::Prepared:
            if (issuance.identity() == nullptr) {
                return PngVerifyResultV1::failed(::diag(PngVerifyErrorCodeV1::InternalInvariant));
            }
            return PngVerifyResultV1::verified(issuance.identity()->digest());
        case OutputSemanticIdentityPreparationStatusV1::Cancelled:
            return PngVerifyResultV1::cancelled();
        case OutputSemanticIdentityPreparationStatusV1::Failed:
        default:
            return PngVerifyResultV1::failed(::diag(PngVerifyErrorCodeV1::IdentityIssuanceFailed));
        }
    } catch (const std::bad_alloc&) {
        return PngVerifyResultV1::failed(::diag(PngVerifyErrorCodeV1::AllocationFailure));
    } catch (const std::length_error&) {
        return PngVerifyResultV1::failed(::diag(PngVerifyErrorCodeV1::AllocationFailure));
    } catch (const std::exception&) {
        return PngVerifyResultV1::failed(::diag(PngVerifyErrorCodeV1::InternalInvariant));
    } catch (...) {
        return PngVerifyResultV1::failed(::diag(PngVerifyErrorCodeV1::InternalInvariant));
    }
}

} // namespace bloom::output
