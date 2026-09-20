// Support fragment for output_analysis_attempt_runner_tests.cpp. Included inside that file's
// anonymous namespace, after its includes and namespace aliases, before the test fragments. It
// holds only the shared fixture vocabulary the test groups use: the failure collector, a temp
// directory, the closed id set, a trivial one-node plan, the request builder, GPU option builder,
// and the completion pump. No test is defined here.

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

class TempDirectory final {
  public:
    TempDirectory() {
        std::array<char, 64> pattern{};
        constexpr std::string_view prefix = "/tmp/bloom-attempt-runner-XXXXXX";
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

constexpr auto kProjectId = document::ProjectId::fromRaw(0x1001);
constexpr auto kCompositionId = document::CompositionId::fromRaw(0x1002);
constexpr auto kSolidNodeId = document::NodeId::fromRaw(0x1003);
constexpr auto kOutputNodeId = document::NodeId::fromRaw(0x1004);
constexpr auto kColorParameterId = document::ParameterId::fromRaw(0x1005);
constexpr auto kWidthParameterId = document::ParameterId::fromRaw(0x1105);
constexpr auto kHeightParameterId = document::ParameterId::fromRaw(0x1106);
constexpr auto kRevision = document::Revision::fromRaw(0x1006);
constexpr auto kLayerNodeId = document::NodeId::fromRaw(0x1007);
constexpr auto kLayerId = document::LayerId::fromRaw(0x1008);
constexpr auto kStackNodeId = document::NodeId::fromRaw(0x1009);
constexpr auto kSlotId = document::LayerSlotId::fromRaw(0x100a);
constexpr auto kPositionParameterId = document::ParameterId::fromRaw(0x100b);
constexpr auto kOpacityParameterId = document::ParameterId::fromRaw(0x100c);
constexpr auto kAnchorParameterId = document::ParameterId::fromRaw(0x100d);
constexpr auto kScaleParameterId = document::ParameterId::fromRaw(0x100e);
constexpr auto kRotationParameterId = document::ParameterId::fromRaw(0x100f);
constexpr auto kBlendModeParameterId = document::ParameterId::fromRaw(0x1010);

// A trivial one-node (solid -> composition output) plan: a real, directly evaluable composition,
// unlike bloom/output/tests/flat_exr_test_support.hpp's shellPlan() (which evaluates a plan only
// to immediately overwrite the result with a fixture). This test exercises the real
// runtime::CpuCompositionEvaluator through beginOutputAnalysisAttemptV1()'s own Cpu stage.
[[nodiscard]] std::shared_ptr<const runtime::CompiledCompositionPlan> smallSolidPlan() {
    const auto format = document::CompositionFormat::create(2, 2);
    if (!format) {
        std::abort();
    }
    std::vector<runtime::CompiledOperation> operations;
    operations.emplace_back(
        runtime::CompiledSolid{kSolidNodeId,
                               {kColorParameterId, bloom::core::Color4d{0.25, 0.5, 0.75, 1.0}},
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

[[nodiscard]] host::OutputAnalysisAttemptRequestV1
requestFor(const std::filesystem::path& targetPath) {
    const auto plan = smallSolidPlan();
    return {
        .plan = plan,
        .evaluation = {.time = bloom::core::RationalTime::fromInteger(0),
                       .output = plan->output(),
                       .resolution = runtime::CompositionFormatResolution{},
                       .quality = runtime::EvaluationQuality::Reference,
                       .colorIntent = runtime::EvaluationColorIntent::LinearRec709Scene,
                       .pixelStorageByteLimit = 4096},
        .targetPath = targetPath,
        .overwritePolicy = platform::ArtifactOverwritePolicy::CreateOrReplace,
        .owner = {.kind = runtime::TaskOwnerKind::Export, .id = runtime::TaskOwnerId::fromRaw(1)}};
}

[[nodiscard]] runtime::GpuProcessFrameEvaluatorOptions
gpuOptions(const bool enabled, std::filesystem::path loader = {}) {
    runtime::GpuProcessFrameEvaluatorOptions options;
    options.enabled = enabled;
    options.loaderPath = std::move(loader);
    return options;
}

[[nodiscard]] std::optional<host::OutputAnalysisAttemptOutcomeV1>
pumpUntilComplete(host::OutputAnalysisAttemptRunnerV1& runner) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto outcome = runner.tryComplete()) {
            return outcome;
        }
        std::this_thread::sleep_for(1ms);
    }
    return std::nullopt;
}
