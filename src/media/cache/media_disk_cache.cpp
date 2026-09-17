#include <bloom/media/cache/media_disk_cache.hpp>

#include <bloom/core/sha256.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <span>
#include <sstream>
#include <system_error>
#include <thread>

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace bloom::media::cache {

namespace {

constexpr std::array<char, 4> kMagic = {'B', 'L', 'M', '1'};
constexpr std::uint32_t kFormatVersion = 1;
// magic(4) + version(4) + width(4) + height(4) + dataOriginX(8) + dataOriginY(8) +
// displayOriginX(8) + displayOriginY(8) + displayWidth(4) + displayHeight(4) +
// pixelAspectNum(4) + pixelAspectDen(4) + payloadBytes(8) + payloadDigest(32)
constexpr std::size_t kHeaderBytes = 4 + 4 + 4 + 4 + 8 + 8 + 8 + 8 + 4 + 4 + 4 + 4 + 8 + 32;
// A hard corruption-safety ceiling: a header claiming more than this is rejected before any
// allocation is attempted, independent of the decoder's own (smaller) pixel budget.
constexpr std::uint64_t kMaxEntryPayloadBytes = 1ULL * 1024 * 1024 * 1024;

void appendU32(std::vector<std::byte>& out, const std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8)
        out.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
}
void appendU64(std::vector<std::byte>& out, const std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8)
        out.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
}
void appendI64(std::vector<std::byte>& out, const std::int64_t value) {
    appendU64(out, static_cast<std::uint64_t>(value));
}
[[nodiscard]] std::uint32_t readU32(std::span<const std::byte> bytes, const std::size_t offset) {
    std::uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i)
        value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + i]))
                 << (8U * i);
    return value;
}
[[nodiscard]] std::uint64_t readU64(std::span<const std::byte> bytes, const std::size_t offset) {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i)
        value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes[offset + i]))
                 << (8U * i);
    return value;
}
[[nodiscard]] std::int64_t readI64(std::span<const std::byte> bytes, const std::size_t offset) {
    return static_cast<std::int64_t>(readU64(bytes, offset));
}

