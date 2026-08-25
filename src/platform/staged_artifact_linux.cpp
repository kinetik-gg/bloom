#include "staged_artifact_platform.hpp"

#include <bloom/core/sha256.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <limits>
#include <linux/fs.h>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <unordered_map>
#include <utility>

#if !defined(__linux__)
#error "staged_artifact_linux.cpp requires Linux"
#endif

namespace bloom::platform::detail {

namespace {

class FileDescriptor final {
  public:
    FileDescriptor() noexcept = default;
    explicit FileDescriptor(const int value) noexcept : value_(value) {}
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    FileDescriptor(FileDescriptor&& other) noexcept : value_(std::exchange(other.value_, -1)) {}
    FileDescriptor& operator=(FileDescriptor&& other) noexcept {
        if (this != &other) {
            reset();
            value_ = std::exchange(other.value_, -1);
        }
        return *this;
    }
    ~FileDescriptor() { reset(); }

    [[nodiscard]] int get() const noexcept { return value_; }
    [[nodiscard]] bool isValid() const noexcept { return value_ >= 0; }

    [[nodiscard]] bool closeChecked() noexcept {
        if (value_ < 0) {
            return true;
        }
        const auto value = std::exchange(value_, -1);
        return ::close(value) == 0;
    }

    void reset() noexcept {
        if (value_ >= 0) {
            static_cast<void>(::close(value_));
            value_ = -1;
        }
    }

  private:
    int value_ = -1;
};

struct NativeFileIdentity final {
    std::uint64_t device = 0;
    std::uint64_t inode = 0;

    friend bool operator==(const NativeFileIdentity&, const NativeFileIdentity&) = default;
};

struct TargetIdentity final {
    NativeFileIdentity parent;
    std::string leaf;

    friend bool operator==(const TargetIdentity&, const TargetIdentity&) = default;
};

struct TargetIdentityHash final {
    [[nodiscard]] std::size_t operator()(const TargetIdentity& value) const noexcept {
        auto result = std::hash<std::uint64_t>{}(value.parent.device);
        result ^= std::hash<std::uint64_t>{}(value.parent.inode) + 0x9E3779B97F4A7C15ULL +
                  (result << 6U) + (result >> 2U);
        result ^= std::hash<std::string>{}(value.leaf) + 0x9E3779B97F4A7C15ULL + (result << 6U) +
                  (result >> 2U);
        return result;
    }
};

struct TargetRecord final {
    explicit TargetRecord(const core::ArtifactTargetKey value) noexcept : key(value) {}

    core::ArtifactTargetKey key;
    std::mutex publicationMutex;
};

struct LinuxSharedState final {
    explicit LinuxSharedState(const StagedArtifactConfig& value) noexcept : config(value) {}

    [[nodiscard]] bool shouldFail(const StagedArtifactFaultPoint point) noexcept {
        if (config.faults.point != point) {
            return false;
        }
        std::lock_guard lock(mutex);
        ++matchingFaultVisits;
        return matchingFaultVisits == config.faults.occurrence;
    }

    void releaseActiveTarget() noexcept {
        std::lock_guard lock(mutex);
        if (activeTargetCount == 0) {
            std::terminate();
        }
        --activeTargetCount;
    }

    void recordCleanupFailure() noexcept {
        std::lock_guard lock(mutex);
        ++cleanupFailureCount;
    }

    StagedArtifactConfig config;
    mutable std::mutex mutex;
    std::unordered_map<TargetIdentity, std::shared_ptr<TargetRecord>, TargetIdentityHash> records;
    std::size_t activeTargetCount = 0;
    std::uint64_t lastIssuedTargetKey = 0;
    std::uint64_t cleanupFailureCount = 0;
    std::uint64_t matchingFaultVisits = 0;
    bool targetIdentityExhausted = false;
};

class AdmissionToken final {
  public:
    AdmissionToken() noexcept = default;
    explicit AdmissionToken(std::shared_ptr<LinuxSharedState> state) noexcept
        : state_(std::move(state)) {}
    AdmissionToken(const AdmissionToken&) = delete;
    AdmissionToken& operator=(const AdmissionToken&) = delete;
    AdmissionToken(AdmissionToken&& other) noexcept : state_(std::move(other.state_)) {}
    AdmissionToken& operator=(AdmissionToken&& other) noexcept {
        if (this != &other) {
            release();
            state_ = std::move(other.state_);
        }
        return *this;
    }
    ~AdmissionToken() { release(); }

  private:
    void release() noexcept {
        if (state_ != nullptr) {
            state_->releaseActiveTarget();
            state_.reset();
        }
    }

