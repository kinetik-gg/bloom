#include <bloom/platform/staged_artifact.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << "FAILED: " << message << '\n';
    }

    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

class TempDirectory final {
  public:
    TempDirectory() {
        std::array<char, 64> pattern{};
        constexpr std::string_view prefix = "/tmp/bloom-staged-artifact-XXXXXX";
        std::ranges::copy(prefix, pattern.begin());
        const auto* result = ::mkdtemp(pattern.data());
        if (result != nullptr) {
            path_ = result;
        }
    }

    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;
    ~TempDirectory() {
        if (path_.empty()) {
            return;
        }
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] bool isValid() const noexcept { return !path_.empty(); }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

  private:
    std::filesystem::path path_;
};

using bloom::platform::ArtifactOverwritePolicy;
using bloom::platform::PublicationDisposition;
using bloom::platform::StagedArtifactConfig;
using bloom::platform::StagedArtifactCoordinator;
using bloom::platform::StagedArtifactError;
using bloom::platform::StagedArtifactFaultPoint;
using bloom::platform::StagedArtifactPreflightRequest;
using bloom::platform::StagedArtifactPublicationOutcome;

static_assert(!std::is_copy_constructible_v<bloom::platform::StagedArtifactTarget>);
static_assert(!std::is_copy_constructible_v<bloom::platform::StagedArtifactLease>);
static_assert(std::is_nothrow_move_constructible_v<bloom::platform::StagedArtifactTarget>);
static_assert(std::is_nothrow_move_constructible_v<bloom::platform::StagedArtifactLease>);

[[nodiscard]] std::span<const std::byte> bytes(const std::string_view value) noexcept {
    return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}

[[nodiscard]] StagedArtifactPreflightRequest makeRequest(
    std::filesystem::path targetPath,
    const ArtifactOverwritePolicy overwritePolicy = ArtifactOverwritePolicy::CreateOrReplace,
    std::optional<bloom::platform::ArtifactTargetObservation> expectedTarget = std::nullopt) {
    return {.targetPath = std::move(targetPath),
            .overwritePolicy = overwritePolicy,
            .expectedTarget = expectedTarget};
}

[[nodiscard]] std::optional<StagedArtifactCoordinator>
makeCoordinator(Expectations& expectations, const StagedArtifactConfig config = {}) {
    auto result = StagedArtifactCoordinator::create(config);
    expectations.expect(result.succeeded(), "the staged-artifact coordinator is created");
    if (!result) {
        return std::nullopt;
    }
    return std::move(result).takeCoordinator();
}

[[nodiscard]] bool writeFile(const std::filesystem::path& path, const std::string_view contents) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    stream.close();
    return stream.good();
}

[[nodiscard]] std::string readFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

[[nodiscard]] std::vector<std::filesystem::path>
directoryEntries(const std::filesystem::path& path) {
    std::vector<std::filesystem::path> result;
    std::error_code error;
    for (std::filesystem::directory_iterator iterator(path, error), end; !error && iterator != end;
         iterator.increment(error)) {
        result.push_back(iterator->path());
    }
    return result;
}

