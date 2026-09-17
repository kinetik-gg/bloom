#pragma once

#include <bloom/render/image.hpp>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

// A bounded, content-addressed, on-disk cache for decoded and proxied media. See
// docs/architecture/media-io.md "Disk cache" for the contract this implements and
// docs/architecture/animation-and-time.md for how it relates to the in-memory RAM preview and
// operation caches. Qt-free: usable from both the runtime evaluator and the Qt UI layer.
namespace bloom::media::cache {

// Physical disk-free-derived default, capped so one machine's huge drive cannot silently claim
// dozens of gigabytes: min(10% of free space on the cache volume, capBytes).
inline constexpr std::uint64_t kDefaultMediaDiskCacheCapBytes = 32ULL * 1024 * 1024 * 1024;
// A safeguard independent of the byte budget: even a cache of tiny images stops growing its
// index and directory-entry count past this many entries.
inline constexpr std::uint64_t kDefaultMediaDiskCacheMaxEntries = 200'000;
// Bounded pending-write queue for storeAsync(). A full queue drops the newest write rather than
// blocking the calling (evaluation) thread; a dropped write only means the next read decodes
// again, never incorrect pixels.
inline constexpr std::size_t kMediaDiskCacheAsyncQueueCapacity = 64;

struct MediaDiskCacheConfig final {
    // The cache's private root directory (typically from bloom::platform::userCacheDirectory()
    // plus a "media" leaf). Created lazily on first use.
    std::filesystem::path rootDirectory;
    // 0 defers to kDefaultMediaDiskCacheCapBytes via defaultMediaDiskCacheByteBudget(); a
    // settings override passes its own resolved byte count.
    std::uint64_t byteBudget = 0;
    std::uint64_t maxEntryCount = kDefaultMediaDiskCacheMaxEntries;
    bool enabled = true;
};

struct MediaDiskCacheStatistics final {
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    // Entries removed because their stored digest did not match their bytes on read.
    std::uint64_t corruptDropped = 0;
    std::uint64_t evictions = 0;
    // storeAsync() calls whose write was dropped because the background queue was full.
    std::uint64_t droppedAsyncWrites = 0;
    std::uint64_t entryCount = 0;
    std::uint64_t storedBytes = 0;

    [[nodiscard]] double hitRate() const noexcept {
        const auto total = hits + misses;
        return total == 0 ? 0.0 : static_cast<double>(hits) / static_cast<double>(total);
    }
};

// Content-addressed, LRU-bounded, corruption-checked on-disk store of decoded Float32 RGBA
// images. v0 stores pixels uncompressed with a per-entry SHA-256 digest of the payload bytes
// (see media-io.md); a future task may add a lossless codec through the dependency intake.
//
// Thread-safety: every public member function may be called concurrently from any number of
// threads against one instance (the index, LRU order and statistics are guarded by an internal
// mutex). Two SEPARATE instances rooted at the same directory (e.g. across a process restart, or
// a second evaluator pointed at the same cache root) are also safe with each other: every write
// lands via a temp-file-plus-rename inside the shard directory, and the index file itself is
// rewritten the same atomic way, so a reader never observes a torn file from another writer.
// Two independent instances do not share in-memory LRU/statistics state, so eviction between
// them is only eventually consistent (each instance evicts against what IT has recorded), which
// is acceptable for a best-effort cache: losing track of another instance's entry only means it
// is evicted later than ideal, never that a corrupt or wrong entry is served.
class MediaDiskCache final {
  public:
    explicit MediaDiskCache(MediaDiskCacheConfig config);
    MediaDiskCache(const MediaDiskCache&) = delete;
    MediaDiskCache& operator=(const MediaDiskCache&) = delete;
    MediaDiskCache(MediaDiskCache&&) = delete;
    MediaDiskCache& operator=(MediaDiskCache&&) = delete;
    ~MediaDiskCache();

    // Looks up `key` (a caller-derived, fixed-length lowercase hex digest -- see
    // buildImageCacheKey() in media_disk_cache_decode.hpp -- built from content identity, member
    // frame, interpretation, the Bloom Neutral config digest, and decoder identity/version so a
    // decoder upgrade invalidates old entries). A structurally invalid or digest-mismatched entry
    // is treated as a miss and removed (`corruptDropped`). Disabled or unresolved cache returns a
    // miss without touching disk.
    [[nodiscard]] std::shared_ptr<const render::Rgba32fImage> find(const std::string& key);