[[nodiscard]] std::int64_t nowNanos() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Serializes one decoded image to header + raw pixel payload. Payload bytes are the process's
// native in-memory float layout (see media-io.md "Disk cache": v0 is same-machine only; a
// cross-architecture read is caught by the digest/size checks below and treated as a miss, never
// as corrupted pixels).
[[nodiscard]] std::vector<std::byte> serializeEntry(const render::Rgba32fImage& image,
                                                    const core::Sha256Digest& payloadDigest) {
    const auto* descriptor = image.descriptor();
    std::vector<std::byte> out;
    out.reserve(kHeaderBytes + image.pixels().size_bytes());
    for (const auto character : kMagic)
        out.push_back(static_cast<std::byte>(character));
    appendU32(out, kFormatVersion);
    appendU32(out, descriptor->dataWindow().extent().width());
    appendU32(out, descriptor->dataWindow().extent().height());
    appendI64(out, descriptor->dataWindow().originX());
    appendI64(out, descriptor->dataWindow().originY());
    appendI64(out, descriptor->displayWindow().originX());
    appendI64(out, descriptor->displayWindow().originY());
    appendU32(out, descriptor->displayWindow().extent().width());
    appendU32(out, descriptor->displayWindow().extent().height());
    appendU32(out, descriptor->pixelAspect().numerator());
    appendU32(out, descriptor->pixelAspect().denominator());
    appendU64(out, image.pixels().size_bytes());
    for (const auto byte : payloadDigest.bytes())
        out.push_back(static_cast<std::byte>(byte));
    const auto payload = std::as_bytes(image.pixels());
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

struct ParsedHeader final {
    std::uint32_t dataWidth = 0;
    std::uint32_t dataHeight = 0;
    std::int64_t dataOriginX = 0;
    std::int64_t dataOriginY = 0;
    std::int64_t displayOriginX = 0;
    std::int64_t displayOriginY = 0;
    std::uint32_t displayWidth = 0;
    std::uint32_t displayHeight = 0;
    std::uint32_t pixelAspectNumerator = 0;
    std::uint32_t pixelAspectDenominator = 0;
    std::uint64_t payloadBytes = 0;
    core::Sha256Digest payloadDigest;
};

[[nodiscard]] std::optional<ParsedHeader> parseHeader(std::span<const std::byte> bytes) {
    if (bytes.size() < kHeaderBytes)
        return std::nullopt;
    for (std::size_t i = 0; i < kMagic.size(); ++i)
        if (bytes[i] != static_cast<std::byte>(kMagic[i]))
            return std::nullopt;
    if (readU32(bytes, 4) != kFormatVersion)
        return std::nullopt;
    ParsedHeader header;
    header.dataWidth = readU32(bytes, 8);
    header.dataHeight = readU32(bytes, 12);
    header.dataOriginX = readI64(bytes, 16);
    header.dataOriginY = readI64(bytes, 24);
    header.displayOriginX = readI64(bytes, 32);
    header.displayOriginY = readI64(bytes, 40);
    header.displayWidth = readU32(bytes, 48);
    header.displayHeight = readU32(bytes, 52);
    header.pixelAspectNumerator = readU32(bytes, 56);
    header.pixelAspectDenominator = readU32(bytes, 60);
    header.payloadBytes = readU64(bytes, 64);
    std::array<std::uint8_t, core::kSha256DigestBytes> digestBytes{};
    for (std::size_t i = 0; i < digestBytes.size(); ++i)
        digestBytes[i] = std::to_integer<std::uint8_t>(bytes[72 + i]);
    header.payloadDigest = core::Sha256Digest::fromBytes(digestBytes);
    return header;
}

// Rebuilds an immutable Rgba32fImage from a validated header and payload. Returns nullopt on any
// structural problem (never partially publishes an image).
[[nodiscard]] std::optional<render::Rgba32fImage> rebuildImage(const ParsedHeader& header,
                                                               std::span<const std::byte> payload) {
    const auto dataWindow = render::ImageWindow::create(header.dataOriginX, header.dataOriginY,
                                                        header.dataWidth, header.dataHeight);
    const auto displayWindow = render::ImageWindow::create(
        header.displayOriginX, header.displayOriginY, header.displayWidth, header.displayHeight);
    const auto pixelAspect =
        core::PixelAspectRatio::create(header.pixelAspectNumerator, header.pixelAspectDenominator);
    if (!dataWindow || !displayWindow || !pixelAspect)
        return std::nullopt;
    const auto descriptor = render::Rgba32fImageDescriptor::create(
        *dataWindow.value(), *displayWindow.value(), *pixelAspect);
    if (!descriptor)
        return std::nullopt;
    // payload.size_bytes() is exact (the caller already checked file size == header + payload),
    // so this is really "does the declared extent match the bytes actually present".
    if (payload.size() != header.payloadBytes)
        return std::nullopt;
    const auto rowWidth = header.dataWidth;
    const auto rowBytes = static_cast<std::size_t>(rowWidth) * sizeof(render::Rgba32f);
    if (rowBytes != 0 && payload.size() != static_cast<std::size_t>(header.dataHeight) * rowBytes)
        return std::nullopt;
    auto builder = render::Rgba32fImageBuilder::create(*descriptor.value(), kMaxEntryPayloadBytes);
    if (!builder)
        return std::nullopt;
    for (std::uint32_t y = 0; y < header.dataHeight; ++y) {
        auto row =
            builder.value()->row(dataWindow.value()->originY() + static_cast<std::int64_t>(y));
        if (!row)
            return std::nullopt;
        const auto rowSpan = std::as_writable_bytes(*row.value());
        if (rowSpan.size_bytes() != rowBytes)
            return std::nullopt;
        std::memcpy(rowSpan.data(), payload.data() + static_cast<std::size_t>(y) * rowBytes,
                    rowBytes);
    }
    auto frozen = std::move(*builder.value()).freeze();
    if (!frozen)
        return std::nullopt;
    return std::move(*frozen.value());
}

[[nodiscard]] std::string uniqueSuffix() {
    static std::atomic<std::uint64_t> counter{0};
    std::ostringstream out;
    out << std::hex << std::this_thread::get_id() << '-' << nowNanos() << '-'
        << counter.fetch_add(1, std::memory_order_relaxed);
    return out.str();
}

// Writes `bytes` to `path` atomically: a temp file in the same directory, fsync'd where the
// platform supports it, then renamed over the target (atomic replace on every platform the
// project ships on). Returns false without leaving a partial file behind on any failure.
[[nodiscard]] bool atomicWrite(const std::filesystem::path& path,
                               const std::vector<std::byte>& bytes) {
    std::error_code createError;
    std::filesystem::create_directories(path.parent_path(), createError);
    const auto tempPath =
        path.parent_path() / (path.filename().string() + ".tmp-" + uniqueSuffix());
    {
        std::ofstream out(tempPath, std::ios::binary | std::ios::trunc);
        if (!out)
            return false;
        out.write(reinterpret_cast<const char*>(bytes.data()), // NOLINT(*-reinterpret-cast)
                  static_cast<std::streamsize>(bytes.size()));
        if (!out) {
            out.close();
            std::error_code removeError;
            std::filesystem::remove(tempPath, removeError);
            return false;
        }
        out.flush();
        if (!out) {
            out.close();
            std::error_code removeError;
            std::filesystem::remove(tempPath, removeError);
            return false;
        }
    }
#if !defined(_WIN32)
    {
        const int descriptor = ::open(tempPath.c_str(), O_WRONLY); // NOLINT(*-vararg)
        if (descriptor >= 0) {
            static_cast<void>(::fsync(descriptor));
            static_cast<void>(::close(descriptor));
        }
    }
#endif
    std::error_code renameError;
    std::filesystem::rename(tempPath, path, renameError);
    if (renameError) {
        std::error_code removeError;
        std::filesystem::remove(tempPath, removeError);
        return false;
    }
    return true;
}

[[nodiscard]] std::optional<std::vector<std::byte>> readWholeFile(const std::filesystem::path& path,
                                                                  const std::uint64_t byteLimit) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input)
        return std::nullopt;
    const auto size = input.tellg();
    if (size < 0 || static_cast<std::uint64_t>(size) > byteLimit)
        return std::nullopt;
    input.seekg(0);
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    if (!bytes.empty())
        input.read(reinterpret_cast<char*>(bytes.data()), // NOLINT(*-reinterpret-cast)
                   static_cast<std::streamsize>(bytes.size()));
    if (!input && !input.eof())
        return std::nullopt;
    return bytes;
}

} // namespace

