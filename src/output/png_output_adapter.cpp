#include "png_preset_contract.hpp"

#include <bloom/output/output_limits.hpp>
#include <bloom/output/png_output_adapter.hpp>

// zlib is Bloom's Bloom-owned codec's only dependency (design decision 1): deflate for IDAT,
// crc32_z for every chunk's CRC-32. No OIIO/libpng anywhere. src/project/zip_container_writer.cpp
// already establishes this exact deflateInit2/deflate/crc32_z consumption idiom for the qualified
// prefix's ZLIB target; this writer reuses it with PNG's own (positive, zlib-wrapped) windowBits.
#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <span>
#include <system_error>
#include <vector>

namespace {

namespace output = bloom::output;
namespace render = bloom::render;
namespace runtime = bloom::runtime;

static_assert(output::detail::kPngDeflateStrategyV1 == Z_DEFAULT_STRATEGY,
              "png_preset_contract.hpp's spelled-out strategy constant must track zlib.h");

[[nodiscard]] bool removeDestinationBestEffort(const std::filesystem::path& destination) noexcept {
    std::error_code removeError;
    std::filesystem::remove(destination, removeError);
    std::error_code existsError;
    const bool stillExists = std::filesystem::exists(destination, existsError);
    return !existsError && !stillExists;
}

[[nodiscard]] output::PngWriteResultV1 fail(const output::PngWriteErrorCodeV1 error,
                                            const std::filesystem::path& destination,
                                            const bool destinationWasCreated) noexcept {
    const bool removed = !destinationWasCreated || removeDestinationBestEffort(destination);
    return output::PngWriteResultV1::failed(error, removed);
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

struct ValidatedGeometry final {
    std::uint32_t width;
    std::uint32_t height;
    std::uint64_t rowRgbaBytes;     // width * 4
    std::uint64_t rowOutputBytes;   // 1 (filter byte) + rowRgbaBytes
    std::uint64_t totalOutputBytes; // height * rowOutputBytes -- the deflate source length
};

// Checked preflight, before any allocation or file I/O: `output_limits.hpp`'s closed version 1
// dimension/pixel-count ceilings, `prepared`'s own internal consistency (pixels.size() must equal
// width * height), and the arithmetic this writer needs to size its deflate source length.
[[nodiscard]] std::optional<ValidatedGeometry>
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
    if (!pixelCount || *pixelCount > output::kOutputAnalysisMaximumPixelCountV1) {
        return std::nullopt;
    }
    if (prepared.pixels.size() != *pixelCount) {
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
    return ValidatedGeometry{width, height, *rowRgbaBytes, *rowOutputBytes, *totalOutputBytes};
}

void reportProgress(const output::PngWriteProgressCallbackV1& callback,
                    const output::PngWriteProgressV1& progress) noexcept {
    if (!callback) {
        return;
    }
    try {
        callback(progress);
    } catch (...) {
        // Monitoring cannot change a portable write outcome.
        return;
    }
}

void putBigEndianU32(std::vector<std::byte>& out, const std::uint32_t value) {
    out.push_back(static_cast<std::byte>((value >> 24U) & 0xFFU));
    out.push_back(static_cast<std::byte>((value >> 16U) & 0xFFU));
    out.push_back(static_cast<std::byte>((value >> 8U) & 0xFFU));
    out.push_back(static_cast<std::byte>(value & 0xFFU));
}

// Writes one complete chunk (length + type + data + CRC-32) to `file`. The CRC covers the type and
// data bytes only (never the length field), via zlib's crc32_z -- the same incremental-CRC idiom
// src/project/zip_container_writer.cpp already uses for its own entries.
[[nodiscard]] bool writeChunk(std::ofstream& file, const std::array<char, 4>& type,
                              const std::span<const std::byte> data) {
    std::vector<std::byte> header;
    header.reserve(8);
    if (data.size() > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    putBigEndianU32(header, static_cast<std::uint32_t>(data.size()));
    file.write(reinterpret_cast<const char*>(header.data()),
               static_cast<std::streamsize>(header.size()));
    file.write(type.data(), static_cast<std::streamsize>(type.size()));
    if (!data.empty()) {
        file.write(reinterpret_cast<const char*>(data.data()),
                   static_cast<std::streamsize>(data.size()));
    }
    auto crc = crc32_z(0L, reinterpret_cast<const Bytef*>(type.data()), type.size());
    if (!data.empty()) {
        crc = crc32_z(crc, reinterpret_cast<const Bytef*>(data.data()), data.size());
    }
    std::vector<std::byte> crcBytes;
    crcBytes.reserve(4);
    putBigEndianU32(crcBytes, static_cast<std::uint32_t>(crc));
    file.write(reinterpret_cast<const char*>(crcBytes.data()),
               static_cast<std::streamsize>(crcBytes.size()));
    return static_cast<bool>(file);
}

struct DeflateStreamGuard final {
    z_stream* handle;
    ~DeflateStreamGuard() { deflateEnd(handle); }
};

} // namespace

namespace bloom::output {

PngWriteResultV1::PngWriteResultV1(const PngWriteStatusV1 status, const PngWriteErrorCodeV1 error,
                                   const bool destinationRemoved) noexcept
    : status_(status), error_(error), destinationRemoved_(destinationRemoved) {}

PngWriteResultV1 PngWriteResultV1::written() noexcept {
    return {PngWriteStatusV1::Written, PngWriteErrorCodeV1::None, true};
}

PngWriteResultV1 PngWriteResultV1::cancelled(const bool destinationRemoved) noexcept {
    return {PngWriteStatusV1::Cancelled, PngWriteErrorCodeV1::None, destinationRemoved};
}

PngWriteResultV1 PngWriteResultV1::failed(const PngWriteErrorCodeV1 error,
                                          const bool destinationRemoved) noexcept {
    return {PngWriteStatusV1::Failed,
            error == PngWriteErrorCodeV1::None ? PngWriteErrorCodeV1::InternalInvariant : error,
            destinationRemoved};
}

PngWriteResultV1
PngRgba8SrgbWriterV1::write(const PngRgba8SrgbPreparedStreamV1& prepared,
                            const std::filesystem::path& destination,
                            const runtime::CancellationToken& cancellation,
                            const PngWriteProgressCallbackV1& progress) const noexcept {
    const auto geometry = ::validateGeometry(prepared);
    if (!geometry) {
        const auto width = prepared.dimensions.width();
        const auto height = prepared.dimensions.height();
        const auto error = (width == 0 || height == 0 ||
                            prepared.pixels.size() != static_cast<std::uint64_t>(width) * height)
                               ? PngWriteErrorCodeV1::InvalidPreparedStream
                               : PngWriteErrorCodeV1::ResourceLimitExceeded;
        return ::fail(error, destination, false);
    }
    if (cancellation.isCancellationRequested()) {
        return PngWriteResultV1::cancelled(true);
    }

    bool destinationCreated = false;
    try {
        std::ofstream file(destination, std::ios::binary | std::ios::trunc);
        if (!file) {
            return ::fail(PngWriteErrorCodeV1::DestinationUnavailable, destination, false);
        }
        destinationCreated = true;

        file.write(reinterpret_cast<const char*>(detail::kPngSignatureV1.data()),
                   static_cast<std::streamsize>(detail::kPngSignatureV1.size()));

        std::vector<std::byte> ihdrData;
        ihdrData.reserve(detail::kPngIhdrDataBytesV1);
        ::putBigEndianU32(ihdrData, geometry->width);
        ::putBigEndianU32(ihdrData, geometry->height);
        ihdrData.push_back(static_cast<std::byte>(detail::kPngBitDepthV1));
        ihdrData.push_back(static_cast<std::byte>(detail::kPngColorTypeRgbaV1));
        ihdrData.push_back(static_cast<std::byte>(detail::kPngCompressionMethodV1));
        ihdrData.push_back(static_cast<std::byte>(detail::kPngFilterMethodV1));
        ihdrData.push_back(static_cast<std::byte>(detail::kPngInterlaceMethodV1));
        if (!::writeChunk(file, detail::kPngChunkTypeIhdrV1, ihdrData)) {
            return ::fail(PngWriteErrorCodeV1::IoFailure, destination, destinationCreated);
        }

        const std::array<std::byte, 1> srgbData{
            static_cast<std::byte>(detail::kPngSrgbRenderingIntentV1)};
        if (!::writeChunk(file, detail::kPngChunkTypeSrgbV1, srgbData)) {
            return ::fail(PngWriteErrorCodeV1::IoFailure, destination, destinationCreated);
        }

        if (cancellation.isCancellationRequested()) {
            return PngWriteResultV1::cancelled(::removeDestinationBestEffort(destination));
        }

        z_stream stream{};
        if (deflateInit2(&stream, detail::kPngDeflateLevelV1, Z_DEFLATED,
                         detail::kPngDeflateWindowBitsV1, detail::kPngDeflateMemLevelV1,
                         detail::kPngDeflateStrategyV1) != Z_OK) {
            return ::fail(PngWriteErrorCodeV1::CompressorFailure, destination, destinationCreated);
        }
        ::DeflateStreamGuard guard{&stream};

        std::vector<std::byte> compressed;
        compressed.reserve(deflateBound(&stream, static_cast<uLong>(geometry->totalOutputBytes)));

        const auto rowsPerChunk =
            std::max<std::uint64_t>(1, kOutputAdapterMaximumStreamingChunkBytesV1 /
                                           std::max<std::uint64_t>(geometry->rowOutputBytes, 1));
        std::vector<std::byte> rowChunkBuffer;
        constexpr std::size_t kDrainBufferBytes = 1U << 16U;
        std::array<std::byte, kDrainBufferBytes> drainBuffer{};

        std::uint64_t rowsRemaining = geometry->height;
        std::uint64_t completedRows = 0;
        ::reportProgress(progress, {0, geometry->height});
        bool compressorFailed = false;
        while (rowsRemaining > 0) {
            if (cancellation.isCancellationRequested()) {
                return PngWriteResultV1::cancelled(::removeDestinationBestEffort(destination));
            }
            const auto chunkRows = std::min(rowsPerChunk, rowsRemaining);
            const bool isLastChunk = chunkRows == rowsRemaining;
            rowChunkBuffer.resize(chunkRows * geometry->rowOutputBytes);
            for (std::uint64_t row = 0; row < chunkRows; ++row) {
                const auto rowIndex = completedRows + row;
                auto* const rowOut = rowChunkBuffer.data() +
                                     static_cast<std::ptrdiff_t>(row * geometry->rowOutputBytes);
                rowOut[0] = static_cast<std::byte>(detail::kPngFilterTypeNoneV1);
                const auto* const sourceRow =
                    prepared.pixels.data() +
                    static_cast<std::ptrdiff_t>(rowIndex * geometry->width);
                std::memcpy(rowOut + 1, sourceRow,
                            static_cast<std::size_t>(geometry->rowRgbaBytes));
            }

            stream.next_in = reinterpret_cast<Bytef*>(rowChunkBuffer.data());
            stream.avail_in = static_cast<uInt>(rowChunkBuffer.size());
            const int flush = isLastChunk ? Z_FINISH : Z_NO_FLUSH;
            int deflateStatus = Z_OK;
            do {
                stream.next_out = reinterpret_cast<Bytef*>(drainBuffer.data());
                stream.avail_out = static_cast<uInt>(drainBuffer.size());
                deflateStatus = deflate(&stream, flush);
                if (deflateStatus == Z_STREAM_ERROR) {
                    compressorFailed = true;
                    break;
                }
                const auto produced = drainBuffer.size() - stream.avail_out;
                compressed.insert(compressed.end(), drainBuffer.begin(),
                                  drainBuffer.begin() + static_cast<std::ptrdiff_t>(produced));
            } while (!compressorFailed &&
                     (stream.avail_out == 0 || (isLastChunk && deflateStatus != Z_STREAM_END)));
            if (compressorFailed) {
                break;
            }

            rowsRemaining -= chunkRows;
            completedRows += chunkRows;
            ::reportProgress(progress, {completedRows, geometry->height});
        }
        if (compressorFailed) {
            return ::fail(PngWriteErrorCodeV1::CompressorFailure, destination, destinationCreated);
        }

        // Deterministic fixed-size IDAT splits (png_preset_contract.hpp): one chunk unless the
        // compressed stream exceeds kOutputAdapterMaximumStreamingChunkBytesV1, in which case
        // every chunk but the last is exactly that size.
        std::size_t offset = 0;
        do {
            if (cancellation.isCancellationRequested()) {
                return PngWriteResultV1::cancelled(::removeDestinationBestEffort(destination));
            }
            const auto remaining = compressed.size() - offset;
            const auto thisChunkBytes =
                std::min<std::size_t>(remaining, kOutputAdapterMaximumStreamingChunkBytesV1);
            const std::span<const std::byte> chunkData(compressed.data() + offset, thisChunkBytes);
            if (!::writeChunk(file, detail::kPngChunkTypeIdatV1, chunkData)) {
                return ::fail(PngWriteErrorCodeV1::IoFailure, destination, destinationCreated);
            }
            offset += thisChunkBytes;
        } while (offset < compressed.size());

        if (!::writeChunk(file, detail::kPngChunkTypeIendV1, {})) {
            return ::fail(PngWriteErrorCodeV1::IoFailure, destination, destinationCreated);
        }

        file.flush();
        if (!file) {
            return ::fail(PngWriteErrorCodeV1::IoFailure, destination, destinationCreated);
        }
    } catch (const std::bad_alloc&) {
        return ::fail(PngWriteErrorCodeV1::AllocationFailure, destination, destinationCreated);
    } catch (const std::exception&) {
        return ::fail(destinationCreated ? PngWriteErrorCodeV1::IoFailure
                                         : PngWriteErrorCodeV1::DestinationUnavailable,
                      destination, destinationCreated);
    } catch (...) {
        return ::fail(PngWriteErrorCodeV1::InternalInvariant, destination, destinationCreated);
    }

    return PngWriteResultV1::written();
}

} // namespace bloom::output
