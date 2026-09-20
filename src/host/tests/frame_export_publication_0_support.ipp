// Support fragment for frame_export_publication_tests.cpp. Included inside that file's anonymous
// namespace after its includes and aliases. It holds the shared fixture vocabulary (the failure
// collector, the optional-dereference helper, a temp directory, the closed id set, the trivial
// one-node plan, the attempt request builders, the export fixture owning the real coordinator/
// scheduler/ledger, and the submit/await helpers). No test is defined here.

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message,
                const std::source_location location = std::source_location::current()) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }

    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

// The one place this file dereferences a std::optional (or a std::optional-shaped result type)
// without a directly-adjacent has_value()/operator bool() check: it performs that check right
// here and returns a raw pointer, so every call site below works with a plain (possibly-null)
// pointer instead -- clang-tidy's bugprone-unchecked-optional-access only tracks std::optional
// itself, so converting to a pointer once, in one place, is the established way to avoid repeating
// an unprovable-at-a-distance check at dozens of call sites.
template <typename Optional>
[[nodiscard]] auto require(Optional& value, Expectations& expectations,
                           const std::string_view message) -> decltype(&*value) {
    expectations.expect(static_cast<bool>(value), message);
    if (!value) {
        return nullptr;
    }
    return &*value; // NOLINT(bugprone-unchecked-optional-access) -- guarded immediately above.
}

class TempDirectory final {
  public:
    TempDirectory() {
        std::array<char, 64> pattern{};
        constexpr std::string_view prefix = "/tmp/bloom-frame-export-XXXXXX";
        std::ranges::copy(prefix, pattern.begin());
        const auto* result = ::mkdtemp(pattern.data());
        if (result != nullptr) {
            path_ = result;
        }
    }
    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;
    ~TempDirectory() {
        if (!path_.empty()) {
            std::error_code errorCode;
            std::filesystem::remove_all(path_, errorCode);
        }
    }

    [[nodiscard]] bool isValid() const noexcept { return !path_.empty(); }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

  private:
    std::filesystem::path path_;
};

constexpr auto kProjectId = document::ProjectId::fromRaw(0x2001);
constexpr auto kCompositionId = document::CompositionId::fromRaw(0x2002);
constexpr auto kSolidNodeId = document::NodeId::fromRaw(0x2003);
constexpr auto kOutputNodeId = document::NodeId::fromRaw(0x2004);
constexpr auto kColorParameterId = document::ParameterId::fromRaw(0x2005);
constexpr auto kWidthParameterId = document::ParameterId::fromRaw(0x2105);
constexpr auto kHeightParameterId = document::ParameterId::fromRaw(0x2106);
constexpr auto kRevision = document::Revision::fromRaw(0x2006);
constexpr auto kLayerNodeId = document::NodeId::fromRaw(0x2007);
constexpr auto kLayerId = document::LayerId::fromRaw(0x2008);
constexpr auto kStackNodeId = document::NodeId::fromRaw(0x2009);
constexpr auto kSlotId = document::LayerSlotId::fromRaw(0x200a);
constexpr auto kPositionParameterId = document::ParameterId::fromRaw(0x200b);
constexpr auto kOpacityParameterId = document::ParameterId::fromRaw(0x200c);
constexpr auto kAnchorParameterId = document::ParameterId::fromRaw(0x200d);
constexpr auto kScaleParameterId = document::ParameterId::fromRaw(0x200e);
constexpr auto kRotationParameterId = document::ParameterId::fromRaw(0x200f);
constexpr auto kBlendModeParameterId = document::ParameterId::fromRaw(0x2010);