    // Writes `image` under `key` synchronously on the CALLING thread: serialize, write to a temp
    // file in the key's shard directory, fsync where the platform supports it, then rename over
    // the final path (atomic on every supported platform). Safe to call from any thread,
    // including concurrently with find()/store() for other keys or the same key.
    void store(const std::string& key, const std::shared_ptr<const render::Rgba32fImage>& image);

    // Queues `store()` on the cache's own single background writer thread and returns
    // immediately without blocking the caller -- the contract evaluateImageSource() needs so
    // evaluation never waits on a disk write. A full queue drops the write immediately (counted
    // in droppedAsyncWrites, never queued indefinitely and never blocks).
    void storeAsync(std::string key, std::shared_ptr<const render::Rgba32fImage> image);

    // Blocks until every write requested by storeAsync() so far has been applied (or dropped).
    // Tests use this to make "second pass never decodes" deterministic; production callers never
    // need to wait on their own write.
    void flush();

    // Removes every entry and resets statistics (used by the "Clear media cache" command).
    void clear();

    [[nodiscard]] MediaDiskCacheStatistics statistics() const;
    void setByteBudget(std::uint64_t bytes);
    [[nodiscard]] std::uint64_t byteBudget() const;
    [[nodiscard]] bool enabled() const noexcept { return enabled_.load(std::memory_order_acquire); }
    void setEnabled(bool value) { enabled_.store(value, std::memory_order_release); }
    [[nodiscard]] const std::filesystem::path& rootDirectory() const noexcept { return root_; }

    // Test-only surface (mirrors the *ForTest() precedent elsewhere in this codebase, e.g.
    // WindowStatusBar): the on-disk path `key` would occupy, so a corruption test can flip a byte
    // without depending on the private sharding scheme.
    [[nodiscard]] std::filesystem::path entryPathForTest(const std::string& key) const {
        return entryPath(key);
    }

  private:
    struct IndexEntry final {
        std::uint64_t bytes = 0;
        std::int64_t lastUseNanos = 0;
    };
    struct AsyncWrite final {
        std::string key;
        std::shared_ptr<const render::Rgba32fImage> image;
    };
    using Lru = std::list<std::string>;

    [[nodiscard]] std::filesystem::path shardDirectory(const std::string& key) const;
    [[nodiscard]] std::filesystem::path entryPath(const std::string& key) const;
    [[nodiscard]] std::filesystem::path indexPath() const;

    void loadIndexLocked();
    void rewriteIndexLocked() const;
    void touchLocked(const std::string& key, std::uint64_t bytes);
    void evictLocked();
    void removeEntryLocked(const std::string& key, bool deleteFile);
    void storeOnCallingThread(const std::string& key,
                              const std::shared_ptr<const render::Rgba32fImage>& image);
    void writerLoop();

    mutable std::mutex mutex_;
    std::filesystem::path root_;
    std::uint64_t byteBudget_;
    std::uint64_t maxEntryCount_;
    std::atomic<bool> enabled_;
    Lru lru_; // front = most recently used
    std::unordered_map<std::string, std::pair<IndexEntry, Lru::iterator>> index_;
    std::uint64_t storedBytes_ = 0;
    MediaDiskCacheStatistics statistics_;
    bool indexLoaded_ = false;

    std::mutex queueMutex_;
    std::condition_variable queueCv_;
    std::condition_variable idleCv_;
    std::deque<AsyncWrite> queue_;
    std::size_t inFlight_ = 0;
    bool stopping_ = false;
    std::thread writer_;
};

// physical-free-space-derived default budget for a cache rooted at `root` (or its nearest
// existing ancestor, when `root` does not exist yet): min(10% of free space, capBytes). Returns
// capBytes when free space cannot be determined.
[[nodiscard]] std::uint64_t
defaultMediaDiskCacheByteBudget(const std::filesystem::path& root,
                                std::uint64_t capBytes = kDefaultMediaDiskCacheCapBytes);

} // namespace bloom::media::cache