void testConfigurationAndAdmission(Expectations& expectations) {
    StagedArtifactConfig invalid;
    invalid.activeTargetLimit = 0;
    auto invalidResult = StagedArtifactCoordinator::create(invalid);
    expectations.expect(!invalidResult &&
                            invalidResult.error() == StagedArtifactError::InvalidConfiguration,
                        "zero admission capacity is rejected");

    TempDirectory directory;
    if (!directory.isValid()) {
        expectations.expect(false, "the admission test creates a temporary directory");
        return;
    }
    StagedArtifactConfig config;
    config.activeTargetLimit = 3;
    config.targetRecordLimit = 3;
    auto coordinator = makeCoordinator(expectations, config);
    if (!coordinator.has_value()) {
        return;
    }

    auto first = coordinator->preflight(makeRequest(directory.path() / "alpha"));
    auto alias = coordinator->preflight(makeRequest(directory.path() / "." / "alpha"));
    expectations.expect(first && alias &&
                            first.target()->targetKey() == alias.target()->targetKey(),
                        "canonical parent identity and native leaf spelling converge aliases");
    {
        auto second = coordinator->preflight(makeRequest(directory.path() / "beta"));
        expectations.expect(static_cast<bool>(second),
                            "a distinct target consumes the third active admission");
        auto limited = coordinator->preflight(makeRequest(directory.path() / "gamma"));
        expectations.expect(!limited && limited.error() == StagedArtifactError::AdmissionLimit,
                            "active target admission is bounded before staging");
    }
    auto third = coordinator->preflight(makeRequest(directory.path() / "gamma"));
    expectations.expect(static_cast<bool>(third),
                        "released target admission becomes available again");
    const auto snapshot = coordinator->snapshot();
    expectations.expect(snapshot.activeTargetCount == 3 && snapshot.targetRecordCount == 3 &&
                            snapshot.activeTargetLimit == 3 && snapshot.targetRecordLimit == 3,
                        "the coordinator reports bounded active and canonical target state");

    StagedArtifactConfig recordConfig;
    recordConfig.activeTargetLimit = 2;
    recordConfig.targetRecordLimit = 1;
    auto recordCoordinator = makeCoordinator(expectations, recordConfig);
    if (!recordCoordinator.has_value()) {
        return;
    }
    auto retained = recordCoordinator->preflight(makeRequest(directory.path() / "record-one"));
    auto recordLimited = recordCoordinator->preflight(makeRequest(directory.path() / "record-two"));
    expectations.expect(retained && !recordLimited &&
                            recordLimited.error() == StagedArtifactError::TargetRecordLimit,
                        "canonical target records have an independent bound");

    auto sourceCoordinator = makeCoordinator(expectations);
    auto foreignCoordinator = makeCoordinator(expectations);
    if (!sourceCoordinator.has_value() || !foreignCoordinator.has_value()) {
        expectations.expect(false, "the coordinator ownership fixture is available");
        return;
    }
    auto foreignTarget = sourceCoordinator->preflight(makeRequest(directory.path() / "foreign"));
    if (!foreignTarget) {
        expectations.expect(false, "the coordinator ownership target is available");
        return;
    }
    const auto foreignStage = foreignCoordinator->stage(std::move(foreignTarget).takeTarget());
    expectations.expect(!foreignStage &&
                            foreignStage.error() == StagedArtifactError::CoordinatorMismatch,
                        "a target token cannot cross coordinator ownership domains");
}

void testCreateAndReplacePublication(Expectations& expectations) {
    TempDirectory directory;
    auto coordinator = makeCoordinator(expectations);
    if (!directory.isValid() || !coordinator.has_value()) {
        expectations.expect(false, "the publication fixture is available");
        return;
    }
    const auto targetPath = directory.path() / "frame.bin";
    auto preflight =
        coordinator->preflight(makeRequest(targetPath, ArtifactOverwritePolicy::CreateOnly));
    expectations.expect(preflight && !preflight.target()->observation().exists,
                        "create-only preflight captures an absent target");
    if (!preflight) {
        return;
    }
    auto stageResult = coordinator->stage(std::move(preflight).takeTarget());
    if (!stageResult) {
        expectations.expect(false, "same-directory exclusive stage creation succeeds");
        return;
    }
    auto lease = std::move(stageResult).takeLease();
    const auto stagedEntries = directoryEntries(directory.path());
    bool stageIsPrivate = false;
    if (stagedEntries.size() == 1) {
        const auto permissions = std::filesystem::status(stagedEntries.front()).permissions();
        constexpr auto nonOwnerPermissions =
            std::filesystem::perms::group_all | std::filesystem::perms::others_all;
        stageIsPrivate = (permissions & nonOwnerPermissions) == std::filesystem::perms::none;
    }
    expectations.expect(stagedEntries.size() == 1 && stagedEntries.front() != targetPath &&
                            std::filesystem::is_regular_file(stagedEntries.front()) &&
                            stageIsPrivate,
                        "staging creates one private regular file beside the target");
    expectations.expect(lease.write(bytes("new ")) && lease.write(bytes("artifact")) &&
                            lease.stageBytes() == 12 && lease.finishWriting() &&
                            lease.acceptVerification(),
                        "incremental writes finish and accepted bytes are flushed");
    const auto publication = lease.publish(PublicationDisposition::Proceed);
    expectations.expect(publication.outcome == StagedArtifactPublicationOutcome::Published &&
                            publication.error == StagedArtifactError::None &&
                            publication.targetWasPublished() &&
                            readFile(targetPath) == "new artifact",
                        "atomic create publishes accepted bytes with parent durability");
    const auto repeatedPublication = lease.publish(PublicationDisposition::Cancelled);
    expectations.expect(repeatedPublication.outcome == publication.outcome &&
                            repeatedPublication.error == publication.error,
                        "a terminal lease reports the original publication outcome on replay");
    const auto finalEntries = directoryEntries(directory.path());
    expectations.expect(finalEntries.size() == 1 && finalEntries.front() == targetPath,
                        "successful publication consumes the staging name");

    auto replacement =
        coordinator->preflight(makeRequest(targetPath, ArtifactOverwritePolicy::ReplaceExisting));
    if (!replacement) {
        expectations.expect(false, "replace-existing preflight observes the published target");
        return;
    }
    auto replacementStage = coordinator->stage(std::move(replacement).takeTarget());
    auto replacementLease = std::move(replacementStage).takeLease();
    const auto replaced = replacementLease.write(bytes("replacement"));
    const auto finished = replacementLease.finishWriting();
    const auto accepted = replacementLease.acceptVerification();
    const auto replacedResult = replacementLease.publish(PublicationDisposition::Proceed);
    expectations.expect(replaced && finished && accepted &&
                            replacedResult.outcome == StagedArtifactPublicationOutcome::Published &&
                            readFile(targetPath) == "replacement",
                        "atomic replacement updates an existing regular target");
}

