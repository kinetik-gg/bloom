#include <algorithm>
#include <array>
#include <bloom/media/provider/ffmpeg_launch.hpp>
#include <bloom/media/provider/ffmpeg_manifest.hpp>
#if defined(__APPLE__)
#include <bloom/media/provider/videotoolbox_manifest.hpp>
#include <mach-o/dyld.h>
#endif
#include <bloom/media/video/session.hpp>
#include <bloom/platform/cache_directory.hpp>
#include <cctype>
#include <fstream>
#include <thread>
namespace bloom::media::video {
bool isVideoExtension(const std::filesystem::path& path) {
    auto extension = path.extension().string();
    std::ranges::transform(extension, extension.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension == ".mp4" || extension == ".mov" || extension == ".mkv" ||
           extension == ".mxf" || extension == ".mts" || extension == ".m2ts" ||
           extension == ".ts" || extension == ".m4v";
}

namespace {
using namespace provider;
// The video read provider is chosen by platform: the Apple-framework VideoToolbox provider on
// macOS, FFmpeg elsewhere. The host and its worker build the same handshake, so validation agrees.
Handshake providerHandshake() {
#if defined(__APPLE__)
    return videoToolboxHandshake();
#else
    return ffmpegHandshake();
#endif
}
PipelineQualificationV1 providerPipeline(const ProviderDeclaration& declaration) {
#if defined(__APPLE__)
    return videoToolboxPipeline(declaration);
#else
    return ffmpegPipeline(declaration);
#endif
}
runtime::TaskSchedulerConfig schedulerConfig() {
    runtime::TaskSchedulerConfig c;
    c.cpuWorkerCount = 1;
    c.blockingIoWorkerCount = 1;
    c.rowBandWorkerCount = runtime::kSerialRowBandWorkers;
    return c;
}
[[maybe_unused]] Result<Digest> digestFile(const std::filesystem::path& path,
                                           const Cancel& cancel) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error)
        return Unavailable{Error::Io, "Video source must be a local regular file"};
    const auto size = std::filesystem::file_size(path, error);
    if (error)
        return Unavailable{Error::Io, "Video source is unavailable"};
    if (size == 0 || size > Limits::sourceBytes)
        return Unavailable{Error::Oversized, "Video source exceeds limits"};
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return Unavailable{Error::Io, "Video source is unavailable"};
    core::Sha256Hasher hasher;
    std::array<char, 65536> bytes{};
    std::uint64_t total = 0;
    while (file.read(bytes.data(), bytes.size()) || file.gcount() > 0) {
        if (cancel && cancel())
            return Unavailable{Error::Cancelled, "Video read cancelled"};
        const auto count = static_cast<std::size_t>(file.gcount());
        total += count;
        if (total > Limits::sourceBytes ||
            !hasher.update(std::as_bytes(std::span(bytes.data(), count))))
            return Unavailable{Error::Oversized, "Video source exceeds limits"};
    }
    if (!file.eof())
        return Unavailable{Error::Io, "Video source read failed"};
    return hasher.finalize();
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
    std::filesystem::path path;
    Handshake hello = providerHandshake();
    CapabilityRegistry registry;
    runtime::TaskScheduler scheduler{schedulerConfig()};
    MediaWorkerPool pool;
    std::mutex mutex;
    std::mutex revisionMutex;
    std::shared_ptr<const platform::ReadOnlyFileSnapshot> revision;
    std::shared_ptr<platform::FileSnapshotQuota> quota;
    std::filesystem::path spoolDirectory;
    std::shared_ptr<render::detail::PackedHostRetirementState> retirement;
    State(std::filesystem::path source, std::string worker,
          std::shared_ptr<platform::FileSnapshotQuota> limits, std::filesystem::path directory,
          std::shared_ptr<render::detail::PackedHostRetirementState> owner)
        : path(std::move(source)), pool(options(std::move(worker))), quota(std::move(limits)),
          spoolDirectory(std::move(directory)), retirement(std::move(owner)) {
        if (!quota)
            quota = std::make_shared<platform::FileSnapshotQuota>(32ULL * 1024 * 1024 * 1024, 8);
        if (spoolDirectory.empty()) {
            if (const auto cache = platform::userCacheDirectory("bloom"))
                spoolDirectory = *cache / "video-source-snapshots";
        }
        for (const auto& declaration : hello.declarations) {
            (void)registry.registerProvider(declaration);
            (void)registry.qualifyPipeline(providerPipeline(declaration));
        }
    }
    WorkerPoolOptions options(std::string executable) {
        WorkerPoolOptions options;
        options.process.executable = std::move(executable);
        configureFfmpegWorkerEnvironment(options.process, options.process.executable);
        options.expected = hello;
        options.capacity = 1;
        options.timeout = std::chrono::seconds(30);
        return options;
    }
};
VideoDecodeSession::VideoDecodeSession(
    std::filesystem::path source, std::string worker,
    std::shared_ptr<platform::FileSnapshotQuota> quota, std::filesystem::path spoolDirectory,
    std::shared_ptr<render::detail::PackedHostRetirementState> retirement)
    : state_(std::make_unique<State>(std::move(source), std::move(worker), std::move(quota),
                                     std::move(spoolDirectory), std::move(retirement))) {}
VideoDecodeSession::~VideoDecodeSession() = default;
std::string VideoDecodeSession::defaultWorker() {
#if defined(__linux__)
    std::error_code error;
    const auto executable = std::filesystem::read_symlink("/proc/self/exe", error);
    if (!error) {
        const auto installed = executable.parent_path().parent_path() /
                               "libexec/bloom/media/ffmpeg-v1/bloom-media-worker";
        if (std::filesystem::is_regular_file(installed, error) && !error)
            return installed.string();
    }
#elif defined(__APPLE__)
    // Installed macOS worker: <Bloom.app>/Contents/libexec/bloom/media/videotoolbox-v1/.
    char path[4096] = {};
    std::uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size) == 0) {
        std::error_code error;
        const auto installed = std::filesystem::path(path).parent_path().parent_path() /
                               "libexec/bloom/media/videotoolbox-v1/bloom-media-worker";
        if (std::filesystem::is_regular_file(installed, error) && !error)
            return installed.string();
    }
