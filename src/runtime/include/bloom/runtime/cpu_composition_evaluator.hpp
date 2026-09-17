#pragma once

#include <bloom/runtime/cancellation.hpp>
#include <bloom/runtime/evaluation.hpp>
#include <bloom/runtime/row_band_execution.hpp>

#include <filesystem>
#include <memory>
#include <mutex>

namespace bloom::media::cache {
class MediaDiskCache;
} // namespace bloom::media::cache

namespace bloom::runtime {

// A device-free audio projection of one compiled composition. It carries only the source identity
// and time-varying controls; decoding and device ownership stay at the media/audio boundary.
struct AudioClipDescription final {
    document::NodeId sourceNodeId;
    document::AssetId assetId;
    core::RationalTime startTime{};
    core::RationalTime endTime{};
    double level = 1.0;
    bool muted = false;
    bool solo = false;

    friend bool operator==(const AudioClipDescription&, const AudioClipDescription&) = default;
};

struct AudioMixDescription final {
    document::NodeId outputNodeId;
    std::vector<AudioClipDescription> clips;

    friend bool operator==(const AudioMixDescription&, const AudioMixDescription&) = default;
};

class CpuCompositionEvaluator final {
  public:
    void setAssetBaseDirectory(std::filesystem::path directory) const {
        std::lock_guard lock(assetContext_->mutex);
        assetContext_->directory = std::move(directory);
    }
    [[nodiscard]] std::filesystem::path assetBaseDirectory() const {
        std::lock_guard lock(assetContext_->mutex);
        return assetContext_->directory;
    }
    // Owned by the host application (media-io.md "Disk cache"): a shared, bounded, on-disk store
    // of decoded image frames. Null (the default) disables the disk-cache stage entirely --
    // evaluateImageSource() falls back to memory-cache-or-decode exactly as before this store
    // existed. Never consulted or written for an interactive/overridden request (see evaluate()).
    void setMediaDiskCache(std::shared_ptr<media::cache::MediaDiskCache> diskCache) const {
        std::lock_guard lock(assetContext_->mutex);
        assetContext_->diskCache = std::move(diskCache);
    }
    [[nodiscard]] std::shared_ptr<media::cache::MediaDiskCache> mediaDiskCache() const {
        std::lock_guard lock(assetContext_->mutex);
        return assetContext_->diskCache;
    }
    [[nodiscard]] const std::shared_ptr<OperationCache>& operationCache() const { return cache_; }
    // `rowBands` is the bounded pool the per-row kernels are spread across (the scheduler owns one;
    // TaskContext::rowBandExecutor() is where a task body gets it). Null evaluates every row band
    // on the calling thread, in band order.
    //
    // The pixels are the same either way, to the bit: banding partitions rows, and every row kernel
    // writes only its own row. The evaluator therefore does NOT enter the pool into
    // ProcessFrameIdentity -- a frame evaluated in parallel and the same frame evaluated serially
    // are the same frame, and a cache that holds one may serve the other.
    [[nodiscard]] EvaluationResult evaluate(std::shared_ptr<const CompiledCompositionPlan> plan,
                                            const EvaluationRequest& request,
                                            const CancellationToken& cancellation,
                                            EvaluationProgressCallback progress = {},
                                            CpuRowBandExecutor* rowBands = nullptr,
                                            OperationCacheStatistics* statistics = nullptr) const;

    // Resolves the compiled audio graph at `time` without touching pixels, a device, or the
    // filesystem. Curve-backed and value-graph-backed levels use the same exact evaluators as the
    // image path. A malformed audio operand returns nullopt instead of silently changing the mix.
    [[nodiscard]] std::optional<AudioMixDescription>
    evaluateAudioMix(const std::shared_ptr<const CompiledCompositionPlan>& plan,
                     core::RationalTime time, const CancellationToken& cancellation = {}) const;

  private:
    struct AssetContext {
        std::mutex mutex;
        std::filesystem::path directory;
        std::shared_ptr<media::cache::MediaDiskCache> diskCache;
    };
    std::shared_ptr<AssetContext> assetContext_ = std::make_shared<AssetContext>();
    std::shared_ptr<OperationCache> cache_ = std::make_shared<OperationCache>();
};

} // namespace bloom::runtime