void testTargetChecksAndExpectedState(Expectations& expectations) {
    TempDirectory directory;
    auto coordinator = makeCoordinator(expectations);
    if (!directory.isValid() || !coordinator.has_value()) {
        return;
    }
    const auto regularPath = directory.path() / "regular";
    const auto symlinkPath = directory.path() / "link";
    const auto directoryPath = directory.path() / "folder";
    expectations.expect(writeFile(regularPath, "original"),
                        "the target check fixture writes a file");
    std::error_code error;
    std::filesystem::create_symlink(regularPath, symlinkPath, error);
    std::filesystem::create_directory(directoryPath, error);
    const auto symlink = coordinator->preflight(makeRequest(symlinkPath));
    const auto nonRegular = coordinator->preflight(makeRequest(directoryPath));
    expectations.expect(!symlink && symlink.error() == StagedArtifactError::TargetLeafSymlink,
                        "target-leaf symlinks are rejected without traversal");
    expectations.expect(!nonRegular &&
                            nonRegular.error() == StagedArtifactError::TargetLeafNotRegular,
                        "non-regular target leaves are rejected");

    auto observed = coordinator->preflight(makeRequest(regularPath));
    if (!observed) {
        expectations.expect(false, "an existing regular target is fingerprinted");
        return;
    }
    const auto expected = observed.target()->observation();
    observed = coordinator->preflight(makeRequest(directory.path() / "unused"));
    expectations.expect(writeFile(regularPath, "externally changed"),
                        "the fixture mutates the target after observation");
    auto conflict = coordinator->preflight(
        makeRequest(regularPath, ArtifactOverwritePolicy::CreateOrReplace, expected));
    expectations.expect(!conflict &&
                            conflict.error() == StagedArtifactError::ExternalModificationConflict,
                        "caller-supplied expected fingerprint rejects an earlier external change");
}

void testExternalMutationBeforePublication(Expectations& expectations) {
    TempDirectory directory;
    auto coordinator = makeCoordinator(expectations);
    if (!directory.isValid() || !coordinator.has_value()) {
        return;
    }
    const auto targetPath = directory.path() / "project.bloom";
    expectations.expect(writeFile(targetPath, "old"), "the conflict fixture writes its old target");
    auto preflight = coordinator->preflight(makeRequest(targetPath));
    auto stage = coordinator->stage(std::move(preflight).takeTarget());
    auto lease = std::move(stage).takeLease();
    expectations.expect(lease.write(bytes("new")) && lease.finishWriting() &&
                            lease.acceptVerification(),
                        "the conflict fixture verifies a replacement");
    expectations.expect(writeFile(targetPath, "external"),
                        "the target changes after staging and before publication");
    const auto result = lease.publish(PublicationDisposition::Proceed);
    expectations.expect(
        result.outcome == StagedArtifactPublicationOutcome::ExternalModificationConflict &&
            result.error == StagedArtifactError::ExternalModificationConflict &&
            !result.targetWasPublished() && readFile(targetPath) == "external" &&
            directoryEntries(directory.path()) == std::vector<std::filesystem::path>{targetPath},
        "complete fingerprint revalidation preserves an externally changed target");
}