MediaDiskCache::MediaDiskCache(MediaDiskCacheConfig config)
    : root_(std::move(config.rootDirectory)),
      byteBudget_(config.byteBudget != 0 ? config.byteBudget : kDefaultMediaDiskCacheCapBytes),
      maxEntryCount_(config.maxEntryCount),
      asyncQueueByteCapacity_(config.asyncQueueByteCapacity != 0
                                  ? config.asyncQueueByteCapacity
                                  : kMediaDiskCacheAsyncQueueByteCapacity),
      enabled_(config.enabled) {
    writer_ = std::thread([this] { writerLoop(); });
}

MediaDiskCache::~MediaDiskCache() {
    {
        std::lock_guard lock(queueMutex_);
        stopping_ = true;
    }
    queueCv_.notify_all();
    if (writer_.joinable())
        writer_.join();
}

std::filesystem::path MediaDiskCache::shardDirectory(const std::string& key) const {
    const auto shard = key.size() >= 2 ? key.substr(0, 2) : std::string("00");
    return root_ / "entries" / shard;
}
std::filesystem::path MediaDiskCache::entryPath(const std::string& key) const {
    return shardDirectory(key) / (key + ".blm");
}
std::filesystem::path MediaDiskCache::indexPath() const { return root_ / "index.v1"; }

void MediaDiskCache::loadIndexLocked() {
    if (indexLoaded_ || root_.empty())
        return;
    indexLoaded_ = true;
    std::ifstream input(indexPath());
    if (!input)
        return;
    struct Loaded final {
        std::string key;
        IndexEntry entry;
    };
    std::vector<Loaded> loaded;
    std::string key;
    std::uint64_t bytes = 0;
    std::int64_t lastUse = 0;
    while (input >> key >> bytes >> lastUse)
        loaded.push_back({key, IndexEntry{bytes, lastUse}});
    std::ranges::sort(loaded, std::ranges::greater{},
                      [](const Loaded& item) { return item.entry.lastUseNanos; });
    for (auto& item : loaded) {
        if (index_.contains(item.key))
            continue;
        if (!std::filesystem::exists(entryPath(item.key)))
            continue;
        lru_.push_back(item.key);
        index_.emplace(item.key, std::make_pair(item.entry, std::prev(lru_.end())));
        storedBytes_ += item.entry.bytes;
    }
    statistics_.entryCount = index_.size();
    statistics_.storedBytes = storedBytes_;
}

void MediaDiskCache::rewriteIndexLocked() const {
    if (root_.empty())
        return;
    std::ostringstream out;
    for (const auto& key : lru_) {
        const auto& entry = index_.at(key).first;
        out << key << ' ' << entry.bytes << ' ' << entry.lastUseNanos << '\n';
    }
    const auto text = out.str();
    std::vector<std::byte> bytes(text.size());
    std::memcpy(bytes.data(), text.data(), text.size());
    static_cast<void>(atomicWrite(indexPath(), bytes));
}

