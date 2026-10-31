#pragma once
#include <bloom/media/provider/worker_pool.hpp>
#include <bloom/platform/file_snapshot.hpp>
#include <bloom/render/packed_host_retirement.hpp>
#include <bloom/runtime/memory_budget_ledger.hpp>
#include <filesystem>
#include <list>
#include <mutex>
#include <optional>
namespace bloom::media::video {
[[nodiscard]] bool isVideoExtension(const std::filesystem::path& path);
using Cancel = std::function<bool()>;
// Revalidate: observe the original's identity (size, times, volume/file id) and reuse the verified
// snapshot while it matches the observation that bracketed its full hash; a changed or unknown
// identity captures and rehashes, so changed bytes report SourceChanged. Retained: reuse this
// session's last verified snapshot without touching the original.
enum class SourceRevisionPolicy : std::uint8_t { Revalidate, Retained };
// Live verified snapshots are shared by digest between sessions (non-owning index).
[[nodiscard]] std::size_t sharedSourceSnapshotCount();
struct SourceRevisionAccess final {
    SourceRevisionPolicy policy = SourceRevisionPolicy::Revalidate;
    bool allowRetention = true;
    platform::FileSnapshotStatistics* work = nullptr;
    bool* retained = nullptr;
    platform::ReadOnlyFileSnapshot::Progress progress{};
    std::size_t hostByteBudget = std::numeric_limits<std::size_t>::max();
};
struct FrameKey {
    provider::Digest content;
    std::uint32_t stream = 0;
    std::uint64_t frame = 0;
    std::uint32_t interpretation = 0;
    std::string inputColorSpaceId;
    std::string workingColorSpaceId;
    provider::Digest configRevision;
    // Preview hardware frames never satisfy a software (final-render) lookup, or vice versa.
    bool hardware = false;
    friend bool operator==(const FrameKey&, const FrameKey&) = default;
};
// Automatic decoded-video retention target: one sixteenth of reported physical memory, between
// 256 MiB and 4 GiB. Zero physical capacity selects the 256 MiB floor. The cache registers with
// the process memory ledger, so an observed OS limit still constrains it.
[[nodiscard]] std::size_t automaticDecodedVideoCacheBytes(std::size_t physicalBytes) noexcept;
// Conservative resident estimate for one retained decode worker of a stream: decoder base plus
// 24 frames of 16-bit 4:4:4 planes (reference set, frame threads and the product), capped at the
// stream's media-v1 worker address space (provider::workerAddressSpaceBytes, issue #352). Used to
// admit retained sessions by memory.
[[nodiscard]] std::size_t estimatedDecodeWorkerBytes(std::uint32_t width,
                                                     std::uint32_t height) noexcept;
// Issue #352: the resident byte quota for source snapshots, derived from the free space of the
// snapshot spool volume instead of a fixed 32 GiB: three quarters of the space available there,
// never below 32 GiB (a capture beyond the real free space still fails with its I/O error) and
// never above four 1 TiB sources. `available` is the spool volume's free bytes.
[[nodiscard]] std::uint64_t sourceSnapshotQuotaBytes(std::uint64_t available) noexcept;
// The same for the default spool directory (the user cache), measured now.
[[nodiscard]] std::uint64_t defaultSourceSnapshotQuotaBytes();
// Which decoder produced a frame. Software is the reference; hardware is preview-only.
struct FrameDecodeProvenance final {
    provider::Implementation implementation = provider::Implementation::Software;
    // True when hardware was requested and this frame came from the software fallback.
    bool fallback = false;
    // "software", "vaapi", or "software (fallback: <typed reason>)".
    std::string description = "software";
    // The serving provider (ADR 0035): "ffmpeg", "ffmpeg-vaapi" or the platform framework's id.
    // When the platform framework was retired for this source, `nativeFallback` names why.
    std::string provider = "ffmpeg";
    std::string nativeFallback;
};
struct FrameDecodeRoute final {
    // Interactive preview may use a discovered VA-API decoder for H.264/HEVC. The request is
    // honoured only for retained preview revisions; final rendering always decodes in software.
    bool preferHardware = false;
    FrameDecodeProvenance* provenance = nullptr;
};
// Independent resident byte budget. Callers reserve this budget through their memory ledger.
class DecodedVideoCache final {
  public:
    explicit DecodedVideoCache(std::size_t budget, runtime::MemoryBudgetLedger& ledger =
                                                       runtime::processMemoryBudgetLedger());
    ~DecodedVideoCache();
    [[nodiscard]] std::shared_ptr<const provider::FrameProduct> find(const FrameKey& key);
    void store(const FrameKey& key, std::shared_ptr<const provider::FrameProduct> frame);
    void setByteBudget(std::size_t budget);
    // "Purge media cache": drops every decoded video frame this cache retains. It only releases the
    // cache's own references; a frame already handed to a caller stays valid until that caller
    // releases it. Safe to call from any thread.
    void clear();
    [[nodiscard]] std::size_t residentBytes() const;
    [[nodiscard]] std::size_t byteBudget() const;