void testVerificationReadsAndAcceptance(Expectations& expectations) {
    TempDirectory directory;
    auto coordinator = makeCoordinator(expectations);
    if (!directory.isValid() || !coordinator.has_value()) {
        return;
    }
    const auto targetPath = directory.path() / "binary";
    auto preflight = coordinator->preflight(makeRequest(targetPath));
    auto stage = coordinator->stage(std::move(preflight).takeTarget());
    auto lease = std::move(stage).takeLease();
    constexpr std::array payload = {std::byte{0x00}, std::byte{0x11}, std::byte{0x7F},
                                    std::byte{0x80}, std::byte{0xFE}, std::byte{0xFF}};
    expectations.expect(static_cast<bool>(lease.write(payload)),
                        "the verification fixture writes binary bytes");

    const auto premature = lease.publish(PublicationDisposition::Proceed);
    expectations.expect(premature.outcome ==
                                StagedArtifactPublicationOutcome::FailedBeforePublication &&
                            premature.error == StagedArtifactError::StageVerificationNotAccepted &&
                            directoryEntries(directory.path()).size() == 1,
                        "proceed cannot consume bytes before semantic verification is accepted");

    expectations.expect(static_cast<bool>(lease.finishWriting()),
                        "finishing closes and reopens the exact stage for verification");
    const auto unaccepted = lease.publish(PublicationDisposition::Proceed);
    const auto lateWrite = lease.write(bytes("x"));
    expectations.expect(
        unaccepted.error == StagedArtifactError::StageVerificationNotAccepted && !lateWrite &&
            lateWrite.error == StagedArtifactError::StageNotWritable,
        "publication stays blocked and the writer cannot be reused during verification");

    std::array<std::byte, 3> middle{};
    const auto middleRead = lease.readForVerification(1, middle);
    expectations.expect(middleRead && middleRead.bytesRead == middle.size() &&
                            middleRead.stageBytes == payload.size() && !middleRead.endOfFile &&
                            middle == std::array{std::byte{0x11}, std::byte{0x7F}, std::byte{0x80}},
                        "bounded random access returns exact binary bytes before EOF");

    std::array<std::byte, 5> tail{};
    tail.fill(std::byte{0x55});
    const auto tailRead = lease.readForVerification(3, tail);
    expectations.expect(tailRead && tailRead.bytesRead == 3 && tailRead.endOfFile &&
                            tail[0] == std::byte{0x80} && tail[1] == std::byte{0xFE} &&
                            tail[2] == std::byte{0xFF} && tail[3] == std::byte{0x55} &&
                            tail[4] == std::byte{0x55},
                        "a verification read reports its partial count and preserves tail bytes");

    std::array<std::byte, 1> one{};
    const auto exactEnd = lease.readForVerification(payload.size(), one);
    const auto beyondEnd =
        lease.readForVerification(std::numeric_limits<std::uint64_t>::max(), one);
    const auto emptyRead = lease.readForVerification(0, std::span<std::byte>{});
    expectations.expect(exactEnd && exactEnd.bytesRead == 0 && exactEnd.endOfFile && beyondEnd &&
                            beyondEnd.bytesRead == 0 && beyondEnd.endOfFile && emptyRead &&
                            emptyRead.bytesRead == 0 && !emptyRead.endOfFile,
                        "EOF and maximal offsets are zero-byte successes without offset wrap");

    const auto accepted = lease.acceptVerification();
    const auto acceptedReplay = lease.acceptVerification();
    const auto readAfterAccept = lease.readForVerification(0, one);
    expectations.expect(accepted && acceptedReplay && !readAfterAccept &&
                            readAfterAccept.error == StagedArtifactError::StageNotVerifying,
                        "verification acceptance flushes once and has a stable successful replay");
    const auto publication = lease.publish(PublicationDisposition::Proceed);
    const auto contents = readFile(targetPath);
    expectations.expect(publication.targetWasPublished() && contents.size() == payload.size() &&
                            std::equal(payload.begin(), payload.end(),
                                       reinterpret_cast<const std::byte*>(contents.data())),
                        "only accepted binary bytes reach the target");

    const auto emptyTargetPath = directory.path() / "empty";
    auto emptyPreflight = coordinator->preflight(makeRequest(emptyTargetPath));
    auto emptyStage = coordinator->stage(std::move(emptyPreflight).takeTarget());
    auto emptyLease = std::move(emptyStage).takeLease();
    std::array<std::byte, 1> emptyBuffer{std::byte{0x55}};
    const auto emptyFinished = emptyLease.finishWriting();
    const auto emptyEof = emptyLease.readForVerification(0, emptyBuffer);
    const auto emptyAccepted = emptyLease.acceptVerification();
    const auto emptyPublication = emptyLease.publish(PublicationDisposition::Proceed);
    const auto emptyPublicationReplay = emptyLease.publish(PublicationDisposition::Cancelled);
    expectations.expect(emptyFinished && emptyEof && emptyEof.bytesRead == 0 &&
                            emptyEof.stageBytes == 0 && emptyEof.endOfFile && emptyAccepted &&
                            emptyPublication.targetWasPublished() &&
                            emptyPublicationReplay.outcome == emptyPublication.outcome &&
                            emptyPublicationReplay.error == emptyPublication.error &&
                            std::filesystem::file_size(emptyTargetPath) == 0,
                        "an empty stage reports ordinary EOF and can still be accepted exactly");
}

