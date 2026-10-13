#pragma once
#include <bloom/core/sha256.hpp>
#include <bloom/platform/process_supervisor.hpp>
#include <filesystem>
#include <limits>
#include <mutex>
#include <optional>

namespace bloom::platform {
struct FileSnapshotStatistics final {
    std::uint64_t bytesRead = 0, bytesHashed = 0;
    std::uint64_t ioNanoseconds = 0, hashNanoseconds = 0;
};
// Disk quota includes captures in flight and unlinked files waiting for their last handle to close.
class FileSnapshotQuota final {
  public:
    FileSnapshotQuota(std::uint64_t bytes, std::size_t files) : ceiling_(bytes), files_(files) {}
    [[nodiscard]] std::uint64_t residentBytes() const;
    [[nodiscard]] std::size_t residentFiles() const;

  private:
    friend class ReadOnlyFileSnapshot;
    bool reserve(std::uint64_t bytes);
    void release(std::uint64_t bytes);
    mutable std::mutex mutex_;
    std::uint64_t ceiling_ = 0, resident_ = 0;
    std::size_t files_ = 0, count_ = 0;
};
// A host-owned anonymous regular file. There is no public native-handle/path constructor: only
// capture() can establish the digest/length authority accepted by ProcessSupervisor. Capture and
// final release are blocking filesystem work; a UI host supplies its existing retirement owner.
class ReadOnlyFileSnapshot final {
  public:
    using Progress = std::function<void(std::uint64_t, std::uint64_t)>;
    [[nodiscard]] static ProcessResult<std::unique_ptr<ReadOnlyFileSnapshot>>
    capture(const std::filesystem::path& source, const std::filesystem::path& spoolDirectory,
            const std::shared_ptr<FileSnapshotQuota>& quota, std::uint64_t sourceLimit,
            const ProcessCancellation& cancel = {}, const Progress& progress = {},
            std::optional<core::Sha256Digest> expected = {},
            std::size_t hostByteBudget = std::numeric_limits<std::size_t>::max());
    ~ReadOnlyFileSnapshot();
    ReadOnlyFileSnapshot(const ReadOnlyFileSnapshot&) = delete;
    ReadOnlyFileSnapshot& operator=(const ReadOnlyFileSnapshot&) = delete;
    [[nodiscard]] const core::Sha256Digest& digest() const;
    [[nodiscard]] std::uint64_t size() const;
    [[nodiscard]] const FileSnapshotStatistics& statistics() const;

  private:
    friend class ProcessSupervisor;
    struct State;
    explicit ReadOnlyFileSnapshot(std::unique_ptr<State> state);
    [[nodiscard]] int descriptor() const;
    std::unique_ptr<State> state_;
};
// Child-only typed view of the descriptor installed by launch(), never a serialized pathname.
// Admission checks read-only access, anonymous regular-file identity and exact bounded length.
class WorkerSnapshotInput final {
  public:
    [[nodiscard]] static ProcessResult<std::unique_ptr<WorkerSnapshotInput>>
    open(std::uint64_t size);
    ~WorkerSnapshotInput();
    WorkerSnapshotInput(const WorkerSnapshotInput&) = delete;
    WorkerSnapshotInput& operator=(const WorkerSnapshotInput&) = delete;
    [[nodiscard]] ProcessResult<std::size_t> read(std::uint64_t offset,
                                                  std::span<std::byte> bytes) const;
    [[nodiscard]] std::uint64_t size() const { return size_; }

  private:
    explicit WorkerSnapshotInput(std::uint64_t size) : size_(size) {}
    std::uint64_t size_ = 0;
};
} // namespace bloom::platform
