#pragma once

// Private (non-FILE_SET, non-installed) header shared by the two export-write bridges
// (flat_exr_export_write.cpp and png_export_write.cpp), mirroring flat_exr_preset_contract.hpp /
// png_preset_contract.hpp's role of keeping a shared rule in one place instead of letting the two
// preset paths drift apart. It holds exactly the two preset-independent helpers both bridges need:
// the best-effort progress-callback trampoline, and the bounded streaming artifact SHA-256
// (docs/architecture/frame-output.md, "Atomic Publication" step 5: "compute the artifact SHA-256").
// Nothing here is preset-specific, and the flat OpenEXR behavior is byte-for-byte the code that
// previously lived in flat_exr_export_write.cpp's own anonymous namespace.

#include <bloom/core/sha256.hpp>
#include <bloom/output/output_export_stage.hpp>
#include <bloom/output/output_limits.hpp>
#include <bloom/runtime/cancellation.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <vector>

namespace bloom::output::detail {

inline void reportExportProgress(const OutputExportProgressCallbackV1& callback,
                                 const OutputExportProgressV1& progress) noexcept {
    if (!callback) {
        return;
    }
    try {
        callback(progress);
    } catch (...) {
        return;
    }
}

// Streams `path`'s complete raw bytes in bounded chunks, computing their SHA-256. Returns
// nullopt on I/O failure, allocation failure, or cancellation.
[[nodiscard]] inline std::optional<core::Sha256Digest> hashExportArtifactFile(
    const std::filesystem::path& path, const runtime::CancellationToken& cancellation,
    const OutputExportProgressCallbackV1& progress, std::uint64_t& byteCountOut) noexcept {
    try {
        std::ifstream stream(path, std::ios::binary);
        if (!stream) {
            return std::nullopt;
        }
        std::vector<std::byte> buffer(kOutputAdapterMaximumStreamingChunkBytesV1);
        core::Sha256Hasher hasher;
        std::uint64_t total = 0;
        reportExportProgress(
            progress, {.stage = OutputExportStageV1::Publishing, .completed = 0, .total = 0});
        while (stream) {
            if (cancellation.isCancellationRequested()) {
                return std::nullopt;
            }
            stream.read(reinterpret_cast<char*>(buffer.data()),
                        static_cast<std::streamsize>(buffer.size()));
            const auto readCount = static_cast<std::size_t>(stream.gcount());
            if (readCount == 0) {
                break;
            }
            if (!hasher.update(std::span(buffer).first(readCount))) {
                return std::nullopt;
            }
            total += readCount;
            reportExportProgress(
                progress,
                {.stage = OutputExportStageV1::Publishing, .completed = total, .total = total});
        }
        if (stream.bad()) {
            return std::nullopt;
        }
        byteCountOut = total;
        return hasher.finalize();
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace bloom::output::detail