void testVerificationRejectionAndTerminalReplay(Expectations& expectations) {
    TempDirectory directory;
    auto coordinator = makeCoordinator(expectations);
    if (!directory.isValid() || !coordinator.has_value()) {
        return;
    }
    const auto targetPath = directory.path() / "rejected";
    auto preflight = coordinator->preflight(makeRequest(targetPath));
    auto stage = coordinator->stage(std::move(preflight).takeTarget());
    auto lease = std::move(stage).takeLease();
    expectations.expect(lease.write(bytes("invalid format")) && lease.finishWriting(),
                        "the rejection fixture enters semantic verification");
    const auto rejected = lease.rejectVerification();
    const auto rejectedReplay = lease.rejectVerification();
    const auto publication = lease.publish(PublicationDisposition::Proceed);
    const auto publicationReplay = lease.publish(PublicationDisposition::Cancelled);
    expectations.expect(rejected && rejectedReplay && lease.isValid() &&
                            directoryEntries(directory.path()).empty() &&
                            !std::filesystem::exists(targetPath),
                        "semantic rejection cleans immediately while the lease remains alive");
    expectations.expect(
        publication.outcome == StagedArtifactPublicationOutcome::FailedBeforePublication &&
            publication.error == StagedArtifactError::StageVerificationRejected &&
            publicationReplay.outcome == publication.outcome &&
            publicationReplay.error == publication.error,
        "a rejected lease replays its typed terminal result for every publication attempt");
}

void testDispositionAndLimits(Expectations& expectations) {
    for (const auto disposition :
         {PublicationDisposition::Cancelled, PublicationDisposition::Superseded}) {
        for (const bool enterVerification : {false, true}) {
            TempDirectory directory;
            auto coordinator = makeCoordinator(expectations);
            if (!directory.isValid() || !coordinator.has_value()) {
                continue;
            }
            const auto targetPath = directory.path() / "output";
            auto preflight = coordinator->preflight(makeRequest(targetPath));
            auto stage = coordinator->stage(std::move(preflight).takeTarget());
            auto lease = std::move(stage).takeLease();
            const auto write = lease.write(bytes("discarded"));
            const auto finished = enterVerification
                                      ? lease.finishWriting()
                                      : bloom::platform::StagedArtifactOperationResult{};
            expectations.expect(write && finished,
                                "a disposition fixture reaches its requested live phase");
            const auto result = lease.publish(disposition);
            const auto replay = lease.publish(PublicationDisposition::Proceed);
            const auto expected = disposition == PublicationDisposition::Cancelled
                                      ? StagedArtifactPublicationOutcome::CancelledBeforePublication
                                      : StagedArtifactPublicationOutcome::Superseded;
            expectations.expect(
                result.outcome == expected && replay.outcome == result.outcome &&
                    replay.error == result.error && !result.targetWasPublished() &&
                    !std::filesystem::exists(targetPath) &&
                    directoryEntries(directory.path()).empty(),
                "cancellation and supersession clean from writable and verification phases");
        }
    }

    TempDirectory directory;
    StagedArtifactConfig config;
    config.artifactByteLimit = 3;
    auto coordinator = makeCoordinator(expectations, config);
    if (!directory.isValid() || !coordinator.has_value()) {
        return;
    }
    const auto targetPath = directory.path() / "limited";
    auto preflight = coordinator->preflight(makeRequest(targetPath));
    auto stage = coordinator->stage(std::move(preflight).takeTarget());
    auto lease = std::move(stage).takeLease();
    const auto write = lease.write(bytes("four"));
    const auto replay = lease.publish(PublicationDisposition::Proceed);
    expectations.expect(!write && write.error == StagedArtifactError::ArtifactSizeLimit &&
                            replay.error == write.error && write.stageBytes == 0 &&
                            !std::filesystem::exists(targetPath) &&
                            directoryEntries(directory.path()).empty(),
                        "artifact growth failure cleans and remains a stable terminal error");
}