    std::shared_ptr<LinuxSharedState> state_;
};

struct TargetData final {
    std::shared_ptr<LinuxSharedState> shared;
    AdmissionToken admission;
    std::shared_ptr<TargetRecord> record;
    FileDescriptor parentDescriptor;
    NativeFileIdentity parentIdentity;
    std::filesystem::path canonicalParentPath;
    std::string leaf;
    ArtifactTargetObservation expectedTarget;
    ArtifactOverwritePolicy overwritePolicy = ArtifactOverwritePolicy::CreateOrReplace;
};

struct InspectionResult final {
    explicit InspectionResult(const StagedArtifactError value) noexcept : error(value) {}
    explicit InspectionResult(const ArtifactTargetObservation value) noexcept
        : observation(value) {}

    StagedArtifactError error = StagedArtifactError::None;
    ArtifactTargetObservation observation;
};

[[nodiscard]] NativeFileIdentity identityFromStat(const struct stat& status) noexcept {
    return {.device = static_cast<std::uint64_t>(status.st_dev),
            .inode = static_cast<std::uint64_t>(status.st_ino)};
}

[[nodiscard]] bool isRegular(const struct stat& status) noexcept {
    return S_ISREG(status.st_mode) != 0;
}

[[nodiscard]] bool isDirectory(const struct stat& status) noexcept {
    return S_ISDIR(status.st_mode) != 0;
}

[[nodiscard]] ArtifactTargetFingerprint fingerprintFrom(const struct stat& status,
                                                        const std::uint64_t byteSize,
                                                        const core::Sha256Digest digest) noexcept {
    return {.identity = {.first = static_cast<std::uint64_t>(status.st_dev),
                         .second = static_cast<std::uint64_t>(status.st_ino)},
            .byteSize = byteSize,
            .modificationSeconds = static_cast<std::int64_t>(status.st_mtim.tv_sec),
            .modificationNanoseconds = static_cast<std::uint32_t>(status.st_mtim.tv_nsec),
            .digest = digest};
}

[[nodiscard]] bool sameFingerprintEvidence(const struct stat& before,
                                           const struct stat& after) noexcept {
    return identityFromStat(before) == identityFromStat(after) && before.st_size == after.st_size &&
           before.st_mtim.tv_sec == after.st_mtim.tv_sec &&
           before.st_mtim.tv_nsec == after.st_mtim.tv_nsec && isRegular(after);
}

[[nodiscard]] InspectionResult inspectTarget(const int parentDescriptor, const std::string& leaf,
                                             const std::uint64_t byteLimit) noexcept {
    struct stat pathStatus{};
    if (::fstatat(parentDescriptor, leaf.data(), &pathStatus, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT) {
            return InspectionResult(ArtifactTargetObservation::absent());
        }
        return InspectionResult(StagedArtifactError::TargetInspectionFailed);
    }
    if (S_ISLNK(pathStatus.st_mode) != 0) {
        return InspectionResult(StagedArtifactError::TargetLeafSymlink);
    }
    if (!isRegular(pathStatus)) {
        return InspectionResult(StagedArtifactError::TargetLeafNotRegular);
    }
    if (pathStatus.st_size < 0 || static_cast<std::uint64_t>(pathStatus.st_size) > byteLimit) {
        return InspectionResult(StagedArtifactError::ArtifactSizeLimit);
    }

    FileDescriptor descriptor(
        ::openat(parentDescriptor, leaf.data(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (!descriptor.isValid()) {
        return InspectionResult(StagedArtifactError::TargetInspectionFailed);
    }
    struct stat openedStatus{};
    if (::fstat(descriptor.get(), &openedStatus) != 0 || !isRegular(openedStatus) ||
        identityFromStat(openedStatus) != identityFromStat(pathStatus)) {
        return InspectionResult(StagedArtifactError::TargetInspectionFailed);
    }

    core::Sha256Hasher hasher;
    constexpr std::size_t readBufferByteCount = 65'536;
    std::array<std::byte, readBufferByteCount> buffer{};
    std::uint64_t bytesRead = 0;
    while (true) {
        const auto count = ::read(descriptor.get(), buffer.data(), buffer.size());
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return InspectionResult(StagedArtifactError::TargetInspectionFailed);
        }
        if (count == 0) {
            break;
        }
        const auto unsignedCount = static_cast<std::uint64_t>(count);
        if (unsignedCount > byteLimit - bytesRead ||
            !hasher.update(std::span(buffer).first(static_cast<std::size_t>(count)))) {
            return InspectionResult(StagedArtifactError::ArtifactSizeLimit);
        }
        bytesRead += unsignedCount;
    }

    struct stat finalStatus{};
    if (::fstat(descriptor.get(), &finalStatus) != 0 ||
        !sameFingerprintEvidence(openedStatus, finalStatus) || finalStatus.st_size < 0 ||
        bytesRead != static_cast<std::uint64_t>(finalStatus.st_size)) {
        return InspectionResult(StagedArtifactError::TargetInspectionFailed);
    }
    return InspectionResult(ArtifactTargetObservation::existing(
        fingerprintFrom(finalStatus, bytesRead, hasher.finalize())));
}

[[nodiscard]] bool targetPolicyAccepts(const ArtifactOverwritePolicy policy,
                                       const ArtifactTargetObservation& observation) noexcept {
    switch (policy) {
    case ArtifactOverwritePolicy::CreateOnly:
        return !observation.exists;
    case ArtifactOverwritePolicy::ReplaceExisting:
        return observation.exists;
    case ArtifactOverwritePolicy::CreateOrReplace:
        return true;
    }
    return false;
}

[[nodiscard]] bool sameNativeIdentity(const struct stat& status,
                                      const NativeFileIdentity identity) noexcept {
    return identityFromStat(status) == identity;
}

[[nodiscard]] bool revalidateParent(const TargetData& target) noexcept {
    struct stat pinnedStatus{};
    if (::fstat(target.parentDescriptor.get(), &pinnedStatus) != 0 || !isDirectory(pinnedStatus) ||
        !sameNativeIdentity(pinnedStatus, target.parentIdentity)) {
        return false;
    }

    FileDescriptor current(::open(target.canonicalParentPath.c_str(),
                                  O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    struct stat currentStatus{};
    return current.isValid() && ::fstat(current.get(), &currentStatus) == 0 &&
           isDirectory(currentStatus) && sameNativeIdentity(currentStatus, target.parentIdentity);
}

[[nodiscard]] std::optional<std::string> makeStageName() {
    constexpr std::size_t nonceByteCount = 16;
    std::array<unsigned char, nonceByteCount> nonce{};
    std::size_t offset = 0;
    while (offset < nonce.size()) {
        const auto count = ::getrandom(nonce.data() + offset, nonce.size() - offset, 0);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return std::nullopt;
        }
        if (count == 0) {
            return std::nullopt;
        }
        offset += static_cast<std::size_t>(count);
    }

    constexpr std::string_view prefix = ".bloom-stage-";
    constexpr std::array<char, 16> hexadecimal = {'0', '1', '2', '3', '4', '5', '6', '7',
                                                  '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    std::string result;
    result.reserve(prefix.size() + (nonce.size() * 2));
    result.append(prefix);
    for (const auto byte : nonce) {
        result.push_back(hexadecimal[byte >> 4U]);
        result.push_back(hexadecimal[byte & 0x0FU]);
    }
    return result;
}

class LinuxTarget final : public StagedArtifactTargetState {
  public:
    explicit LinuxTarget(TargetData data) noexcept : data_(std::move(data)) {}
    ~LinuxTarget() override = default;

    [[nodiscard]] core::ArtifactTargetKey targetKey() const noexcept override {
        return data_.record->key;
    }
    [[nodiscard]] ArtifactTargetObservation observation() const noexcept override {
        return data_.expectedTarget;
    }

    [[nodiscard]] TargetData takeData() noexcept { return std::move(data_); }

    [[nodiscard]] const TargetData& dataForValidation() const noexcept { return data_; }

  private:
    TargetData data_;
};

class LinuxLease final : public StagedArtifactLeaseState {
  public:
    LinuxLease(TargetData target, FileDescriptor descriptor, std::string stageName,
               const NativeFileIdentity stageIdentity) noexcept
        : target_(std::move(target)), descriptor_(std::move(descriptor)),
          stageName_(std::move(stageName)), stageIdentity_(stageIdentity) {}

    ~LinuxLease() override { cleanupStage(); }

    [[nodiscard]] core::ArtifactTargetKey targetKey() const noexcept override {
        return target_.record->key;
    }
    [[nodiscard]] std::uint64_t stageBytes() const noexcept override { return stageBytes_; }

    [[nodiscard]] StagedArtifactOperationResult
    write(const std::span<const std::byte> bytes) noexcept override {
        if (phase_ == LeasePhase::Terminal) {
            return terminalOperationResult(StagedArtifactError::StageNotWritable);
        }
        if (phase_ != LeasePhase::Writable) {
            return {.error = StagedArtifactError::StageNotWritable, .stageBytes = stageBytes_};
        }
        if (bytes.size() > target_.shared->config.artifactByteLimit - stageBytes_) {
            return failOperation(StagedArtifactError::ArtifactSizeLimit);
        }
        if (target_.shared->shouldFail(StagedArtifactFaultPoint::StageWrite)) {
            return failOperation(StagedArtifactError::FaultInjected);
        }

        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const auto count =
                ::write(descriptor_.get(), bytes.data() + offset, bytes.size() - offset);
            if (count < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return failOperation(StagedArtifactError::StageWriteFailed);
            }
            if (count == 0) {
                return failOperation(StagedArtifactError::StageWriteFailed);
            }
            offset += static_cast<std::size_t>(count);
            stageBytes_ += static_cast<std::uint64_t>(count);
        }
        return {.stageBytes = stageBytes_};
    }

    [[nodiscard]] StagedArtifactOperationResult finishWriting() noexcept override {
        if (phase_ == LeasePhase::Terminal) {
            return terminalOperationResult(StagedArtifactError::StageNotWritable);
        }
        if (phase_ != LeasePhase::Writable) {
            return {.error = StagedArtifactError::StageNotWritable, .stageBytes = stageBytes_};
        }

        const bool injectedCloseFailure =
            target_.shared->shouldFail(StagedArtifactFaultPoint::StageWriterClose);
        const bool closeSucceeded = descriptor_.closeChecked();
        if (injectedCloseFailure) {
            return failOperation(StagedArtifactError::FaultInjected);
        }
        if (!closeSucceeded) {
            return failOperation(StagedArtifactError::StageWriterCloseFailed);
        }
        if (target_.shared->shouldFail(StagedArtifactFaultPoint::StageReopen)) {
            return failOperation(StagedArtifactError::FaultInjected);
        }

        FileDescriptor reopened(::openat(target_.parentDescriptor.get(), stageName_.c_str(),
                                         O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW));
        if (!reopened.isValid()) {
            const bool identityEvidence = errno == ENOENT || errno == ELOOP || errno == ENOTDIR;
            return failOperation(identityEvidence ? StagedArtifactError::StageIdentityMismatch
                                                  : StagedArtifactError::StageReopenFailed);
        }
        descriptor_ = std::move(reopened);
        if (!revalidateStage()) {
            return failOperation(StagedArtifactError::StageIdentityMismatch);
        }
        phase_ = LeasePhase::Verifying;
        return {.stageBytes = stageBytes_};
    }

    [[nodiscard]] StagedArtifactVerificationReadResult
    readForVerification(const std::uint64_t offset,
                        const std::span<std::byte> destination) noexcept override {
        if (phase_ == LeasePhase::Terminal) {
            return terminalReadResult(StagedArtifactError::StageNotVerifying);
        }
        if (phase_ != LeasePhase::Verifying) {
            return {.error = StagedArtifactError::StageNotVerifying, .stageBytes = stageBytes_};
        }
        if (target_.shared->shouldFail(StagedArtifactFaultPoint::StageVerificationRead)) {
            return failRead(StagedArtifactError::FaultInjected);
        }
        if (offset >= stageBytes_) {
            return {.stageBytes = stageBytes_, .endOfFile = true};
        }

        constexpr auto maximumOffset =
            static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
        if (offset > maximumOffset) {
            return failRead(StagedArtifactError::StageVerificationReadOffsetOutOfRange);
        }

        const auto available = stageBytes_ - offset;
        const std::uint64_t requested = destination.size();
        const auto desired = std::min(available, requested);
        std::uint64_t total = 0;
        std::size_t destinationOffset = 0;
        while (total < desired) {
            const auto remaining = desired - total;
            const auto chunk = static_cast<std::size_t>(std::min<std::uint64_t>(
                remaining, static_cast<std::uint64_t>(std::numeric_limits<ssize_t>::max())));
            const auto nativeOffset = offset + total;
            if (nativeOffset > maximumOffset) {
                return failRead(StagedArtifactError::StageVerificationReadOffsetOutOfRange, total);
            }
            const auto count = ::pread(descriptor_.get(), destination.data() + destinationOffset,
                                       chunk, static_cast<off_t>(nativeOffset));
            if (count < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return failRead(StagedArtifactError::StageVerificationReadFailed, total);
            }
            if (count == 0) {
                return failRead(StagedArtifactError::StageVerificationReadFailed, total);
            }
            total += static_cast<std::uint64_t>(count);
            destinationOffset += static_cast<std::size_t>(count);
        }
        return {.bytesRead = total,
                .stageBytes = stageBytes_,
                .endOfFile = offset + total >= stageBytes_};
    }

    [[nodiscard]] StagedArtifactOperationResult acceptVerification() noexcept override {
        if (phase_ == LeasePhase::Terminal) {
            return terminalOperationResult(StagedArtifactError::StageNotVerifying);
        }
        if (phase_ == LeasePhase::Accepted) {
            return {.stageBytes = stageBytes_};
        }
        if (phase_ != LeasePhase::Verifying) {
            return {.error = StagedArtifactError::StageNotVerifying, .stageBytes = stageBytes_};
        }
        if (target_.shared->shouldFail(StagedArtifactFaultPoint::StageVerificationAccept)) {
            return failOperation(StagedArtifactError::FaultInjected);
        }
        if (!revalidateStage()) {
            return failOperation(StagedArtifactError::StageIdentityMismatch);
        }
        if (target_.shared->shouldFail(StagedArtifactFaultPoint::StageFlush)) {
            return failOperation(StagedArtifactError::FaultInjected);
        }
        if (::fsync(descriptor_.get()) != 0) {
            return failOperation(StagedArtifactError::StageFlushFailed);
        }
        if (!revalidateStage()) {
            return failOperation(StagedArtifactError::StageIdentityMismatch);
        }
        phase_ = LeasePhase::Accepted;
        return {.stageBytes = stageBytes_};
    }

    [[nodiscard]] StagedArtifactOperationResult rejectVerification() noexcept override {
        if (phase_ == LeasePhase::Terminal) {
            if (terminalOperationError_ == StagedArtifactError::StageVerificationRejected) {
                return {.stageBytes = stageBytes_};
            }
            return terminalOperationResult(StagedArtifactError::StageNotVerifying);
        }
        if (phase_ != LeasePhase::Verifying) {
            return {.error = StagedArtifactError::StageNotVerifying, .stageBytes = stageBytes_};
        }
        terminalOperationError_ = StagedArtifactError::StageVerificationRejected;
        phase_ = LeasePhase::Terminal;
        publicationResult_ = {
            .outcome = StagedArtifactPublicationOutcome::FailedBeforePublication,
            .error = StagedArtifactError::StageVerificationRejected,
        };
        cleanupStage();
        return {.stageBytes = stageBytes_};
    }

    [[nodiscard]] StagedArtifactPublicationResult
    publish(const PublicationDisposition disposition) noexcept override {
        if (publicationResult_.has_value()) {
            return *publicationResult_;
        }
        if (phase_ == LeasePhase::Terminal) {
            return {.outcome = StagedArtifactPublicationOutcome::FailedBeforePublication,
                    .error = terminalOperationError_ != StagedArtifactError::None
                                 ? terminalOperationError_
                                 : StagedArtifactError::StageVerificationNotAccepted};
        }
        if (disposition != PublicationDisposition::Proceed &&
            disposition != PublicationDisposition::Superseded &&
            disposition != PublicationDisposition::Cancelled) {
            return {.outcome = StagedArtifactPublicationOutcome::FailedBeforePublication,
                    .error = StagedArtifactError::InvalidPublicationDisposition};
        }
        if (disposition == PublicationDisposition::Superseded) {
            cleanupStage();
            return finishPublication({.outcome = StagedArtifactPublicationOutcome::Superseded});
        }
        if (disposition == PublicationDisposition::Cancelled) {
            cleanupStage();
            return finishPublication(
                {.outcome = StagedArtifactPublicationOutcome::CancelledBeforePublication});
        }
        if (phase_ != LeasePhase::Accepted) {
            return {.outcome = StagedArtifactPublicationOutcome::FailedBeforePublication,
                    .error = StagedArtifactError::StageVerificationNotAccepted};
        }

        std::lock_guard publicationLock(target_.record->publicationMutex);
        if (target_.shared->shouldFail(StagedArtifactFaultPoint::IdentityRevalidation)) {
            return finishPublication(
                {.outcome = StagedArtifactPublicationOutcome::FailedBeforePublication,
                 .error = StagedArtifactError::FaultInjected});
        }
        if (!revalidateParent(target_)) {
            return finishPublication(
                {.outcome = StagedArtifactPublicationOutcome::FailedBeforePublication,
                 .error = StagedArtifactError::ParentIdentityMismatch});
        }
        if (!revalidateStage()) {
            return finishPublication(
                {.outcome = StagedArtifactPublicationOutcome::FailedBeforePublication,
                 .error = StagedArtifactError::StageIdentityMismatch});
        }

        const auto current = inspectTarget(target_.parentDescriptor.get(), target_.leaf,
                                           target_.shared->config.artifactByteLimit);
        if (current.error != StagedArtifactError::None) {
            const bool isConflictEvidence =
                current.error == StagedArtifactError::TargetLeafSymlink ||
                current.error == StagedArtifactError::TargetLeafNotRegular ||
                current.error == StagedArtifactError::ArtifactSizeLimit;
            return finishPublication(
                {.outcome = isConflictEvidence
                                ? StagedArtifactPublicationOutcome::ExternalModificationConflict
                                : StagedArtifactPublicationOutcome::FailedBeforePublication,
                 .error = isConflictEvidence ? StagedArtifactError::ExternalModificationConflict
                                             : current.error});
        }
        if (current.observation != target_.expectedTarget) {
            return finishPublication(
                {.outcome = StagedArtifactPublicationOutcome::ExternalModificationConflict,
                 .error = StagedArtifactError::ExternalModificationConflict});
        }
        if (target_.shared->shouldFail(StagedArtifactFaultPoint::AtomicPublication)) {
            return finishPublication(
                {.outcome = StagedArtifactPublicationOutcome::FailedBeforePublication,
                 .error = StagedArtifactError::FaultInjected});
        }

        const auto renameResult = publishAtomically();
        if (renameResult != StagedArtifactError::None) {
            const auto outcome =
                renameResult == StagedArtifactError::ExternalModificationConflict
                    ? StagedArtifactPublicationOutcome::ExternalModificationConflict
                    : StagedArtifactPublicationOutcome::FailedBeforePublication;
            return finishPublication({.outcome = outcome, .error = renameResult});
        }

        ownsStageName_ = false;
        const bool injectedDurabilityFailure =
            target_.shared->shouldFail(StagedArtifactFaultPoint::ParentDurability);
        if (injectedDurabilityFailure || ::fsync(target_.parentDescriptor.get()) != 0) {
            return finishPublication(
                {.outcome = StagedArtifactPublicationOutcome::PublishedWithDurabilityWarning,
                 .error = injectedDurabilityFailure ? StagedArtifactError::FaultInjected
                                                    : StagedArtifactError::ParentDurabilityFailed});
        }
        return finishPublication({.outcome = StagedArtifactPublicationOutcome::Published});
    }

  private:
    enum class LeasePhase : std::uint8_t {
        Writable,
        Verifying,
        Accepted,
        Terminal,
    };

    [[nodiscard]] StagedArtifactOperationResult
    failOperation(const StagedArtifactError error) noexcept {
        phase_ = LeasePhase::Terminal;
        terminalOperationError_ = error;
        publicationResult_ = {
            .outcome = StagedArtifactPublicationOutcome::FailedBeforePublication,
            .error = error,
        };
        cleanupStage();
        return {.error = error, .stageBytes = stageBytes_};
    }

    [[nodiscard]] StagedArtifactOperationResult
    terminalOperationResult(const StagedArtifactError fallback) const noexcept {
        return {.error = terminalOperationError_ != StagedArtifactError::None
                             ? terminalOperationError_
                             : fallback,
                .stageBytes = stageBytes_};
    }

    [[nodiscard]] StagedArtifactVerificationReadResult
    failRead(const StagedArtifactError error, const std::uint64_t bytesRead = 0) noexcept {
        const auto failed = failOperation(error);
        return {.error = failed.error, .bytesRead = bytesRead, .stageBytes = failed.stageBytes};
    }

    [[nodiscard]] StagedArtifactVerificationReadResult
    terminalReadResult(const StagedArtifactError fallback) const noexcept {
        return {.error = terminalOperationError_ != StagedArtifactError::None
                             ? terminalOperationError_
                             : fallback,
                .stageBytes = stageBytes_};
    }

    [[nodiscard]] StagedArtifactPublicationResult
    finishPublication(const StagedArtifactPublicationResult result) noexcept {
        phase_ = LeasePhase::Terminal;
        publicationResult_ = result;
        if (!result.targetWasPublished()) {
            cleanupStage();
        } else {
            descriptor_.reset();
        }
        return result;
    }

    [[nodiscard]] bool revalidateStage() const noexcept {
        struct stat descriptorStatus{};
        struct stat pathStatus{};
        return ::fstat(descriptor_.get(), &descriptorStatus) == 0 && isRegular(descriptorStatus) &&
               descriptorStatus.st_nlink == 1 && descriptorStatus.st_size >= 0 &&
               static_cast<std::uint64_t>(descriptorStatus.st_size) == stageBytes_ &&
               sameNativeIdentity(descriptorStatus, stageIdentity_) &&
               ::fstatat(target_.parentDescriptor.get(), stageName_.c_str(), &pathStatus,
                         AT_SYMLINK_NOFOLLOW) == 0 &&
               isRegular(pathStatus) && pathStatus.st_nlink == 1 && pathStatus.st_size >= 0 &&
               static_cast<std::uint64_t>(pathStatus.st_size) == stageBytes_ &&
               sameNativeIdentity(pathStatus, stageIdentity_);
    }

    [[nodiscard]] StagedArtifactError publishAtomically() noexcept {
        if (!target_.expectedTarget.exists) {
#if defined(SYS_renameat2)
            const auto result =
                ::syscall(SYS_renameat2, target_.parentDescriptor.get(), stageName_.c_str(),
                          target_.parentDescriptor.get(), target_.leaf.c_str(), RENAME_NOREPLACE);
            if (result == 0) {
                return StagedArtifactError::None;
            }
            if (errno == EEXIST) {
                return StagedArtifactError::ExternalModificationConflict;
            }
            if (errno == ENOSYS || errno == EINVAL) {
                return StagedArtifactError::AtomicCreateUnsupported;
            }
            return StagedArtifactError::AtomicPublicationFailed;
#else
            return StagedArtifactError::AtomicCreateUnsupported;
#endif
        }
        if (::renameat(target_.parentDescriptor.get(), stageName_.c_str(),
                       target_.parentDescriptor.get(), target_.leaf.c_str()) != 0) {
            return StagedArtifactError::AtomicPublicationFailed;
        }
        return StagedArtifactError::None;
    }

    void cleanupStage() noexcept {
        descriptor_.reset();
        if (!ownsStageName_) {
            return;
        }
        struct stat status{};
        if (::fstatat(target_.parentDescriptor.get(), stageName_.c_str(), &status,
                      AT_SYMLINK_NOFOLLOW) == 0) {
            const bool identityMatches =
                isRegular(status) && sameNativeIdentity(status, stageIdentity_);
            if (!identityMatches) {
                target_.shared->recordCleanupFailure();
            } else {
                const bool unexpectedLinks = status.st_nlink != 1;
                if (::unlinkat(target_.parentDescriptor.get(), stageName_.c_str(), 0) != 0 ||
                    unexpectedLinks) {
                    target_.shared->recordCleanupFailure();
                }
            }
        } else if (errno != ENOENT) {
            target_.shared->recordCleanupFailure();
        }
        ownsStageName_ = false;
    }

    TargetData target_;
    FileDescriptor descriptor_;
    std::string stageName_;
    NativeFileIdentity stageIdentity_;
    std::uint64_t stageBytes_ = 0;
    LeasePhase phase_ = LeasePhase::Writable;
    bool ownsStageName_ = true;
    StagedArtifactError terminalOperationError_ = StagedArtifactError::None;
    std::optional<StagedArtifactPublicationResult> publicationResult_;
};

class LinuxCoordinator final : public StagedArtifactCoordinatorState {
  public:
    explicit LinuxCoordinator(std::shared_ptr<LinuxSharedState> state) noexcept
        : state_(std::move(state)) {}

    [[nodiscard]] PlatformPreflightResult
    preflight(const StagedArtifactPreflightRequest& request) noexcept override {
        try {
            return preflightChecked(request);
        } catch (const std::bad_alloc&) {
            return PlatformPreflightResult(StagedArtifactError::ResourceUnavailable);
        } catch (...) {
            return PlatformPreflightResult(StagedArtifactError::ParentResolutionFailed);
        }
    }

    [[nodiscard]] PlatformStageResult
    stage(std::unique_ptr<StagedArtifactTargetState> target) noexcept override {
        auto* linuxTarget = dynamic_cast<LinuxTarget*>(target.get());
        if (linuxTarget == nullptr) {
            return PlatformStageResult(StagedArtifactError::InvalidTargetPath);
        }
        if (linuxTarget->dataForValidation().shared != state_) {
            return PlatformStageResult(StagedArtifactError::CoordinatorMismatch);
        }
        if (state_->shouldFail(StagedArtifactFaultPoint::StageCreation)) {
            return PlatformStageResult(StagedArtifactError::FaultInjected);
        }
        if (!revalidateParent(linuxTarget->dataForValidation())) {
            return PlatformStageResult(StagedArtifactError::ParentIdentityMismatch);
        }

        try {
            auto data = linuxTarget->takeData();
            target.reset();
            constexpr std::size_t maximumAttempts = 32;
            for (std::size_t attempt = 0; attempt < maximumAttempts; ++attempt) {
                auto stageName = makeStageName();
                if (!stageName.has_value()) {
                    return PlatformStageResult(StagedArtifactError::StageCreationFailed);
                }
                FileDescriptor descriptor(::openat(
                    data.parentDescriptor.get(), stageName->c_str(),
                    O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, S_IRUSR | S_IWUSR));
                if (!descriptor.isValid()) {
                    if (errno == EEXIST) {
                        continue;
                    }
                    return PlatformStageResult(StagedArtifactError::StageCreationFailed);
                }
                struct stat status{};
                if (::fstat(descriptor.get(), &status) != 0 || !isRegular(status) ||
                    status.st_nlink != 1) {
                    ::unlinkat(data.parentDescriptor.get(), stageName->c_str(), 0);
                    return PlatformStageResult(StagedArtifactError::StageCreationFailed);
                }
                auto lease =
                    std::make_unique<LinuxLease>(std::move(data), std::move(descriptor),
                                                 std::move(*stageName), identityFromStat(status));
                return PlatformStageResult(std::move(lease));
            }
            return PlatformStageResult(StagedArtifactError::StageCreationFailed);
        } catch (const std::bad_alloc&) {
            return PlatformStageResult(StagedArtifactError::ResourceUnavailable);
        } catch (...) {
            return PlatformStageResult(StagedArtifactError::StageCreationFailed);
        }
    }

    [[nodiscard]] StagedArtifactCoordinatorSnapshot snapshot() const noexcept override {
        std::lock_guard lock(state_->mutex);
        return {.activeTargetCount = state_->activeTargetCount,
                .activeTargetLimit = state_->config.activeTargetLimit,
                .targetRecordCount = state_->records.size(),
                .targetRecordLimit = state_->config.targetRecordLimit,
                .cleanupFailureCount = state_->cleanupFailureCount,
                .lastIssuedTargetKey =
                    core::ArtifactTargetKey::fromRaw(state_->lastIssuedTargetKey),
                .targetIdentityExhausted = state_->targetIdentityExhausted};
    }

  private:
    [[nodiscard]] PlatformPreflightResult
    preflightChecked(const StagedArtifactPreflightRequest& request) {
        const auto leafPath = request.targetPath.filename();
        auto leaf = leafPath.native();
        if (leaf.empty() || leaf == "." || leaf == ".." || leaf.find('\0') != std::string::npos) {
            return PlatformPreflightResult(StagedArtifactError::InvalidTargetPath);
        }
        auto parentPath = request.targetPath.parent_path();
        if (parentPath.empty()) {
            parentPath = ".";
        }
        std::error_code pathError;
        auto canonicalParent = std::filesystem::canonical(parentPath, pathError);
        if (pathError) {
            return PlatformPreflightResult(StagedArtifactError::ParentResolutionFailed);
        }

        FileDescriptor parentDescriptor(
            ::open(canonicalParent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
        if (!parentDescriptor.isValid()) {
            return PlatformPreflightResult(errno == ENOTDIR
                                               ? StagedArtifactError::ParentNotDirectory
                                               : StagedArtifactError::ParentResolutionFailed);
        }
        struct stat parentStatus{};
        if (::fstat(parentDescriptor.get(), &parentStatus) != 0 || !isDirectory(parentStatus)) {
            return PlatformPreflightResult(StagedArtifactError::ParentNotDirectory);
        }

        if (state_->shouldFail(StagedArtifactFaultPoint::TargetInspection)) {
            return PlatformPreflightResult(StagedArtifactError::FaultInjected);
        }
        const auto inspection =
            inspectTarget(parentDescriptor.get(), leaf, state_->config.artifactByteLimit);
        if (inspection.error != StagedArtifactError::None) {
            return PlatformPreflightResult(inspection.error);
        }
        if (request.expectedTarget.has_value() &&
            *request.expectedTarget != inspection.observation) {
            return PlatformPreflightResult(StagedArtifactError::ExternalModificationConflict);
        }
        if (!targetPolicyAccepts(request.overwritePolicy, inspection.observation)) {
            return PlatformPreflightResult(StagedArtifactError::OverwritePolicyConflict);
        }

        const TargetIdentity identity{identityFromStat(parentStatus), leaf};
        std::shared_ptr<TargetRecord> record;
        {
            std::lock_guard lock(state_->mutex);
            if (state_->activeTargetCount >= state_->config.activeTargetLimit) {
                return PlatformPreflightResult(StagedArtifactError::AdmissionLimit);
            }
            const auto found = state_->records.find(identity);
            if (found != state_->records.end()) {
                record = found->second;
            } else {
                if (state_->records.size() >= state_->config.targetRecordLimit) {
                    return PlatformPreflightResult(StagedArtifactError::TargetRecordLimit);
                }
                if (state_->targetIdentityExhausted) {
                    return PlatformPreflightResult(StagedArtifactError::TargetIdentityExhausted);
                }
                const auto rawKey = state_->lastIssuedTargetKey + 1;
                if (rawKey == 0) {
                    state_->targetIdentityExhausted = true;
                    return PlatformPreflightResult(StagedArtifactError::TargetIdentityExhausted);
                }
                record = std::make_shared<TargetRecord>(core::ArtifactTargetKey::fromRaw(rawKey));
                state_->records.emplace(identity, record);
                state_->lastIssuedTargetKey = rawKey;
                if (rawKey == std::numeric_limits<std::uint64_t>::max()) {
                    state_->targetIdentityExhausted = true;
                }
            }
            ++state_->activeTargetCount;
        }

        try {
            TargetData data{state_,
                            AdmissionToken(state_),
                            std::move(record),
                            std::move(parentDescriptor),
                            identity.parent,
                            std::move(canonicalParent),
                            leaf,
                            inspection.observation,
                            request.overwritePolicy};
            return PlatformPreflightResult(std::make_unique<LinuxTarget>(std::move(data)));
        } catch (...) {
            // If TargetData construction completed, its admission token already released the
            // count. An exception before token ownership is established cannot occur here.
            throw;
        }
    }

    std::shared_ptr<LinuxSharedState> state_;
};

} // namespace

PlatformCoordinatorResult
createPlatformStagedArtifactCoordinator(const StagedArtifactConfig& config) noexcept {
    try {
        auto shared = std::make_shared<LinuxSharedState>(config);
        return PlatformCoordinatorResult(std::make_unique<LinuxCoordinator>(std::move(shared)));
    } catch (const std::bad_alloc&) {
        return PlatformCoordinatorResult(StagedArtifactError::ResourceUnavailable);
    } catch (...) {
        return PlatformCoordinatorResult(StagedArtifactError::ResourceUnavailable);
    }
}

} // namespace bloom::platform::detail
