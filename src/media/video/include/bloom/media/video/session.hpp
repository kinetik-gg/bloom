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
enum class SourceRevisionPolicy : std::uint8_t { Revalidate, Retained };
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
    friend bool operator==(const FrameKey&, const FrameKey&) = default;
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
// Blocking off-UI facade. One bounded in-flight request per asset; excess calls return Busy.
// The private scheduler prevents deadlock when import/evaluation itself runs on BlockingIo.
class VideoDecodeSession final {
  public:
    explicit VideoDecodeSession(
        std::filesystem::path source, std::string worker = defaultWorker(),
        std::shared_ptr<platform::FileSnapshotQuota> quota = {},
        std::filesystem::path spoolDirectory = {},
        std::shared_ptr<render::detail::PackedHostRetirementState> retirement = {});
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
          const SourceRevisionAccess& revision = {});
    [[nodiscard]] provider::Result<provider::AudioBlock>
    audio(const provider::ProbeResult& source, std::uint32_t stream, std::uint64_t sample,
          std::uint32_t count, const Cancel& cancel = {});
    [[nodiscard]] provider::Result<provider::DemuxIndex> index(const provider::ProbeResult& source,
                                                               const Cancel& cancel = {});
    [[nodiscard]] static std::string defaultWorker();

  private:
    provider::WorkerReply call(provider::Role role, const provider::ProbeResult* source,
                               std::uint32_t stream, std::uint64_t position, std::uint32_t count,
                               const Cancel& cancel,
                               std::shared_ptr<const platform::ReadOnlyFileSnapshot> snapshot = {});
    [[nodiscard]] provider::Result<std::shared_ptr<const platform::ReadOnlyFileSnapshot>>
    sourceRevision(std::optional<provider::Digest> expected, const Cancel& cancel,
                   const SourceRevisionAccess& access) const;
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace bloom::media::video