[[nodiscard]] bloom::platform::StagedArtifactPublicationResult
runPublicationFault(Expectations& expectations, const StagedArtifactFaultPoint fault,
                    const std::uint64_t occurrence, std::string& finalBytes) {
    TempDirectory directory;
    const auto targetPath = directory.path() / "target";
    static_cast<void>(writeFile(targetPath, "old"));
    StagedArtifactConfig config;
    config.faults = {.point = fault, .occurrence = occurrence};
    auto coordinator = makeCoordinator(expectations, config);
    if (!directory.isValid() || !coordinator.has_value()) {
        finalBytes = readFile(targetPath);
        return {};
    }
    auto preflight = coordinator->preflight(makeRequest(targetPath));
    if (!preflight) {
        finalBytes = readFile(targetPath);
        return {.outcome = StagedArtifactPublicationOutcome::FailedBeforePublication,
                .error = preflight.error()};
    }
    auto stage = coordinator->stage(std::move(preflight).takeTarget());
    if (!stage) {
        finalBytes = readFile(targetPath);
        return {.outcome = StagedArtifactPublicationOutcome::FailedBeforePublication,
                .error = stage.error()};
    }
    auto lease = std::move(stage).takeLease();
    const auto terminalFailure = [&directory, &expectations, &lease,
                                  &targetPath](const StagedArtifactError error) {
        const auto first = lease.publish(PublicationDisposition::Proceed);
        const auto replay = lease.publish(PublicationDisposition::Cancelled);
        expectations.expect(first.outcome ==
                                    StagedArtifactPublicationOutcome::FailedBeforePublication &&
                                first.error == error && replay.outcome == first.outcome &&
                                replay.error == first.error &&
                                directoryEntries(directory.path()) ==
                                    std::vector<std::filesystem::path>{targetPath},
                            "a terminal operation fault replays through publication");
        return first;
    };
    const auto write = lease.write(bytes("new"));
    if (!write) {
        finalBytes = readFile(targetPath);
        return terminalFailure(write.error);
    }
    const auto finished = lease.finishWriting();
    if (!finished) {
        finalBytes = readFile(targetPath);
        return terminalFailure(finished.error);
    }
    std::array<std::byte, 1> verificationByte{};
    const auto read = lease.readForVerification(0, verificationByte);
    if (!read) {
        finalBytes = readFile(targetPath);
        return terminalFailure(read.error);
    }
    const auto accepted = lease.acceptVerification();
    if (!accepted) {
        finalBytes = readFile(targetPath);
        return terminalFailure(accepted.error);
    }
    const auto result = lease.publish(PublicationDisposition::Proceed);
    finalBytes = readFile(targetPath);
    expectations.expect(directoryEntries(directory.path()) ==
                            std::vector<std::filesystem::path>{targetPath},
                        "publication faults never leave a staging name behind");
    return result;
}