void MediaDiskCache::touchLocked(const std::string& key, const std::uint64_t bytes) {
    const auto found = index_.find(key);
    if (found != index_.end()) {
        storedBytes_ -= found->second.first.bytes;
        lru_.erase(found->second.second);
        index_.erase(found);
    }
    lru_.push_front(key);
    index_.emplace(key, std::make_pair(IndexEntry{bytes, nowNanos()}, lru_.begin()));
    storedBytes_ += bytes;
    statistics_.entryCount = index_.size();
    statistics_.storedBytes = storedBytes_;
}

void MediaDiskCache::removeEntryLocked(const std::string& key, const bool deleteFile) {
    const auto found = index_.find(key);
    if (found == index_.end())
        return;
    storedBytes_ -= found->second.first.bytes;
    lru_.erase(found->second.second);
    index_.erase(found);
    statistics_.entryCount = index_.size();
    statistics_.storedBytes = storedBytes_;
    if (deleteFile) {
        std::error_code removeError;
        std::filesystem::remove(entryPath(key), removeError);
    }
}

void MediaDiskCache::evictLocked() {
    bool evicted = false;
    while (!lru_.empty() && (storedBytes_ > byteBudget_ || index_.size() > maxEntryCount_)) {
        const auto& victim = lru_.back();
        std::error_code removeError;
        std::filesystem::remove(entryPath(victim), removeError);
        storedBytes_ -= index_.at(victim).first.bytes;
        index_.erase(victim);
        lru_.pop_back();
        ++statistics_.evictions;
        evicted = true;
    }
    if (evicted) {
        statistics_.entryCount = index_.size();
        statistics_.storedBytes = storedBytes_;
        rewriteIndexLocked();
    }
}

std::shared_ptr<const render::Rgba32fImage> MediaDiskCache::find(const std::string& key) {
    if (!enabled() || root_.empty())
        return nullptr;
    std::lock_guard lock(mutex_);
    loadIndexLocked();
    const auto found = index_.find(key);
    if (found == index_.end()) {
        ++statistics_.misses;
        return nullptr;
    }
    const auto expectedBytes = kHeaderBytes + found->second.first.bytes;
    auto raw = readWholeFile(entryPath(key), expectedBytes + 1);
    if (!raw || raw->size() != expectedBytes) {
        removeEntryLocked(key, true);
        ++statistics_.corruptDropped;
        ++statistics_.misses;
        rewriteIndexLocked();
        return nullptr;
    }
    const auto header = parseHeader(*raw);
    if (!header || header->payloadBytes > kMaxEntryPayloadBytes) {
        removeEntryLocked(key, true);
        ++statistics_.corruptDropped;
        ++statistics_.misses;
        rewriteIndexLocked();
        return nullptr;
    }
    const std::span payload(*raw);
    const auto payloadSpan = payload.subspan(kHeaderBytes);
    const auto digest = core::Sha256Hasher::hash(payloadSpan);
    if (!digest || *digest != header->payloadDigest) {
        removeEntryLocked(key, true);
        ++statistics_.corruptDropped;
        ++statistics_.misses;
        rewriteIndexLocked();
        return nullptr;
    }
    auto rebuilt = rebuildImage(*header, payloadSpan);
    if (!rebuilt) {
        removeEntryLocked(key, true);
        ++statistics_.corruptDropped;
        ++statistics_.misses;
        rewriteIndexLocked();
        return nullptr;
    }
    touchLocked(key, found->second.first.bytes);
    ++statistics_.hits;
    return std::make_shared<const render::Rgba32fImage>(std::move(*rebuilt));
}

void MediaDiskCache::storeOnCallingThread(
    const std::string& key, const std::shared_ptr<const render::Rgba32fImage>& image) {
    if (!enabled() || root_.empty() || !image || !image->isValid())
        return;
    const auto payload = std::as_bytes(image->pixels());
    if (payload.size() > kMaxEntryPayloadBytes)
        return;
    const auto digest = core::Sha256Hasher::hash(payload);
    if (!digest)
        return;
    const auto serialized = serializeEntry(*image, *digest);
    if (!atomicWrite(entryPath(key), serialized))
        return;
    std::lock_guard lock(mutex_);
    loadIndexLocked();
    touchLocked(key, payload.size_bytes());
    evictLocked();
    rewriteIndexLocked();
}