[[nodiscard]] std::shared_ptr<const runtime::CompiledCompositionPlan> smallSolidPlan() {
    const auto format = document::CompositionFormat::create(2, 2);
    if (!format) {
        std::abort();
    }
    std::vector<runtime::CompiledOperation> operations;
    operations.emplace_back(
        runtime::CompiledSolid{kSolidNodeId,
                               {kColorParameterId, bloom::core::Color4d{0.1, 0.2, 0.3, 1.0}},
                               {kWidthParameterId, 2.0},
                               {kHeightParameterId, 2.0}});
    // CompiledCompositionOutput requires a layer-stack input, not a bare solid (mirrors
    // bloom/output/tests/flat_exr_test_support.hpp's shellPlan()): solid -> layer output -> layer
    // stack -> composition output.
    operations.emplace_back(runtime::CompiledLayerOutput{
        kLayerNodeId, kLayerId, runtime::OperationIndex::fromRaw(0),
        runtime::CompiledVec2Parameter{kPositionParameterId, document::Vec2d{0.5, 0.5}},
        runtime::CompiledVec2Parameter{kAnchorParameterId, document::kDefaultAnchor},
        runtime::CompiledVec2Parameter{kScaleParameterId, document::kDefaultScale},
        runtime::CompiledScalarParameter{kRotationParameterId, document::kDefaultRotationDegrees},
        runtime::CompiledScalarParameter{kOpacityParameterId, 1.0}, kBlendModeParameterId,
        bloom::core::kDefaultBlendMode});
    operations.emplace_back(runtime::CompiledMerge{
        kStackNodeId, {{kSlotId, kLayerId, runtime::OperationIndex::fromRaw(1)}}});
    operations.emplace_back(
        runtime::CompiledCompositionOutput{kOutputNodeId, runtime::OperationIndex::fromRaw(2)});
    return std::make_shared<const runtime::CompiledCompositionPlan>(
        runtime::CompiledCompositionPlanDefinition{.sourceRevision = kRevision,
                                                   .projectId = kProjectId,
                                                   .compositionId = kCompositionId,
                                                   .format = *format,
                                                   .operations = std::move(operations),
                                                   .output = runtime::OperationIndex::fromRaw(3)});
}

[[nodiscard]] host::OutputAnalysisAttemptRequestV1 attemptRequestFor(
    const std::filesystem::path& target,
    const output::OutputPresetV1 preset = output::OutputPresetV1::FlatExrRgba32fLinRec709SceneV1) {
    const auto plan = smallSolidPlan();
    return {
        .plan = plan,
        .evaluation = {.time = bloom::core::RationalTime::fromInteger(0),
                       .output = plan->output(),
                       .resolution = runtime::CompositionFormatResolution{},
                       .quality = runtime::EvaluationQuality::Reference,
                       .colorIntent = runtime::EvaluationColorIntent::LinearRec709Scene,
                       .pixelStorageByteLimit = 4096},
        .targetPath = target,
        .overwritePolicy = platform::ArtifactOverwritePolicy::CreateOrReplace,
        .owner = {.kind = runtime::TaskOwnerKind::Export, .id = runtime::TaskOwnerId::fromRaw(1)},
        .preset = preset};
}

// Owns a real TaskScheduler/StagedArtifactCoordinator/PublicationCoordinator/ExportResourceLedgerV1
// fixture set. TaskScheduler is non-movable (its move constructor/assignment are explicitly
// deleted) and StagedArtifactCoordinator/PublicationCoordinator are only constructible through
// their own ::create() factories, so every member is held in-place inside a std::optional and
// populated by setUp() -- the accessor methods below are the ONE place each member's optional is
// dereferenced (guarded by setUp()'s own already-checked success), rather than repeating an
// unprovable-at-a-distance dereference at every one of this file's many call sites.
class ExportFixture final {
  public:
    [[nodiscard]] bool setUp(Expectations& expectations, const std::string_view context) {
        auto artifactsResult = platform::StagedArtifactCoordinator::create({});
        auto coordinatorResult = host::PublicationCoordinator::create();
        const bool ok =
            directory_.isValid() && artifactsResult.succeeded() && coordinatorResult.has_value();
        expectations.expect(ok, context);
        if (!ok) {
            return false;
        }
        scheduler_.emplace();
        artifacts_.emplace(std::move(artifactsResult).takeCoordinator());
        coordinator_.emplace(
            std::move(*coordinatorResult)); // NOLINT(bugprone-unchecked-optional-access)
        ledger_.emplace();
        return true;
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return directory_.path(); }
    [[nodiscard]] runtime::TaskScheduler& scheduler() noexcept {
        return *scheduler_; // NOLINT(bugprone-unchecked-optional-access) -- guaranteed by setUp().
    }
    [[nodiscard]] platform::StagedArtifactCoordinator& artifacts() noexcept {
        return *artifacts_; // NOLINT(bugprone-unchecked-optional-access) -- guaranteed by setUp().
    }
    [[nodiscard]] host::PublicationCoordinator& coordinator() noexcept {
        return *coordinator_; // NOLINT(bugprone-unchecked-optional-access) -- guaranteed by
                              // setUp().
    }
    [[nodiscard]] output::ExportResourceLedgerV1& ledger() noexcept {
        return *ledger_; // NOLINT(bugprone-unchecked-optional-access) -- guaranteed by setUp().
    }