void testDeterministicFaultOrdering(Expectations& expectations) {
    for (const auto fault :
         {StagedArtifactFaultPoint::TargetInspection, StagedArtifactFaultPoint::StageCreation,
          StagedArtifactFaultPoint::StageWrite, StagedArtifactFaultPoint::StageWriterClose,
          StagedArtifactFaultPoint::StageReopen, StagedArtifactFaultPoint::StageVerificationRead,
          StagedArtifactFaultPoint::StageVerificationAccept, StagedArtifactFaultPoint::StageFlush,
          StagedArtifactFaultPoint::IdentityRevalidation,
          StagedArtifactFaultPoint::AtomicPublication}) {
        std::string finalBytes;
        const auto result = runPublicationFault(expectations, fault, 1, finalBytes);
        expectations.expect(
            result.outcome == StagedArtifactPublicationOutcome::FailedBeforePublication &&
                result.error == StagedArtifactError::FaultInjected && finalBytes == "old",
            "every injected pre-publication failure preserves the old target");
    }

    std::string finalBytes;
    const auto durability = runPublicationFault(
        expectations, StagedArtifactFaultPoint::ParentDurability, 1, finalBytes);
    expectations.expect(
        durability.outcome == StagedArtifactPublicationOutcome::PublishedWithDurabilityWarning &&
            durability.error == StagedArtifactError::FaultInjected && finalBytes == "new" &&
            durability.targetWasPublished(),
        "a deterministic post-publication durability fault reports the visible new target");
}