void MediaDiskCache::store(const std::string& key,
                           const std::shared_ptr<const render::Rgba32fImage>& image) {
    storeOnCallingThread(key, image);
}

void MediaDiskCache::storeAsync(std::string key,
                                std::shared_ptr<const render::Rgba32fImage> image) {
    if (!enabled() || root_.empty())
        return;
    // CACHEFIX-1: charge the write against the queue's byte capacity BEFORE queueing it. A queued
    // write is the last owner of a decoded Float32 image, so this figure is real process memory --
    // the 38 GiB incident's largest single unaccounted pool. An image that alone exceeds the
    // capacity is refused here rather than staged and then discarded by the writer.
    const std::uint64_t bytes =
        image && image->isValid() ? std::as_bytes(image->pixels()).size_bytes() : 0;
    const auto dropped = [this] {
        std::lock_guard statsLock(mutex_);
        ++statistics_.droppedAsyncWrites;
    };
    {
        std::lock_guard lock(queueMutex_);
        if (stopping_)
            return;
        if (queue_.size() >= kMediaDiskCacheAsyncQueueCapacity ||
            bytes > asyncQueueByteCapacity_ - std::min(queuedBytes_, asyncQueueByteCapacity_)) {
            dropped();
            return;
        }
        queue_.push_back({std::move(key), std::move(image), bytes});
        queuedBytes_ += bytes;
        const auto queued = queuedBytes_;
        const std::lock_guard statsLock(mutex_);
        statistics_.asyncQueueBytes = queued;
        statistics_.peakAsyncQueueBytes = std::max(statistics_.peakAsyncQueueBytes, queued);
    }
    queueCv_.notify_one();
}

void MediaDiskCache::writerLoop() {
    while (true) {
        AsyncWrite work;
        {
            std::unique_lock lock(queueMutex_);
            queueCv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (queue_.empty()) {
                if (stopping_)
                    return;
                continue;
            }
            work = std::move(queue_.front());
            queue_.pop_front();
            ++inFlight_;
            // queuedBytes_ deliberately still counts this write: dequeuing it does not free the
            // image, the writer below is now the one holding it, and an account that dropped here
            // would let one more image in than the capacity allows.
        }
        storeOnCallingThread(work.key, work.image);
        // The write is done: release the image, and only then give its bytes back, so the account
        // and the memory it stands for fall at the same moment.
        work.image.reset();
        {
            std::lock_guard lock(queueMutex_);
            queuedBytes_ -= std::min(queuedBytes_, work.bytes);
            --inFlight_;
            const auto queued = queuedBytes_;
            {
                const std::lock_guard statsLock(mutex_);
                statistics_.asyncQueueBytes = queued;
            }
            if (queue_.empty() && inFlight_ == 0)
                idleCv_.notify_all();
        }
    }
}

void MediaDiskCache::flush() {
    std::unique_lock lock(queueMutex_);
    idleCv_.wait(lock, [this] { return queue_.empty() && inFlight_ == 0; });
}

void MediaDiskCache::clear() {
    flush();
    std::lock_guard lock(mutex_);
    loadIndexLocked();
    for (const auto& key : lru_) {
        std::error_code removeError;
        std::filesystem::remove(entryPath(key), removeError);
    }
    lru_.clear();
    index_.clear();
    storedBytes_ = 0;
    statistics_ = {};
    rewriteIndexLocked();
}

MediaDiskCacheStatistics MediaDiskCache::statistics() const {
    std::lock_guard lock(mutex_);
    return statistics_;
}

void MediaDiskCache::setByteBudget(const std::uint64_t bytes) {
    std::lock_guard lock(mutex_);
    byteBudget_ = bytes;
    loadIndexLocked();
    evictLocked();
}

std::uint64_t MediaDiskCache::byteBudget() const {
    std::lock_guard lock(mutex_);
    return byteBudget_;
}

std::uint64_t defaultMediaDiskCacheByteBudget(const std::filesystem::path& root,
                                              const std::uint64_t capBytes) {
    auto probe = root;
    std::error_code error;
    while (!probe.empty() && !std::filesystem::exists(probe, error)) {
        const auto parent = probe.parent_path();
        if (parent == probe)
            break;
        probe = parent;
    }
    if (probe.empty())
        return capBytes;
    const auto space = std::filesystem::space(probe, error);
    if (error)
        return capBytes;
    const auto tenPercent = space.available / 10;
    return std::min<std::uint64_t>(tenPercent, capBytes);
}

} // namespace bloom::media::cache