  private:
    TempDirectory directory_;
    std::optional<runtime::TaskScheduler> scheduler_;
    std::optional<platform::StagedArtifactCoordinator> artifacts_;
    std::optional<host::PublicationCoordinator> coordinator_;
    std::optional<output::ExportResourceLedgerV1> ledger_;
};

// Runs beginOutputAnalysisAttemptV1() to a completed, approvable attempt over `target`. Every test
// fixture below needs a real attempt (frame + identity + report + digest), not a hand-built one --
// approveFrameExportV1() cannot be exercised meaningfully without one.
[[nodiscard]] std::shared_ptr<const output::OutputAnalysisAttemptV1> buildApprovableAttempt(
    Expectations& expectations, runtime::TaskScheduler& scheduler,
    platform::StagedArtifactCoordinator& artifacts, output::ExportResourceLedgerV1& ledger,
    const std::filesystem::path& target,
    const output::OutputPresetV1 preset = output::OutputPresetV1::FlatExrRgba32fLinRec709SceneV1) {
    auto begin = host::beginOutputAnalysisAttemptV1(scheduler, artifacts, ledger,
                                                    attemptRequestFor(target, preset));
    expectations.expect(static_cast<bool>(begin),
                        "attempt fixture: begin submits the Resolving task");
    if (!begin) {
        return nullptr;
    }
    auto runner = std::move(begin).takeHandle();
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        auto outcome = runner.tryComplete();
        if (!outcome.has_value()) {
            std::this_thread::sleep_for(1ms);
            continue;
        }
        const auto& outcomeValue = *outcome; // NOLINT(bugprone-unchecked-optional-access)
        expectations.expect(static_cast<bool>(outcomeValue),
                            "attempt fixture: the attempt completes");
        return static_cast<bool>(outcomeValue) ? outcomeValue.attempt() : nullptr;
    }
    expectations.expect(false, "attempt fixture: the attempt reaches a terminal outcome in time");
    return nullptr;
}

// Retrieves attempt->digest() with an immediately-adjacent has_value() check (attempt->digest()
// is a fresh temporary each call, so no earlier check anywhere else in the caller can cover it).
[[nodiscard]] bloom::core::Sha256Digest
requireDigest(const std::shared_ptr<const output::OutputAnalysisAttemptV1>& attempt,
              Expectations& expectations) {
    const auto digest = attempt->digest();
    expectations.expect(digest.has_value(), "the attempt fixture is approvable with a digest");
    if (!digest.has_value()) {
        return {};
    }
    return *digest; // NOLINT(bugprone-unchecked-optional-access) -- guarded immediately above.
}

struct ExportRun final {
    runtime::TaskHandle<void> handle;
    std::shared_ptr<std::optional<host::FrameExportPublicationResultV1>> result;
};

[[nodiscard]] std::optional<ExportRun>
beginExportRun(runtime::TaskScheduler& scheduler, platform::StagedArtifactCoordinator& artifacts,
               std::unique_ptr<host::FrameExportRequestV1> request,
               const std::filesystem::path& scratchDir,
               const output::OutputExportClockV1& clock = {}) {
    auto shared = std::make_shared<std::optional<host::FrameExportPublicationResultV1>>();
    auto sharedRequest = std::shared_ptr<host::FrameExportRequestV1>(std::move(request));
    auto submission = scheduler.submit<void>(
        runtime::TaskRequest("export-publication-test",
                             runtime::TaskOwner{.kind = runtime::TaskOwnerKind::Export,
                                                .id = runtime::TaskOwnerId::fromRaw(2)},
                             runtime::TaskPriority::Foreground, runtime::TaskExecutor::BlockingIo),
        [&artifacts, sharedRequest, shared, scratchDir, clock](runtime::TaskContext& context) {
            auto result = host::executeExportPublication(
                context, artifacts, std::move(*sharedRequest), scratchDir, clock);
            shared->emplace(std::move(result));
            return runtime::TaskResult<void>::succeeded();
        });
    if (!submission.accepted()) {
        return std::nullopt;
    }
    return ExportRun{std::move(submission.handle), shared};
}

// Polls handle.tryTakeResult() to terminality, then returns the worker's own recorded outcome.
[[nodiscard]] std::optional<host::FrameExportPublicationResultV1> awaitExportRun(ExportRun& run) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (run.handle.tryTakeResult().has_value()) {
            break;
        }
        std::this_thread::sleep_for(1ms);
    }
    return std::move(*run.result);
}