void testParentAndStageIdentityRevalidation(Expectations& expectations) {
    TempDirectory directory;
    auto coordinator = makeCoordinator(expectations);
    if (!directory.isValid() || !coordinator.has_value()) {
        return;
    }
    const auto parent = directory.path() / "parent";
    const auto movedParent = directory.path() / "moved-parent";
    std::filesystem::create_directory(parent);
    auto preflight = coordinator->preflight(makeRequest(parent / "target"));
    auto stage = coordinator->stage(std::move(preflight).takeTarget());
    auto lease = std::move(stage).takeLease();
    expectations.expect(lease.write(bytes("new")) && lease.finishWriting() &&
                            lease.acceptVerification(),
                        "the parent-identity fixture verifies its stage");
    std::filesystem::rename(parent, movedParent);
    std::filesystem::create_directory(parent);
    const auto publication = lease.publish(PublicationDisposition::Proceed);
    expectations.expect(
        publication.outcome == StagedArtifactPublicationOutcome::FailedBeforePublication &&
            publication.error == StagedArtifactError::ParentIdentityMismatch &&
            !std::filesystem::exists(parent / "target") &&
            !std::filesystem::exists(movedParent / "target") &&
            directoryEntries(movedParent).empty(),
        "replacing the resolved parent identity blocks publication and cleans its stage");

    const auto targetPath = parent / "second-target";
    auto secondPreflight = coordinator->preflight(makeRequest(targetPath));
    auto secondStage = coordinator->stage(std::move(secondPreflight).takeTarget());
    {
        auto secondLease = std::move(secondStage).takeLease();
        const auto entries = directoryEntries(parent);
        expectations.expect(entries.size() == 1,
                            "the stage identity fixture finds its private file");
        if (entries.size() == 1) {
            std::filesystem::remove(entries.front());
            static_cast<void>(writeFile(entries.front(), "attacker"));
            const auto finished = secondLease.finishWriting();
            expectations.expect(!finished &&
                                    finished.error == StagedArtifactError::StageIdentityMismatch,
                                "a writer-close/reopen replacement fails identity revalidation");
        }
    }
    const auto remaining = directoryEntries(parent);
    expectations.expect(remaining.size() == 1 && readFile(remaining.front()) == "attacker" &&
                            coordinator->snapshot().cleanupFailureCount == 1,
                        "RAII cleanup refuses to unlink a foreign replacement and records failure");

    if (remaining.size() != 1) {
        return;
    }
    std::filesystem::remove(remaining.front());

    auto verificationPreflight =
        coordinator->preflight(makeRequest(parent / "verification-target"));
    auto verificationStage = coordinator->stage(std::move(verificationPreflight).takeTarget());
    {
        auto verificationLease = std::move(verificationStage).takeLease();
        expectations.expect(verificationLease.write(bytes("verified inode")) &&
                                verificationLease.finishWriting(),
                            "the replacement fixture reopens its stage for verification");
        const auto stageEntries = directoryEntries(parent);
        if (stageEntries.size() != 1) {
            expectations.expect(false, "the verification replacement fixture finds its stage");
            return;
        }
        std::filesystem::remove(stageEntries.front());
        static_cast<void>(writeFile(stageEntries.front(), "replacement inode"));
        const auto accepted = verificationLease.acceptVerification();
        expectations.expect(
            !accepted && accepted.error == StagedArtifactError::StageIdentityMismatch,
            "acceptance rejects a stage leaf replaced after the verification reopen");
    }
    const auto verificationReplacement = directoryEntries(parent);
    expectations.expect(
        verificationReplacement.size() == 1 &&
            readFile(verificationReplacement.front()) == "replacement inode" &&
            coordinator->snapshot().cleanupFailureCount == 2,
        "failed acceptance preserves a foreign replacement and records cleanup evidence");
    if (verificationReplacement.size() != 1) {
        return;
    }
    std::filesystem::remove(verificationReplacement.front());

    auto acceptedPreflight = coordinator->preflight(makeRequest(parent / "accepted-target"));
    auto acceptedStage = coordinator->stage(std::move(acceptedPreflight).takeTarget());
    {
        auto acceptedLease = std::move(acceptedStage).takeLease();
        expectations.expect(acceptedLease.write(bytes("accepted inode")) &&
                                acceptedLease.finishWriting() && acceptedLease.acceptVerification(),
                            "the accepted replacement fixture accepts its exact inode");
        const auto stageEntries = directoryEntries(parent);
        if (stageEntries.size() != 1) {
            expectations.expect(false, "the accepted replacement fixture finds its stage");
            return;
        }
        std::filesystem::remove(stageEntries.front());
        static_cast<void>(writeFile(stageEntries.front(), "post-accept replacement"));
        const auto acceptedPublication = acceptedLease.publish(PublicationDisposition::Proceed);
        expectations.expect(acceptedPublication.outcome ==
                                    StagedArtifactPublicationOutcome::FailedBeforePublication &&
                                acceptedPublication.error ==
                                    StagedArtifactError::StageIdentityMismatch,
                            "publication revalidates the accepted inode immediately before rename");
    }
    const auto acceptedReplacement = directoryEntries(parent);
    expectations.expect(acceptedReplacement.size() == 1 &&
                            readFile(acceptedReplacement.front()) == "post-accept replacement" &&
                            coordinator->snapshot().cleanupFailureCount == 3,
                        "publication failure preserves a post-accept foreign replacement");
    if (acceptedReplacement.size() != 1) {
        return;
    }
    std::filesystem::remove(acceptedReplacement.front());

    auto hardLinkPreflight = coordinator->preflight(makeRequest(parent / "hard-link-target"));
    auto hardLinkStage = coordinator->stage(std::move(hardLinkPreflight).takeTarget());
    {
        auto hardLinkLease = std::move(hardLinkStage).takeLease();
        const auto stageEntries = directoryEntries(parent);
        if (stageEntries.size() != 1) {
            expectations.expect(false, "the hard-link fixture finds its private stage");
            return;
        }
        const auto linkedPath = parent / "foreign-hard-link";
        std::error_code hardLinkError;
        std::filesystem::create_hard_link(stageEntries.front(), linkedPath, hardLinkError);
        expectations.expect(!hardLinkError, "the fixture adds an unexpected stage hard link");
        const auto finished = hardLinkLease.finishWriting();
        expectations.expect(!finished &&
                                finished.error == StagedArtifactError::StageIdentityMismatch,
                            "an unexpected hard link invalidates stage ownership");
    }
    const auto linkedPath = parent / "foreign-hard-link";
    expectations.expect(
        std::filesystem::exists(linkedPath) && directoryEntries(parent).size() == 1 &&
            coordinator->snapshot().cleanupFailureCount == 4,
        "cleanup removes only Bloom's stage name and reports the retained hard link");
}

} // namespace

int main() {
    Expectations expectations;
    testConfigurationAndAdmission(expectations);
    testCreateAndReplacePublication(expectations);
    testTargetChecksAndExpectedState(expectations);
    testExternalMutationBeforePublication(expectations);
    testVerificationReadsAndAcceptance(expectations);
    testVerificationRejectionAndTerminalReplay(expectations);
    testDispositionAndLimits(expectations);
    testDeterministicFaultOrdering(expectations);
    testParentAndStageIdentityRevalidation(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