#endif
#ifdef BLOOM_VIDEO_WORKER
    return BLOOM_VIDEO_WORKER;
#else
    return {};
#endif
}
WorkerReply
VideoDecodeSession::call(Role role, const ProbeResult* source, std::uint32_t stream,
                         std::uint64_t position, std::uint32_t count, const Cancel& cancel,
                         std::shared_ptr<const platform::ReadOnlyFileSnapshot> snapshot) {
    std::unique_lock lock(state_->mutex, std::try_to_lock);
    if (!lock.owns_lock())
        return Unavailable{Error::Busy, "Video session has an active request"};
    if (cancel && cancel())
        return Unavailable{Error::Cancelled, "Video request cancelled"};
    if (source && stream >= source->streams.size())
        return Unavailable{Error::InvalidValue, "Video stream does not exist"};
    const auto found = std::ranges::find_if(state_->hello.declarations, [&](const auto& d) {
        return d.capability.role == role && (!source || role == Role::DemuxIndex ||
                                             d.capability.codec == source->streams[stream].codec);
    });
    if (found == state_->hello.declarations.end())
        return Unavailable{Error::Unavailable, "No qualified video read capability"};
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
    auto ticket = state_->pool.submit(
        state_->scheduler,
        {runtime::TaskOwnerKind::Application, runtime::TaskOwnerId::fromRaw(914)},
        std::get<MediaAttempt>(attempt), 0, std::move(request), std::move(snapshot));
    if (!ticket.submission.accepted()) {
        if (const auto reply = ticket.result())
            return *reply;
        return Unavailable{Error::Busy, "Video worker queue is full"};
    }
    while (!ticket.submission.handle.tryTakeResult()) {
        if (cancel && cancel())
            ticket.submission.handle.cancel();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (cancel && cancel())
        return Unavailable{Error::Cancelled, "Video request cancelled"};
    if (auto reply = ticket.result())
        return *reply;
    return Unavailable{Error::Cancelled, "Video request ended without a product"};
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
} // namespace
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
    if (access.policy == SourceRevisionPolicy::Revalidate && access.allowRetention)
        state_->revision.reset();
    if (access.hostByteBudget <= sizeof(SnapshotRetirement))
        return Unavailable{Error::Oversized, "Video source capture exceeds host working allowance"};
    auto captured = platform::ReadOnlyFileSnapshot::capture(
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
        return Unavailable{Error::SourceChanged, "Video changed; relink the asset in Assets"};
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
    if (access.allowRetention)
        state_->revision = result;
    return result;
}
Result<ProbeResult> VideoDecodeSession::probe(const Cancel& cancel) {
#if defined(__linux__)
    const auto captured = sourceRevision({}, cancel, {});
    if (const auto* error = std::get_if<Unavailable>(&captured))
        return *error;
    return product<ProbeResult>(
        call(Role::Probe, nullptr, 0, 0, 1, cancel,
             std::get<std::shared_ptr<const platform::ReadOnlyFileSnapshot>>(captured)));
#else
    return product<ProbeResult>(call(Role::Probe, nullptr, 0, 0, 1, cancel));
#endif
}
std::optional<Unavailable> VideoDecodeSession::verifySource(const Digest& expected,
                                                            const Cancel& cancel) const {
#if defined(__linux__)
    const auto identity =
        sourceRevision(expected, cancel, {SourceRevisionPolicy::Revalidate, false});
    if (const auto* error = std::get_if<Unavailable>(&identity))
        return *error;
    return {};
#else
    const auto identity = digestFile(state_->path, cancel);
    if (const auto* error = std::get_if<Unavailable>(&identity))
        return *error;
    if (std::get<Digest>(identity) != expected)
        return Unavailable{Error::SourceChanged, "Video changed; relink the asset in Assets"};
    return {};
#endif
}
Result<std::shared_ptr<const FrameProduct>>
VideoDecodeSession::frame(const ProbeResult& source, std::uint32_t stream, std::uint64_t index,
                          std::uint32_t interpretation, DecodedVideoCache* cache,
                          const Cancel& cancel) {
    return frame(source, stream, index, interpretation, cache, {}, {}, {}, cancel);
}
Result<std::shared_ptr<const FrameProduct>>
VideoDecodeSession::frame(const ProbeResult& source, std::uint32_t stream, std::uint64_t index,
                          std::uint32_t interpretation, DecodedVideoCache* cache,
                          const std::string_view inputColorSpaceId,
                          const std::string_view workingColorSpaceId,
                          const provider::Digest& configRevision, const Cancel& cancel,
                          const SourceRevisionAccess& revisionAccess) {
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
    const FrameKey key{source.sourceDigest,
                       stream,
                       index,
                       interpretation,
                       std::string(inputColorSpaceId),
                       std::string(workingColorSpaceId),
                       configRevision};
    if (cache)
        if (auto cached = cache->find(key))
            return cached;
    auto decoded = product<FrameProduct>(
        call(Role::VideoDecode, &source, stream, index, 1, cancel, std::move(snapshot)));
    if (const auto* error = std::get_if<Unavailable>(&decoded))
        return *error;
    auto result = std::make_shared<const FrameProduct>(std::get<FrameProduct>(std::move(decoded)));
    if (cache && revisionAccess.allowRetention)
        cache->store(key, result);
    return result;
}
Result<AudioBlock> VideoDecodeSession::audio(const ProbeResult& source, std::uint32_t stream,
                                             std::uint64_t sample, std::uint32_t count,
                                             const Cancel& cancel) {
#if defined(__linux__)
    const auto captured = sourceRevision(source.sourceDigest, cancel, {});
    if (const auto* error = std::get_if<Unavailable>(&captured))
        return *error;
    return product<AudioBlock>(
        call(Role::AudioDecode, &source, stream, sample, count, cancel,
             std::get<std::shared_ptr<const platform::ReadOnlyFileSnapshot>>(captured)));
#else
    return product<AudioBlock>(call(Role::AudioDecode, &source, stream, sample, count, cancel));
#endif
}
Result<DemuxIndex> VideoDecodeSession::index(const ProbeResult& source, const Cancel& cancel) {
#if defined(__linux__)
    const auto captured = sourceRevision(source.sourceDigest, cancel, {});
    if (const auto* error = std::get_if<Unavailable>(&captured))
        return *error;
    return product<DemuxIndex>(
        call(Role::DemuxIndex, &source, 0, 0, 1, cancel,
             std::get<std::shared_ptr<const platform::ReadOnlyFileSnapshot>>(captured)));
#else
    return product<DemuxIndex>(call(Role::DemuxIndex, &source, 0, 0, 1, cancel));
#endif
}
} // namespace bloom::media::video
