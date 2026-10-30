#include <algorithm>
#include <array>
#include <bloom/media/provider/ffmpeg_launch.hpp>
#include <bloom/media/provider/ffmpeg_manifest.hpp>
#include <bloom/media/provider/media_workers.hpp>
#if defined(__APPLE__)
#include <bloom/media/provider/videotoolbox_manifest.hpp>
#endif
#include <bloom/media/video/session.hpp>
#include <bloom/platform/cache_directory.hpp>
#include <cctype>
#include <condition_variable>
#include <deque>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <new>
#include <thread>
#include <type_traits>
namespace bloom::media::video {
bool isVideoExtension(const std::filesystem::path& path) {
    auto extension = path.extension().string();
    std::ranges::transform(extension, extension.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension == ".mp4" || extension == ".mov" || extension == ".mkv" ||
           extension == ".mxf" || extension == ".mts" || extension == ".m2ts" ||
           extension == ".ts" || extension == ".m4v" || extension == ".webm";
}

std::size_t automaticDecodedVideoCacheBytes(const std::size_t physicalBytes) noexcept {
    constexpr std::size_t floor = std::size_t{256} << 20U;
    constexpr std::size_t ceiling = std::size_t{4} << 30U;
    return std::clamp(physicalBytes / 16U, floor, ceiling);
}
std::size_t estimatedDecodeWorkerBytes(const std::uint32_t width,
                                       const std::uint32_t height) noexcept {
    constexpr std::uint64_t base = 64ULL << 20U;
    const auto frame = static_cast<std::uint64_t>(width) * height * 3U * 2U;
    return static_cast<std::size_t>(
        std::min(provider::workerAddressSpaceBytes(width, height), base + frame * 24U));
}
std::uint64_t sourceSnapshotQuotaBytes(const std::uint64_t available) noexcept {
    constexpr std::uint64_t floor = 32ULL << 30U, ceiling = 4U * provider::Limits::sourceBytes;
    return std::clamp<std::uint64_t>(available / 4U * 3U, floor, ceiling);
}
std::uint64_t defaultSourceSnapshotQuotaBytes() {
    std::uint64_t available = 0;
    if (const auto cache = platform::userCacheDirectory("bloom")) {
        // The spool directory may not exist yet: measure its nearest existing ancestor.
        std::error_code error;
        auto directory = *cache / "video-source-snapshots";
        while (!directory.empty() && !std::filesystem::exists(directory, error)) {
            const auto parent = directory.parent_path();
            if (parent == directory)
                break;
            directory = parent;
        }
        const auto space = std::filesystem::space(directory, error);
        if (!error)
            available = space.available;
    }
    return sourceSnapshotQuotaBytes(available);
}
bool VideoDecodeSession::hardwareDecodePossible() {
#if defined(__linux__)
    const auto* value = std::getenv("BLOOM_VIDEO_HARDWARE_DECODE");
    const std::string_view mode = value ? value : "";
    if (mode == "0")
        return false;
    // Automatic preference: with SIMD and threads, software decode was faster than VA-API on a
    // 16-thread host and VA-API only won with four or fewer threads
    // (docs/architecture/media-io.md).
    if (mode != "1" && std::thread::hardware_concurrency() > kHardwareDecodeMaximumThreads)
        return false;
    // Render nodes are enumerated once per process; the worker discovers the usable one.
    static const bool renderNode = [] {
        std::error_code error;
        for (std::filesystem::directory_iterator it("/dev/dri", error), end; !error && it != end;
             it.increment(error))
            if (it->path().filename().string().starts_with("renderD"))
                return true;
        return false;
    }();
    return renderNode;
#else
    return false;
#endif
}

namespace {
using namespace provider;
// The video read providers (ADR 0035): the bundled FFmpeg worker on every platform, preceded by
// the platform media framework where Bloom has one (VideoToolbox on macOS). The host and each
// worker build the same handshake, so validation agrees.
PipelineQualificationV1 providerPipeline(const ProviderDeclaration& declaration) {
#if defined(__APPLE__)
    if (declaration.execution.provider == "bloom.videotoolbox")
        return videoToolboxPipeline(declaration);
#endif
    return ffmpegPipeline(declaration);
}
// Waits for a session-level lock while honouring cancellation; queued requests never see Busy.
bool acquire(std::unique_lock<std::timed_mutex>& lock, const Cancel& cancel) {
    while (!lock.try_lock_for(std::chrono::milliseconds(20)))
        if (cancel && cancel())
            return false;
    return true;
}
template <typename T> Result<T> product(WorkerReply reply) {
    if (auto* value = std::get_if<T>(&reply))
        return std::move(*value);
    if (const auto* error = std::get_if<Unavailable>(&reply))
        return *error;
    return Unavailable{Error::UnexpectedMessage, "Unexpected video worker product"};
}
} // namespace
struct VideoDecodeSession::State {
    using FrameOutcome = Result<std::shared_ptr<const FrameProduct>>;
    static_assert(std::is_nothrow_move_assignable_v<FrameOutcome>);
    struct InFlightFrame final {
        explicit InFlightFrame(FrameKey value) : key(std::move(value)) {}
        FrameKey key;
        bool done = false;
        // Construct the terminal fallback before this flight is visible to another caller.
        FrameOutcome result = Unavailable{Error::Io, "Video frame failed"};
        FrameDecodeProvenance provenance;
        std::condition_variable completed;
    };
    std::filesystem::path path;
    Handshake hello = ffmpegHandshake();
    CapabilityRegistry registry;
    MediaWorkerPool pool;
    // The platform media framework's read provider, tried before FFmpeg for the operations it
    // declares. Present only for the default worker on a platform with a native worker; retired
    // for this session after its first failure, after which FFmpeg serves every request.
    Handshake nativeHello;
    std::unique_ptr<MediaWorkerPool> nativePool;
    std::atomic<bool> nativeRetired{false};
    std::string nativeFallback;
    // Preview-only VA-API decode generation (Linux). Created on first eligible request and
    // permanently retired for this session after its first typed hardware refusal.
    Handshake hardwareHello;
    std::string workerExecutable;
    std::function<void(std::int64_t)> workerLaunched;
    std::unique_ptr<MediaWorkerPool> hardwarePool;
    std::mutex hardwareMutex;
    std::atomic<bool> hardwareRetired{false};
    std::string hardwareFallback;
    std::timed_mutex mutex;
    std::mutex frameMutex;
    std::shared_ptr<InFlightFrame> inFlightFrame;
    std::mutex revisionMutex;
    std::shared_ptr<const platform::ReadOnlyFileSnapshot> revision;
    std::shared_ptr<platform::FileSnapshotQuota> quota;
    bool privateQuota = false; // the session created `quota` itself (no caller capacity)
    std::filesystem::path spoolDirectory;
    std::shared_ptr<render::detail::PackedHostRetirementState> retirement;
    struct FrameCompletion final {
        State& state;
        InFlightFrame& flight;
        ~FrameCompletion() noexcept {
            {
                const std::lock_guard lock(state.frameMutex);
                flight.done = true;
                state.inFlightFrame.reset();
            }
            flight.completed.notify_all();
        }
    };
    State(std::filesystem::path source, std::string worker,
          std::shared_ptr<platform::FileSnapshotQuota> limits, std::filesystem::path directory,
          std::shared_ptr<render::detail::PackedHostRetirementState> owner,
          std::function<void(std::int64_t)> launched)
        : path(std::move(source)), pool(options(worker, launched)), workerExecutable(worker),
          workerLaunched(launched), quota(std::move(limits)), spoolDirectory(std::move(directory)),
          retirement(std::move(owner)) {
        if (!quota) {
            quota =
                std::make_shared<platform::FileSnapshotQuota>(defaultSourceSnapshotQuotaBytes(), 8);
            privateQuota = true;
        }
        if (spoolDirectory.empty()) {
            if (const auto cache = platform::userCacheDirectory("bloom"))
                spoolDirectory = *cache / "video-source-snapshots";
        }
        for (const auto& declaration : hello.declarations) {
            (void)registry.registerProvider(declaration);
            (void)registry.qualifyPipeline(providerPipeline(declaration));
        }
        if (const auto native = nativeWorkerExecutable();
            !native.empty() && workerExecutable == ffmpegWorkerExecutable() &&
            regularFileExists(native)) {
            nativeHello = nativeMediaHandshake();
            for (const auto& declaration : nativeHello.declarations) {
                (void)registry.registerProvider(declaration);
                (void)registry.qualifyPipeline(providerPipeline(declaration));
            }
            WorkerPoolOptions o;
            o.process.executable = native;
            o.expected = nativeHello;
            o.capacity = 1;
            o.timeout = std::chrono::seconds(30);
            o.launched = workerLaunched;
            nativePool = std::make_unique<MediaWorkerPool>(std::move(o));
        }
#if defined(__linux__)
        hardwareHello = ffmpegHardwareDecodeHandshake();
        for (const auto& declaration : hardwareHello.declarations) {
            (void)registry.registerProvider(declaration);
            (void)registry.qualifyPipeline(providerPipeline(declaration));
        }
#endif
    }
    MediaWorkerPool* hardwareWorkers() {
        const std::lock_guard lock(hardwareMutex);
        if (!hardwarePool && !hardwareRetired.load()) {
            auto o = options(workerExecutable, workerLaunched);
            o.expected = hardwareHello;
            o.process.arguments.emplace_back("--vaapi-decode");
            hardwarePool = std::make_unique<MediaWorkerPool>(std::move(o));
        }
        return hardwarePool.get();
    }
    // Explicit, sticky software fallback. The retired worker and its source handle are released
    // off the caller's lock order; later preview requests report the typed reason.
    void retireHardware(std::string reason) {
        // Worker calls hold `mutex` while they use the pool, so retirement waits for them.
        const std::lock_guard serial(mutex);
        std::unique_ptr<MediaWorkerPool> retired;
        {
            const std::lock_guard lock(hardwareMutex);
            if (hardwareRetired.exchange(true))
                return;
            hardwareFallback = std::move(reason);
            retired = std::move(hardwarePool);
        }
    }
    [[nodiscard]] std::string hardwareFallbackReason() {
        const std::lock_guard lock(hardwareMutex);
        return hardwareFallback;
    }
    void retireNative(std::string reason) {
        // Called with `mutex` held by the failing call; the pool is released after its reply.
        if (nativeRetired.exchange(true))
            return;
        const std::lock_guard lock(hardwareMutex);
        nativeFallback = std::string(nativeMediaProviderId()) + " " + std::move(reason);
    }
    [[nodiscard]] std::string nativeFallbackReason() {
        const std::lock_guard lock(hardwareMutex);
        return nativeFallback;
    }
    void releaseRetainedWorkers() {
        pool.releaseRetained();
        if (nativePool)
            nativePool->releaseRetained();
        const std::lock_guard lock(hardwareMutex);
        if (hardwarePool)
            hardwarePool->releaseRetained();
    }
    WorkerPoolOptions options(std::string executable, std::function<void(std::int64_t)> launched) {
        WorkerPoolOptions options;
        options.process.executable = std::move(executable);
        // One retained worker per pool decodes in presentation order; the session queues callers
        // instead of refusing them. The watchdog bounds one worker exchange, not the queue.
        configureFfmpegWorkerEnvironment(options.process, options.process.executable);
        options.expected = hello;
        options.capacity = 1;
        options.timeout = std::chrono::seconds(30);
        options.launched = std::move(launched);
#if defined(__linux__)
        options.retainedVideo = true;
#endif
        return options;
    }
};
VideoDecodeSession::VideoDecodeSession(
    std::filesystem::path source, std::string worker,
    std::shared_ptr<platform::FileSnapshotQuota> quota, std::filesystem::path spoolDirectory,
    std::shared_ptr<render::detail::PackedHostRetirementState> retirement,
    std::function<void(std::int64_t)> launched)
    : state_(std::make_unique<State>(std::move(source), std::move(worker), std::move(quota),
                                     std::move(spoolDirectory), std::move(retirement),
                                     std::move(launched))) {}
VideoDecodeSession::~VideoDecodeSession() = default;
std::string VideoDecodeSession::defaultWorker() { return ffmpegWorkerExecutable(); }
WorkerReply
VideoDecodeSession::call(Role role, const ProbeResult* source, std::uint32_t stream,
                         std::uint64_t position, std::uint32_t count, const Cancel& cancel,
                         const std::shared_ptr<const platform::ReadOnlyFileSnapshot>& snapshot,
                         const bool retainWorker, const bool hardware) {
    std::unique_lock lock(state_->mutex, std::defer_lock);
    if (!acquire(lock, cancel) || (cancel && cancel()))
        return Unavailable{Error::Cancelled, "Video request cancelled"};
    if (source && stream >= source->streams.size())
        return Unavailable{Error::InvalidValue, "Video stream does not exist"};
    const auto declared = [&](const Handshake& hello) {
        return std::ranges::find_if(hello.declarations, [&](const auto& d) {
            return d.capability.role == role &&
                   (!source || role == Role::DemuxIndex ||
                    mediaCodecFamilyV1(d.capability.codec) ==
                        mediaCodecFamilyV1(source->streams[stream].codec));
        });
    };
    // Platform media framework first (ADR 0035); a failure retires it for this session and the
    // same request is answered by FFmpeg. Cancellation and a changed source end the request.
    if (!hardware && state_->nativePool && !state_->nativeRetired.load()) {
        const auto& hello = state_->nativeHello;
        if (const auto found = declared(hello); found != hello.declarations.end()) {
            auto reply = callWorker(*state_->nativePool, *found, source, stream, position, count,
                                    cancel, nullptr, false);
            const auto* error = std::get_if<Unavailable>(&reply);
            if (error == nullptr || !mediaFallbackPermittedV1(*error))
                return reply;
            state_->retireNative(error->detail);
            std::cerr << "Bloom media: " << nativeMediaProviderId() << " read failed ("
                      << error->detail << "); FFmpeg serves this source\n";
        }
    }
    auto* workers = hardware ? state_->hardwareWorkers() : &state_->pool;
    if (!workers)
        return Unavailable{Error::Unavailable, "Hardware video decode is retired for this source"};
    const auto& hello = hardware ? state_->hardwareHello : state_->hello;
    const auto found = declared(hello);
    if (found == hello.declarations.end())
        return Unavailable{Error::Unavailable, "No qualified video read capability"};
    return callWorker(*workers, *found, source, stream, position, count, cancel, snapshot,
                      retainWorker);
}
WorkerReply VideoDecodeSession::callWorker(
    MediaWorkerPool& workers, const ProviderDeclaration& declaration, const ProbeResult* source,
    const std::uint32_t stream, const std::uint64_t position, const std::uint32_t count,
    const Cancel& cancel, const std::shared_ptr<const platform::ReadOnlyFileSnapshot>& snapshot,
    const bool retainWorker) {
    const auto* found = &declaration;
    auto attempt = state_->registry.begin(providerPipeline(*found));
    if (const auto* error = std::get_if<Unavailable>(&attempt))
        return *error;
    CallRequest request;
    request.capability = found->capability;
    request.source = std::filesystem::absolute(state_->path).string();
    request.stream = stream;
    request.frame = position;
    request.samples = count;
    if (source) {
        request.sourceDigest = source->sourceDigest;
        request.width = source->streams[stream].width;
        request.height = source->streams[stream].height;
    }
    return workers.call(std::get<MediaAttempt>(attempt), 0, std::move(request), snapshot, cancel,
                        retainWorker);
}
namespace {
struct SnapshotRetirement final : render::detail::PackedHostRetirementNode {
    std::unique_ptr<platform::ReadOnlyFileSnapshot> snapshot;
    std::shared_ptr<render::detail::PackedHostRetirementState> retirement;
    bool registered = false;
    ~SnapshotRetirement() {
        snapshot.reset(); // Closing the last disk handle returns quota only after backing release.
        if (registered)
            render::detail::releasePackedHostRegistration(retirement);
    }
};
// Process-wide index of live verified snapshots keyed by content digest. It holds no ownership:
// a snapshot lives exactly as long as a session (or caller) retains it, so quota and retirement
// semantics are unchanged. Another session for the same digest (thumbnails, preview, export,
// audio) adopts the live capture instead of copying and hashing the movie again. Adoption always
// requires the adopter's own identity-verified digest; an entry never vouches for a pathname.
// Sharing is limited to one retirement domain: a rooted snapshot is only adopted by sessions of
// the same composition root, so a foreign holder can never delay that root's retirement or exit.
struct SharedSnapshot final {
    std::weak_ptr<const platform::ReadOnlyFileSnapshot> snapshot;
    std::weak_ptr<render::detail::PackedHostRetirementState> retirement;
    bool rooted = false;
};
// The bounded owning tail of the index (session.hpp, retainedSourceSnapshotCount): unrooted
// snapshots only, so no composition root's retirement is ever delayed by it.
struct RetainedSnapshot final {
    std::shared_ptr<const platform::ReadOnlyFileSnapshot> snapshot;
    std::chrono::steady_clock::time_point used;
};
struct SharedSnapshots final {
    std::mutex mutex;
    std::vector<SharedSnapshot> entries;
    std::deque<RetainedSnapshot> retained; // least recently used first
};
// Caller holds the registry mutex. Expired entries go first; then, when one snapshot of `incoming`
// bytes is about to be added, the least recently used until the count and byte bounds hold.
void trimRetained(SharedSnapshots& registry, const std::optional<std::uint64_t> incoming) {
    const auto now = std::chrono::steady_clock::now();
    std::erase_if(registry.retained, [now](const RetainedSnapshot& entry) {
        return now - entry.used > kRetainedSourceSnapshotIdle;
    });
    const auto bytes = [&registry] {
        std::uint64_t total = 0;
        for (const auto& entry : registry.retained)
            total += entry.snapshot->size();
        return total;
    };
    while (incoming && !registry.retained.empty() &&
           (registry.retained.size() >= kRetainedSourceSnapshots ||
            bytes() + *incoming > kRetainedSourceSnapshotBytes))
        registry.retained.pop_front();
}
// Caller holds the registry mutex. Marks `snapshot` used if it is retained.
void touchRetained(SharedSnapshots& registry, const platform::ReadOnlyFileSnapshot* snapshot) {
    const auto found = std::ranges::find_if(registry.retained, [snapshot](const auto& entry) {
        return entry.snapshot.get() == snapshot;
    });
    if (found == registry.retained.end())
        return;
    auto entry = std::move(*found);
    registry.retained.erase(found);
    entry.used = std::chrono::steady_clock::now();
    registry.retained.push_back(std::move(entry));
}
SharedSnapshots& sharedSnapshots() {
    static SharedSnapshots value;
    return value;
}
bool sameDomain(const SharedSnapshot& entry,
                const std::shared_ptr<render::detail::PackedHostRetirementState>& retirement) {
    if (!retirement)
        return !entry.rooted;
    return entry.rooted && !entry.retirement.owner_before(retirement) &&
           !retirement.owner_before(entry.retirement);
}
std::shared_ptr<const platform::ReadOnlyFileSnapshot>
findShared(const Digest& digest,
           const std::shared_ptr<render::detail::PackedHostRetirementState>& retirement) {
    auto& registry = sharedSnapshots();
    const std::lock_guard lock(registry.mutex);
    trimRetained(registry, std::nullopt); // idle entries expire here as well as on publication
    for (const auto& entry : registry.entries)
        if (auto snapshot = entry.snapshot.lock();
            snapshot && snapshot->digest() == digest && sameDomain(entry, retirement)) {
            if (!entry.rooted)
                touchRetained(registry, snapshot.get());
            return snapshot;
        }
    return {};
}
void publishShared(const std::shared_ptr<const platform::ReadOnlyFileSnapshot>& snapshot,
                   const std::shared_ptr<render::detail::PackedHostRetirementState>& retirement,
                   const bool retainable) {
    auto& registry = sharedSnapshots();
    const std::lock_guard lock(registry.mutex);
    std::erase_if(registry.entries, [](const auto& entry) { return entry.snapshot.expired(); });
    registry.entries.push_back({snapshot, retirement, retirement != nullptr});
    // Only an unrooted snapshot charged to its session's private quota outlives the session: a
    // caller-supplied quota is the caller's capacity and returns when its sessions end.
    if (retainable && retirement == nullptr && snapshot->size() <= kRetainedSourceSnapshotBytes) {
        trimRetained(registry, snapshot->size());
        registry.retained.push_back({snapshot, std::chrono::steady_clock::now()});
    }
}
// A capture refused for space first takes back what retention holds. True when it released any.
bool releaseRetainedForCapture() {
    auto& registry = sharedSnapshots();
    std::deque<RetainedSnapshot> released;
    {
        const std::lock_guard lock(registry.mutex);
        released.swap(registry.retained);
    }
    return !released.empty(); // destroyed here, outside the lock: closing returns the quota
}
Unavailable sourceChanged() {
    return Unavailable{Error::SourceChanged, "Video changed; relink the asset in Assets"};
}
} // namespace
std::size_t retainedSourceSnapshotCount() {
    auto& registry = sharedSnapshots();
    const std::lock_guard lock(registry.mutex);
    return registry.retained.size();
}
void releaseRetainedSourceSnapshots() { static_cast<void>(releaseRetainedForCapture()); }
std::size_t sharedSourceSnapshotCount() {
    auto& registry = sharedSnapshots();
    const std::lock_guard lock(registry.mutex);
    return static_cast<std::size_t>(std::ranges::count_if(
        registry.entries, [](const auto& entry) { return !entry.snapshot.expired(); }));
}
Result<std::shared_ptr<const platform::ReadOnlyFileSnapshot>>
VideoDecodeSession::sourceRevision(const std::optional<Digest> expected, const Cancel& cancel,
                                   const SourceRevisionAccess& access) const {
    if (access.work)
        *access.work = {};
    if (access.retained)
        *access.retained = false;
    if (cancel && cancel())
        return Unavailable{Error::Cancelled, "Video source capture cancelled"};
    const std::lock_guard lock(state_->revisionMutex);
    if (access.policy == SourceRevisionPolicy::Retained && state_->revision &&
        (!expected || state_->revision->digest() == *expected)) {
        if (access.retained)
            *access.retained = true;
        return state_->revision;
    }
    // Revalidate: one cheap identity observation of the original. While it matches the observation
    // that bracketed a full hash, the verified snapshot stands for the source; otherwise capture.
    const auto observed = platform::observeSource(state_->path);
    if (!observed.identity)
        return Unavailable{Error::Io, "Video source is unavailable; relink the asset in Assets"};
    auto verified = observed.digest;
    if (!verified && state_->revision && state_->revision->sourceIdentity() &&
        *state_->revision->sourceIdentity() == *observed.identity)
        verified = state_->revision->digest();
    if (verified) {
        if (expected && *verified != *expected)
            return sourceChanged();
        auto reusable = state_->revision && state_->revision->digest() == *verified
                            ? state_->revision
                            : findShared(*verified, state_->retirement);
        if (reusable) {
            if (access.allowRetention)
                state_->revision = reusable;
            if (access.retained)
                *access.retained = true;
            return reusable;
        }
    }
    // A prior worker keeps the old anonymous source handle alive. Retire it before a new
    // capture so a one-file quota can admit revalidation or a read-only gesture.
    state_->releaseRetainedWorkers();
    if (access.allowRetention)
        state_->revision.reset();
    if (access.hostByteBudget <= sizeof(SnapshotRetirement))
        return Unavailable{Error::Oversized, "Video source capture exceeds host working allowance"};
    auto captured = platform::ReadOnlyFileSnapshot::capture(
        state_->path, state_->spoolDirectory, state_->quota, Limits::sourceBytes, cancel,
        access.progress, {}, access.hostByteBudget - sizeof(SnapshotRetirement));
    if (const auto* refused = std::get_if<platform::ProcessFailure>(&captured);
        refused && refused->code == platform::ProcessError::ResourceLimit &&
        releaseRetainedForCapture())
        captured = platform::ReadOnlyFileSnapshot::capture(
            state_->path, state_->spoolDirectory, state_->quota, Limits::sourceBytes, cancel,
            access.progress, {}, access.hostByteBudget - sizeof(SnapshotRetirement));
    auto* snapshot = std::get_if<std::unique_ptr<platform::ReadOnlyFileSnapshot>>(&captured);
    if (!snapshot) {
        const auto& error = std::get<platform::ProcessFailure>(captured);
        return Unavailable{error.code == platform::ProcessError::Cancelled       ? Error::Cancelled
                           : error.code == platform::ProcessError::ResourceLimit ? Error::Oversized
                           : error.code == platform::ProcessError::Unavailable ? Error::Unavailable
                                                                               : Error::Io,
                           "Video source capture unavailable or exceeds its disk quota"};
    }
    if (access.work)
        *access.work = (*snapshot)->statistics();
    if (expected && (*snapshot)->digest() != *expected)
        return sourceChanged();
    auto owner = std::make_unique<SnapshotRetirement>();
    owner->snapshot = std::move(*snapshot);
    owner->retirement = state_->retirement;
    if (owner->retirement) {
        if (!render::detail::preparePackedHostRetirement(owner->retirement) ||
            !render::detail::registerPackedHostAllocation(owner->retirement))
            return Unavailable{Error::Shutdown, "Video source retirement owner is closing"};
        owner->registered = true;
    }
    owner->destroy = [](render::detail::PackedHostRetirementNode* node) noexcept {
        auto* entry = static_cast<SnapshotRetirement*>(node);
        entry->registered = false; // The retirement lane releases the queued registration.
        delete entry;
    };
    auto* entry = owner.release();
    std::shared_ptr<const platform::ReadOnlyFileSnapshot> result(
        entry->snapshot.get(), [entry](const platform::ReadOnlyFileSnapshot*) noexcept {
            if (entry->registered)
                render::detail::enqueuePackedHostRetirement(entry->retirement, entry);
            else
                delete entry;
        });
    if (access.allowRetention) {
        state_->revision = result;
        publishShared(result, state_->retirement, state_->privateQuota);
    }
    return result;
}
Result<ProbeResult> VideoDecodeSession::probe(const Cancel& cancel) {
#if defined(__linux__)
    const auto captured = sourceRevision({}, cancel, {});
    if (const auto* error = std::get_if<Unavailable>(&captured))
        return *error;
    return product<ProbeResult>(
        call(Role::Probe, nullptr, 0, 0, 1, cancel,
             std::get<std::shared_ptr<const platform::ReadOnlyFileSnapshot>>(captured), true));
#else
    return product<ProbeResult>(call(Role::Probe, nullptr, 0, 0, 1, cancel));
#endif
}
std::optional<Unavailable> VideoDecodeSession::verifySource(const Digest& expected,
                                                            const Cancel& cancel) const {
    // A known identity is one stat and never needs a snapshot or worker.
    const auto observed = platform::observeSource(state_->path);
    if (!observed.identity)
        return Unavailable{Error::Io, "Video source is unavailable; relink the asset in Assets"};
    if (observed.digest) {
        if (*observed.digest != expected)
            return sourceChanged();
        return {};
    }
#if defined(__linux__)
    // A cold source is verified by the same capture a later decode reuses.
    const auto identity = sourceRevision(expected, cancel, {SourceRevisionPolicy::Revalidate});
    if (const auto* error = std::get_if<Unavailable>(&identity))
        return *error;
    return {};
#else
    switch (platform::verifySourceDigest(state_->path, expected, cancel, Limits::sourceBytes)) {
    case platform::SourceCheck::Matches:
        return {};
    case platform::SourceCheck::Cancelled:
        return Unavailable{Error::Cancelled, "Video read cancelled"};
    case platform::SourceCheck::Missing:
        return Unavailable{Error::Io, "Video source is unavailable; relink the asset in Assets"};
    case platform::SourceCheck::Changed:
        break;
    }
    return sourceChanged();
#endif
}
Result<std::shared_ptr<const FrameProduct>>
VideoDecodeSession::frame(const ProbeResult& source, std::uint32_t stream, std::uint64_t index,
                          std::uint32_t interpretation, DecodedVideoCache* cache,
                          const Cancel& cancel) {
    return frame(source, stream, index, interpretation, cache, {}, {}, {}, cancel);
}
Result<std::shared_ptr<const FrameProduct>> VideoDecodeSession::frame(
    const ProbeResult& source, std::uint32_t stream, std::uint64_t index,
    std::uint32_t interpretation, DecodedVideoCache* cache,
    const std::string_view inputColorSpaceId, const std::string_view workingColorSpaceId,
    const provider::Digest& configRevision, const Cancel& cancel,
    const SourceRevisionAccess& revisionAccess, const FrameDecodeRoute& route) {
    if (revisionAccess.work)
        *revisionAccess.work = {};
    if (revisionAccess.retained)
        *revisionAccess.retained = false;
    std::shared_ptr<const platform::ReadOnlyFileSnapshot> snapshot;
#if defined(__linux__)
    auto captured = sourceRevision(source.sourceDigest, cancel, revisionAccess);
    if (const auto* error = std::get_if<Unavailable>(&captured))
        return *error;
    snapshot = std::get<std::shared_ptr<const platform::ReadOnlyFileSnapshot>>(std::move(captured));
#else
    if (const auto error = verifySource(source.sourceDigest, cancel))
        return *error;
#endif
    const bool retainWorker =
        revisionAccess.policy == SourceRevisionPolicy::Retained && revisionAccess.allowRetention;
    // Hardware is a capability-screened preview accelerator: only retained preview revisions of
    // a codec the hardware generation declares, and never after this session's first refusal.
    const bool hardware =
        route.preferHardware && retainWorker && stream < source.streams.size() &&
        !state_->hardwareRetired.load() && hardwareDecodePossible() &&
        std::ranges::any_of(
            state_->hardwareHello.declarations,
            [&](const auto& d) { return d.capability.codec == source.streams[stream].codec; });
    const auto describe = [&](const bool usedHardware) {
        FrameDecodeProvenance provenance;
        if (usedHardware) {
            provenance.implementation = Implementation::Hardware;
            provenance.description = "vaapi";
        } else if (route.preferHardware && state_->hardwareRetired.load()) {
            provenance.fallback = true;
            provenance.description =
                "software (fallback: " + state_->hardwareFallbackReason() + ")";
        }
        const bool native =
            !usedHardware && state_->nativePool && !state_->nativeRetired.load() &&
            stream < source.streams.size() &&
            declaresMediaOperationV1(state_->nativeHello,
                                     {MediaDirectionV1::Decode, source.streams[stream].codec, {}});
        provenance.provider = usedHardware ? std::string(kFfmpegVaapiProviderId)
                              : native     ? std::string(nativeMediaProviderId())
                                           : std::string(kFfmpegProviderId);
        if (state_->nativeRetired.load())
            provenance.nativeFallback = state_->nativeFallbackReason();
        return provenance;
    };
    const auto publish = [&](const FrameDecodeProvenance& provenance) {
        if (route.provenance)
            *route.provenance = provenance;
    };
    const FrameKey key{source.sourceDigest,
                       stream,
                       index,
                       interpretation,
                       std::string(inputColorSpaceId),
                       std::string(workingColorSpaceId),
                       configRevision,
                       hardware};
    if (cache)
        if (auto cached = cache->find(key)) {
            publish(describe(hardware));
            return cached;
        }
    std::shared_ptr<State::InFlightFrame> flight;
    bool leader = false;
    {
        // A different in-flight frame is waited for, not refused: requests are served in turn
        // by the one retained decoder, which keeps its forward position between them.
        std::unique_lock lock(state_->frameMutex);
        while (!flight) {
            if (!state_->inFlightFrame) {
                flight = std::make_shared<State::InFlightFrame>(key);
                state_->inFlightFrame = flight;
                leader = true;
            } else if (state_->inFlightFrame->key == key)
                flight = state_->inFlightFrame;
            else {
                const auto busy = state_->inFlightFrame;
                busy->completed.wait_for(lock, std::chrono::milliseconds(20));
                if (cancel && cancel())
                    return Unavailable{Error::Cancelled, "Video frame wait cancelled"};
            }
        }
    }
    if (leader && cache)
        if (auto cached = cache->find(key)) {
            // The frame that was in flight before us may have been this one.
            {
                const std::lock_guard lock(state_->frameMutex);
                flight->done = true;
                flight->result = cached;
                state_->inFlightFrame.reset();
            }
            flight->completed.notify_all();
            publish(describe(hardware));
            return cached;
        }
    if (!leader) {
        std::unique_lock lock(state_->frameMutex);
        while (!flight->done) {
            if (cancel && cancel())
                return Unavailable{Error::Cancelled, "Video frame wait cancelled"};
            flight->completed.wait_for(lock, std::chrono::milliseconds(20));
        }
        if (cancel && cancel())
            return Unavailable{Error::Cancelled, "Video frame wait cancelled"};
        auto provenance = flight->provenance;
        auto result = flight->result;
        lock.unlock();
        publish(provenance);
        return result;
    }
    // Everything after admission is covered, including worker callbacks, cache insertion and
    // copying the leader's return value. Completion never allocates or copies an outcome.
    State::FrameCompletion completion{*state_, *flight};
    try {
        bool usedHardware = hardware;
        auto decoded = product<FrameProduct>(call(Role::VideoDecode, &source, stream, index, 1,
                                                  cancel, snapshot, retainWorker, hardware));
        if (const auto* error = std::get_if<Unavailable>(&decoded);
            hardware && error && error->reason != Error::Cancelled &&
            error->reason != Error::Shutdown && error->reason != Error::SourceChanged) {
            // Explicit fallback: retire hardware for this source and decode the software
            // reference. The typed reason stays visible in every later frame's provenance.
            state_->retireHardware(error->detail);
            usedHardware = false;
            decoded = product<FrameProduct>(call(Role::VideoDecode, &source, stream, index, 1,
                                                 cancel, snapshot, retainWorker, false));
        }
        flight->provenance = describe(usedHardware);
        if (const auto* error = std::get_if<Unavailable>(&decoded)) {
            auto failure = *error;
            State::FrameOutcome outcome{std::move(failure)};
            flight->result = std::move(outcome);
        } else {
            auto frame =
                std::make_shared<const FrameProduct>(std::get<FrameProduct>(std::move(decoded)));
            if (cache && revisionAccess.allowRetention) {
                auto stored = key;
                stored.hardware = usedHardware;
                cache->store(stored, frame);
            }
            State::FrameOutcome outcome{std::move(frame)};
            flight->result = std::move(outcome);
        }
    } catch (const std::bad_alloc&) {
        std::get<Unavailable>(flight->result).reason = Error::Oversized;
    } catch (const std::exception&) {
        std::get<Unavailable>(flight->result).reason = Error::Io;
    }
    publish(flight->provenance);
    return flight->result;
}
Result<AudioBlock> VideoDecodeSession::audio(const ProbeResult& source, std::uint32_t stream,
                                             std::uint64_t sample, std::uint32_t count,
                                             const Cancel& cancel) {
#if defined(__linux__)
    const auto captured = sourceRevision(source.sourceDigest, cancel, {});
    if (const auto* error = std::get_if<Unavailable>(&captured))
        return *error;
    // A retained worker continues from the previous block's end, so a clip decodes in one
    // sequential pass instead of restarting at the stream origin for every block.
    return product<AudioBlock>(
        call(Role::AudioDecode, &source, stream, sample, count, cancel,
             std::get<std::shared_ptr<const platform::ReadOnlyFileSnapshot>>(captured), true));
#else
    return product<AudioBlock>(call(Role::AudioDecode, &source, stream, sample, count, cancel));
#endif
}
Result<DemuxIndex> VideoDecodeSession::index(const ProbeResult& source, const Cancel& cancel,
                                             const SourceRevisionAccess& access) {
#if defined(__linux__)
    const auto captured = sourceRevision(source.sourceDigest, cancel, access);
    if (const auto* error = std::get_if<Unavailable>(&captured))
        return *error;
    return product<DemuxIndex>(
        call(Role::DemuxIndex, &source, 0, 0, 1, cancel,
             std::get<std::shared_ptr<const platform::ReadOnlyFileSnapshot>>(captured),
             access.policy == SourceRevisionPolicy::Retained && access.allowRetention));
#else
    (void)access;
    return product<DemuxIndex>(call(Role::DemuxIndex, &source, 0, 0, 1, cancel));
#endif
}
} // namespace bloom::media::video
