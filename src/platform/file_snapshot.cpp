#include <algorithm>
#include <array>
#include <bloom/platform/file_snapshot.hpp>
#include <cerrno>
#include <limits>
#include <utility>
#include <vector>
#if defined(__linux__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace bloom::platform {
std::uint64_t FileSnapshotQuota::residentBytes() const {
    const std::lock_guard lock(mutex_);
    return resident_;
}
std::size_t FileSnapshotQuota::residentFiles() const {
    const std::lock_guard lock(mutex_);
    return count_;
}
bool FileSnapshotQuota::reserve(const std::uint64_t bytes) {
    const std::lock_guard lock(mutex_);
    if (count_ >= files_ || bytes > ceiling_ || resident_ > ceiling_ - bytes)
        return false;
    resident_ += bytes;
    ++count_;
    return true;
}
void FileSnapshotQuota::release(const std::uint64_t bytes) {
    const std::lock_guard lock(mutex_);
    resident_ -= bytes;
    --count_;
}
struct ReadOnlyFileSnapshot::State {
    int fd = -1;
    std::uint64_t bytes = 0;
    core::Sha256Digest digest;
    FileSnapshotStatistics statistics;
    std::optional<FileIdentity> sourceIdentity;
    std::shared_ptr<FileSnapshotQuota> quota;
    ~State() {
#if defined(__linux__) || defined(__APPLE__)
        if (fd >= 0)
            ::close(fd);
#endif
        if (quota)
            quota->release(bytes);
    }
};
ReadOnlyFileSnapshot::ReadOnlyFileSnapshot(std::unique_ptr<State> state)
    : state_(std::move(state)) {}
ReadOnlyFileSnapshot::~ReadOnlyFileSnapshot() = default;
const core::Sha256Digest& ReadOnlyFileSnapshot::digest() const { return state_->digest; }
std::uint64_t ReadOnlyFileSnapshot::size() const { return state_->bytes; }
const FileSnapshotStatistics& ReadOnlyFileSnapshot::statistics() const {
    return state_->statistics;
}
const std::optional<FileIdentity>& ReadOnlyFileSnapshot::sourceIdentity() const {
    return state_->sourceIdentity;
}
int ReadOnlyFileSnapshot::descriptor() const { return state_->fd; }
#if defined(__linux__) || defined(__APPLE__)
namespace {
class Descriptor final {
  public:
    explicit Descriptor(int value = -1) : value_(value) {}
    ~Descriptor() {
        if (value_ >= 0)
            ::close(value_);
    }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    int get() const { return value_; }
    int take() { return std::exchange(value_, -1); }

