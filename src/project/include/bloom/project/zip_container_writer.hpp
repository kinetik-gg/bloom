#pragma once

#include <bloom/project/project_io_memory.hpp>
#include <bloom/project/project_io_memory_resource.hpp>
#include <bloom/project/zip_container.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <vector>

// The Constrained ZIP Profile container writer, per docs/architecture/project-format.md
// ("Constrained ZIP Profile" + "Resource Limits"). `writeZipContainer()` hand-assembles the fixed
// two-entry ("manifest.json" then "document.json") archive layout byte-by-byte and uses the
// qualified zlib (`deflateInit2` with negative windowBits for a raw stream, level 6) confined to
// zip_container_writer.cpp for compression and `crc32_z` for entry CRCs. It never calls libzip: the
// profile makes the writer's exact field bytes (flags 0x0800, zero extra fields, DOS epoch, mode
// 0644) contractual against the strict reader, and libzip's emission details are not controllable
// to that precision. Every written archive is accepted by `readZipContainer()` with default limits
// by construction (the refusal rules below make a rejected-by-the-reader output unreachable); the
// writer itself never runs the reader in production.

namespace bloom::project {

namespace detail {
class ZipContainerWriteBuilder;
} // namespace detail

enum class ZipContainerWriteError : std::uint8_t {
    None,
    InvalidLimits,
    // manifest or document payload exceeds its per-entry expanded-size limit; see entryInError().
    EntrySizeLimitExceeded,
    TotalExpandedLimitExceeded,
    ArchiveSizeLimitExceeded,
    SizeOverflow,
    ResourceExhausted,
    // The qualified zlib compressor reported a failure this writer cannot interpret. Fail closed:
    // never emit a suspect archive.
    QualifiedCompressorFailure,
};

// Names which entry an EntrySizeLimitExceeded failure names; None for every other error.
enum class ZipContainerWriteEntry : std::uint8_t {
    None,
    Manifest,
    Document,
};

// The successfully written archive's bytes, owning its PMR resource behind a stable heap address
// (mirrors ZipContainerDocument) so moving the result never invalidates the byte buffer's
// allocator state.
class ZipContainerArchive final {
  public:
    ZipContainerArchive(const ZipContainerArchive&) = delete;
    ZipContainerArchive& operator=(const ZipContainerArchive&) = delete;
    ZipContainerArchive(ZipContainerArchive&&) noexcept = default;
    ZipContainerArchive& operator=(ZipContainerArchive&&) noexcept = default;
    ~ZipContainerArchive() = default;

    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        return {bytes_.data(), bytes_.size()};
    }

  private:
    friend class detail::ZipContainerWriteBuilder;

    ZipContainerArchive(std::unique_ptr<ProjectIoMemoryResource> resource,
                        std::pmr::vector<std::byte> bytes) noexcept;

    std::unique_ptr<ProjectIoMemoryResource> resource_;
    std::pmr::vector<std::byte> bytes_;
};

// [[nodiscard]] failure-aware result. On success, archive() names the assembled bytes; on failure,
// error() names the typed cause and entryInError() additionally names the offending entry for
// EntrySizeLimitExceeded (None otherwise).
class [[nodiscard]] ZipContainerWriteResult final {
  public:
    ZipContainerWriteResult(const ZipContainerWriteResult&) = delete;
    ZipContainerWriteResult& operator=(const ZipContainerWriteResult&) = delete;
    ZipContainerWriteResult(ZipContainerWriteResult&&) noexcept = default;
    ZipContainerWriteResult& operator=(ZipContainerWriteResult&&) noexcept = default;
    ~ZipContainerWriteResult() = default;

    [[nodiscard]] explicit operator bool() const noexcept {
        return error_ == ZipContainerWriteError::None;
    }
    [[nodiscard]] ZipContainerWriteError error() const noexcept { return error_; }
    [[nodiscard]] ZipContainerWriteEntry entryInError() const noexcept { return entry_; }
    [[nodiscard]] const ZipContainerArchive* archive() const& noexcept {
        return archive_.has_value() ? &*archive_ : nullptr;
    }
    [[nodiscard]] const ZipContainerArchive* archive() const&& = delete;

  private:
    friend class detail::ZipContainerWriteBuilder;

    ZipContainerWriteResult() noexcept = default;

    std::optional<ZipContainerArchive> archive_;
    ZipContainerWriteError error_ = ZipContainerWriteError::InvalidLimits;
    ZipContainerWriteEntry entry_ = ZipContainerWriteEntry::None;
};

// Builds a Constrained ZIP Profile archive from `manifest` and `document` bytes, refusing to build
// one outside `limits` (checked arithmetic throughout; overflow is SizeOverflow, never a wrapped
// smaller value) so every returned archive is accepted by readZipContainer() with default limits.
// Per entry, deflate (level 6) is attempted first; the entry falls back to stored (method 0) when
// deflate would not reduce it or would exceed the permitted expansion ratio. Charges the output
// archive buffer, any intermediate deflate buffer, and a documented worst-case reservation for
// zlib's internal deflate working memory through `operation`. Budget rejection is reported as
// ResourceExhausted rather than throwing or falling back to unmetered heap allocation. Never
// throws.
[[nodiscard]] ZipContainerWriteResult
writeZipContainer(std::span<const std::byte> manifest, std::span<const std::byte> document,
                  const ZipContainerLimits& limits, ProjectIoOperationMemory operation) noexcept;

} // namespace bloom::project