  private:
    struct Entry {
        FrameKey key;
        std::shared_ptr<const provider::FrameProduct> frame;
        std::size_t bytes;
    };
    runtime::MemoryBudgetLedger& ledger_;
    mutable std::mutex mutex_;
    std::list<Entry> entries_;
    std::size_t budget_ = 0, resident_ = 0;
};
// Blocking off-UI facade. One worker request per asset at a time: identical frame requests share
// the in-flight result and other requests wait (cancellably) rather than fail Busy. The caller's
// shared task owns admission and completion.
class VideoDecodeSession final {
  public:
    explicit VideoDecodeSession(
        std::filesystem::path source, std::string worker = defaultWorker(),
        std::shared_ptr<platform::FileSnapshotQuota> quota = {},
        std::filesystem::path spoolDirectory = {},
        std::shared_ptr<render::detail::PackedHostRetirementState> retirement = {},
        std::function<void(std::int64_t)> launched = {});
    ~VideoDecodeSession();
    VideoDecodeSession(const VideoDecodeSession&) = delete;
    VideoDecodeSession& operator=(const VideoDecodeSession&) = delete;
    [[nodiscard]] std::optional<provider::Unavailable>
    verifySource(const provider::Digest& expected, const Cancel& cancel = {}) const;
    [[nodiscard]] provider::Result<provider::ProbeResult> probe(const Cancel& cancel = {});
    [[nodiscard]] provider::Result<std::shared_ptr<const provider::FrameProduct>>
    frame(const provider::ProbeResult& source, std::uint32_t stream, std::uint64_t index,
          std::uint32_t interpretation, DecodedVideoCache* cache, const Cancel& cancel = {});
    [[nodiscard]] provider::Result<std::shared_ptr<const provider::FrameProduct>>
    frame(const provider::ProbeResult& source, std::uint32_t stream, std::uint64_t index,
          std::uint32_t interpretation, DecodedVideoCache* cache,
          std::string_view inputColorSpaceId, std::string_view workingColorSpaceId,
          const provider::Digest& configRevision, const Cancel& cancel = {},
          const SourceRevisionAccess& revision = {}, const FrameDecodeRoute& route = {});
    [[nodiscard]] provider::Result<provider::AudioBlock>
    audio(const provider::ProbeResult& source, std::uint32_t stream, std::uint64_t sample,
          std::uint32_t count, const Cancel& cancel = {});
    [[nodiscard]] provider::Result<provider::DemuxIndex>
    index(const provider::ProbeResult& source, const Cancel& cancel = {},
          const SourceRevisionAccess& revision = {});
    [[nodiscard]] static std::string defaultWorker();
    // Host-side capability screen and preference. A VA-API decode worker is attempted only when
    // the platform exposes a DRM render node and either BLOOM_VIDEO_HARDWARE_DECODE is "1" or,
    // automatically, the host has at most kHardwareDecodeMaximumThreads hardware threads (where
    // threaded software decode stops winning). "0" disables the route.
    [[nodiscard]] static bool hardwareDecodePossible();
    static constexpr unsigned kHardwareDecodeMaximumThreads = 4;

  private:
    provider::WorkerReply
    call(provider::Role role, const provider::ProbeResult* source, std::uint32_t stream,
         std::uint64_t position, std::uint32_t count, const Cancel& cancel,
         const std::shared_ptr<const platform::ReadOnlyFileSnapshot>& snapshot = {},
         bool retainWorker = false, bool hardware = false);
    provider::WorkerReply
    callWorker(provider::MediaWorkerPool& workers, const provider::ProviderDeclaration& declaration,
               const provider::ProbeResult* source, std::uint32_t stream, std::uint64_t position,
               std::uint32_t count, const Cancel& cancel,
               const std::shared_ptr<const platform::ReadOnlyFileSnapshot>& snapshot,
               bool retainWorker);
    [[nodiscard]] provider::Result<std::shared_ptr<const platform::ReadOnlyFileSnapshot>>
    sourceRevision(std::optional<provider::Digest> expected, const Cancel& cancel,
                   const SourceRevisionAccess& access) const;
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace bloom::media::video