  private:
    int value_;
};
using Clock = std::chrono::steady_clock;
void elapsed(std::uint64_t& total, const Clock::time_point begin) {
    const auto value =
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin).count();
    if (value > 0) {
        const auto amount = static_cast<std::uint64_t>(value);
        total = amount > std::numeric_limits<std::uint64_t>::max() - total
                    ? std::numeric_limits<std::uint64_t>::max()
                    : total + amount;
    }
}
struct Temporary final {
    std::filesystem::path directory, file;
    ~Temporary() {
        std::error_code ignored;
        if (!file.empty())
            std::filesystem::remove(file, ignored);
        if (!directory.empty())
            std::filesystem::remove(directory, ignored);
    }
};
} // namespace
#endif
ProcessResult<std::unique_ptr<ReadOnlyFileSnapshot>> ReadOnlyFileSnapshot::capture(
    const std::filesystem::path& source, const std::filesystem::path& spoolDirectory,
    const std::shared_ptr<FileSnapshotQuota>& quota, const std::uint64_t sourceLimit,
    const ProcessCancellation& cancel, const Progress& progress,
    const std::optional<core::Sha256Digest> expected, const std::size_t hostByteBudget) {
#if defined(__linux__) || defined(__APPLE__)
    if (cancel && cancel())
        return ProcessFailure{ProcessError::Cancelled, 0};
    // Observed before the open so a replacement during the copy cannot be recorded as verified.
    const auto observedBefore = observeFileIdentity(source);
    Descriptor input(::open(source.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK));
    struct stat status{};
    if (input.get() < 0 || ::fstat(input.get(), &status) != 0 || !S_ISREG(status.st_mode))
        return ProcessFailure{ProcessError::Io, errno};
    if (status.st_size <= 0 || static_cast<std::uint64_t>(status.st_size) > sourceLimit || !quota)
        return ProcessFailure{ProcessError::ResourceLimit, 0};
    const auto size = static_cast<std::uint64_t>(status.st_size);
    constexpr std::size_t overhead = sizeof(State) + sizeof(ReadOnlyFileSnapshot) + 512;
    const auto pathBytes = spoolDirectory.native().size();
    if (pathBytes > (std::numeric_limits<std::size_t>::max() - overhead) / 6)
        return ProcessFailure{ProcessError::ResourceLimit, 0};
    const auto metadata = overhead + pathBytes * 6;
    if (hostByteBudget <= metadata)
        return ProcessFailure{ProcessError::ResourceLimit, 0};
    const auto chunkBytes = std::min<std::size_t>(65536, hostByteBudget - metadata);
    auto state = std::make_unique<State>();
    if (!quota->reserve(size))
        return ProcessFailure{ProcessError::ResourceLimit, 0};
    state->quota = quota;
    state->bytes = size;
    std::error_code error;
    if (!spoolDirectory.is_absolute())
        return ProcessFailure{ProcessError::Io, EINVAL};
    std::filesystem::create_directories(spoolDirectory, error);
    if (error)
        return ProcessFailure{ProcessError::Io, error.value()};
    auto pattern = (spoolDirectory / "capture-XXXXXX").native();
    if (::mkdtemp(pattern.data()) == nullptr)
        return ProcessFailure{ProcessError::Io, errno};
    Temporary temporary{std::filesystem::path(pattern), {}};
    temporary.file = temporary.directory / "source";
    Descriptor output(
        ::open(temporary.file.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600));
    if (output.get() < 0)
        return ProcessFailure{ProcessError::Io, errno};
    core::Sha256Hasher hasher;
    std::vector<std::byte> buffer(chunkBytes);
    std::uint64_t total = 0;
    while (total < size) {
        if (cancel && cancel())
            return ProcessFailure{ProcessError::Cancelled, 0};
        const auto count =
            static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), size - total));
        auto begin = Clock::now();
        ssize_t read = 0;
        do {
            read = ::read(input.get(), buffer.data(), count);
        } while (read < 0 && errno == EINTR);
        elapsed(state->statistics.ioNanoseconds, begin);
        if (read <= 0)
            return ProcessFailure{ProcessError::Io, errno};
        const auto amount = static_cast<std::size_t>(read);
        begin = Clock::now();
        if (!hasher.update(std::span(buffer).first(amount)))
            return ProcessFailure{ProcessError::ResourceLimit, 0};
        elapsed(state->statistics.hashNanoseconds, begin);
        begin = Clock::now();
        std::size_t written = 0;
        while (written < amount) {
            ssize_t result = 0;
            do {
                result = ::write(output.get(), buffer.data() + written, amount - written);
            } while (result < 0 && errno == EINTR);
            if (result <= 0)
                return ProcessFailure{ProcessError::Io, errno};
            written += static_cast<std::size_t>(result);
        }
        elapsed(state->statistics.ioNanoseconds, begin);
        total += amount;
        state->statistics.bytesRead = total;
        state->statistics.bytesHashed = total;
        if (progress)
            progress(total, size);
    }
    std::byte extra{};
    ssize_t read = 0;
    do {
        read = ::read(input.get(), &extra, 1);
    } while (read < 0 && errno == EINTR);
    if (read != 0)
        return ProcessFailure{ProcessError::Io, read < 0 ? errno : EFBIG};
    recordFullSourceRead(total);
    state->digest = hasher.finalize();
    if (const auto observedAfter = observeFileIdentity(source);
        observedBefore && observedAfter && *observedBefore == *observedAfter &&
        observedAfter->size == size &&
        observedAfter->fileIdLow == static_cast<std::uint64_t>(status.st_ino) &&
        observedAfter->volume == static_cast<std::uint64_t>(status.st_dev)) {
        state->sourceIdentity = observedAfter;
        verifiedSourceDigests().record(source, *observedAfter, state->digest);
    }
    if (expected && state->digest != *expected)
        return ProcessFailure{ProcessError::Io, ESTALE};
    if (cancel && cancel())
        return ProcessFailure{ProcessError::Cancelled, 0};
    if (::fchmod(output.get(), 0400) != 0)
        return ProcessFailure{ProcessError::Io, errno};
    // The private pathname is not authority: a same-size replacement must never associate the
    // writer's captured digest with different bytes reopened for the decoder.
    struct stat writerStatus{};
    if (::fstat(output.get(), &writerStatus) != 0)
        return ProcessFailure{ProcessError::Io, errno};
    if (!S_ISREG(writerStatus.st_mode) || writerStatus.st_size < 0 ||
        static_cast<std::uint64_t>(writerStatus.st_size) != size)
        return ProcessFailure{ProcessError::Io, ESTALE};
    Descriptor readonly(::open(temporary.file.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (readonly.get() < 0)
        return ProcessFailure{ProcessError::Io, errno};
    struct stat frozen{};
    if (::fstat(readonly.get(), &frozen) != 0)
        return ProcessFailure{ProcessError::Io, errno};
    if (!S_ISREG(frozen.st_mode) || frozen.st_size < 0 ||
        static_cast<std::uint64_t>(frozen.st_size) != size ||
        frozen.st_dev != writerStatus.st_dev || frozen.st_ino != writerStatus.st_ino)
        return ProcessFailure{ProcessError::Io, ESTALE};
    // Publish only after both names have disappeared and the sole write descriptor has closed.
    if (::unlink(temporary.file.c_str()) != 0 || ::rmdir(temporary.directory.c_str()) != 0)
        return ProcessFailure{ProcessError::Io, errno};
    const auto writer = output.take();
    if (::close(writer) != 0)
        return ProcessFailure{ProcessError::Io, errno};
    if (::fstat(readonly.get(), &frozen) != 0)
        return ProcessFailure{ProcessError::Io, errno};
    if (!S_ISREG(frozen.st_mode) || frozen.st_nlink != 0 || frozen.st_size < 0 ||
        static_cast<std::uint64_t>(frozen.st_size) != size ||
        frozen.st_dev != writerStatus.st_dev || frozen.st_ino != writerStatus.st_ino)
        return ProcessFailure{ProcessError::Io, ESTALE};
    // Keep source descriptors above the bootstrap/control descriptor pair for spawn file actions.
    state->fd = ::fcntl(readonly.get(), F_DUPFD_CLOEXEC, 5);
    if (state->fd < 0)
        return ProcessFailure{ProcessError::Io, errno};
    return std::unique_ptr<ReadOnlyFileSnapshot>(new ReadOnlyFileSnapshot(std::move(state)));
#else
    (void)source;
    (void)spoolDirectory;
    (void)quota;
    (void)sourceLimit;
    (void)cancel;
    (void)progress;
    (void)expected;
    (void)hostByteBudget;
    return ProcessFailure{ProcessError::Unavailable, 0};
#endif
}
ProcessResult<std::unique_ptr<WorkerSnapshotInput>>
WorkerSnapshotInput::open(const std::uint64_t size) {
#if defined(__linux__) || defined(__APPLE__)
    struct stat status{};
    const int flags = ::fcntl(4, F_GETFL);
    if (size == 0 || size > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
        flags < 0 || (flags & O_ACCMODE) != O_RDONLY || ::fstat(4, &status) != 0 ||
        !S_ISREG(status.st_mode) || status.st_nlink != 0 || status.st_size < 0 ||
        static_cast<std::uint64_t>(status.st_size) != size)
        return ProcessFailure{ProcessError::Io, EINVAL};
    // Keep descriptor 4 as the process-wide immutable authority across retained calls. Each
    // input owns a close-on-exec duplicate and uses pread, so shared file offsets are irrelevant.
    Descriptor descriptor(::fcntl(4, F_DUPFD_CLOEXEC, 5));
    if (descriptor.get() < 0)
        return ProcessFailure{ProcessError::Io, errno};
    auto input =
        std::unique_ptr<WorkerSnapshotInput>(new WorkerSnapshotInput(size, descriptor.get()));
    (void)descriptor.take();
    return input;
#else
    (void)size;
    return ProcessFailure{ProcessError::Unavailable, 0};
#endif
}
WorkerSnapshotInput::~WorkerSnapshotInput() {
#if defined(__linux__) || defined(__APPLE__)
    if (descriptor_ >= 0)
        ::close(descriptor_);
#endif
}
ProcessResult<std::size_t> WorkerSnapshotInput::read(const std::uint64_t offset,
                                                     std::span<std::byte> bytes) const {
#if defined(__linux__) || defined(__APPLE__)
    if (offset > size_)
        return ProcessFailure{ProcessError::Io, EINVAL};
    const auto count =
        static_cast<std::size_t>(std::min<std::uint64_t>(bytes.size(), size_ - offset));
    if (count == 0)
        return std::size_t{0};
    ssize_t result = 0;
    do {
        result = ::pread(descriptor_, bytes.data(), count, static_cast<off_t>(offset));
    } while (result < 0 && errno == EINTR);
    if (result < 0)
        return ProcessFailure{ProcessError::Io, errno};
    return static_cast<std::size_t>(result);
#else
    (void)offset;
    (void)bytes;
    return ProcessFailure{ProcessError::Unavailable, 0};
#endif
}
} // namespace bloom::platform
