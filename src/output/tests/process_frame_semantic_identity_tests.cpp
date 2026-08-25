#include <bloom/core/sha256.hpp>
#include <bloom/output/output_limits.hpp>
#include <bloom/output/process_frame_semantic_identity.hpp>
#include <bloom/render/image.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <source_location>
#include <span>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

namespace core = bloom::core;
namespace document = bloom::document;
namespace output = bloom::output;
namespace render = bloom::render;
namespace runtime = bloom::runtime;

using Identity = output::ProcessFrameSemanticIdentityV1;
using PreparationResult = output::ProcessFrameSemanticIdentityV1PreparationResult;

template <typename Product>
concept ReadsCanonicalBytesFromConstLvalue =
    requires(const Product& product) { product.canonicalBytes(); };

template <typename Product>
concept ReadsCanonicalBytesFromRvalue =
    requires(Product product) { std::move(product).canonicalBytes(); };

template <typename Product>
concept ReadsFrameFromConstLvalue = requires(const Product& product) { product.processFrame(); };

template <typename Product>
concept ReadsFrameFromRvalue = requires(Product product) { std::move(product).processFrame(); };

template <typename Product>
concept ReadsPixelDigestFromConstLvalue =
    requires(const Product& product) { product.processPixelDigest(); };

template <typename Product>
concept ReadsPixelDigestFromRvalue =
    requires(Product product) { std::move(product).processPixelDigest(); };

template <typename Result>
concept ReadsIdentityFromConstLvalue = requires(const Result& result) { result.identity(); };

template <typename Result>
concept ReadsIdentityFromRvalue = requires(Result result) { std::move(result).identity(); };

template <typename Preparer>
concept ChainsIdentityFromTemporaryResult = requires(const Preparer& preparer) {
    preparer.prepare(std::shared_ptr<const runtime::ProcessFrame>{}, runtime::CancellationToken{})
        .identity();
};

static_assert(!std::is_default_constructible_v<Identity>);
static_assert(!std::is_copy_constructible_v<Identity>);
static_assert(!std::is_copy_assignable_v<Identity>);
static_assert(!std::is_move_constructible_v<Identity>);
static_assert(!std::is_move_assignable_v<Identity>);
static_assert(ReadsCanonicalBytesFromConstLvalue<Identity>);
static_assert(!ReadsCanonicalBytesFromRvalue<Identity>);
static_assert(ReadsFrameFromConstLvalue<Identity>);
static_assert(!ReadsFrameFromRvalue<Identity>);
static_assert(ReadsPixelDigestFromConstLvalue<Identity>);
static_assert(!ReadsPixelDigestFromRvalue<Identity>);
static_assert(ReadsIdentityFromConstLvalue<PreparationResult>);
static_assert(!ReadsIdentityFromRvalue<PreparationResult>);
static_assert(!ChainsIdentityFromTemporaryResult<output::ProcessFrameSemanticIdentityV1Preparer>);

constexpr auto kProjectId = document::ProjectId::fromRaw(0x0102030405060708ULL);
constexpr auto kCompositionId = document::CompositionId::fromRaw(0x1112131415161718ULL);
constexpr auto kOutputNodeId = document::NodeId::fromRaw(0x3132333435363738ULL);
constexpr auto kInputNodeId = document::NodeId::fromRaw(0x4142434445464748ULL);
constexpr auto kColorParameterId = document::ParameterId::fromRaw(0x5152535455565758ULL);
constexpr auto kSourceRevision = document::Revision::fromRaw(0x2122232425262728ULL);

constexpr auto kShellLayerNodeId = document::NodeId::fromRaw(0x61);
constexpr auto kShellLayerId = document::LayerId::fromRaw(0x62);
constexpr auto kShellStackNodeId = document::NodeId::fromRaw(0x63);
constexpr auto kShellSlotId = document::LayerSlotId::fromRaw(0x64);
constexpr auto kShellPositionParameterId = document::ParameterId::fromRaw(0x65);
constexpr auto kShellOpacityParameterId = document::ParameterId::fromRaw(0x66);

constexpr std::string_view kCompositionPixelDigest =
    "db7f7d3db2e78643177715f34b8dcca6fd50399509094b9da0a9873016ba5ac9";
constexpr std::string_view kCompositionIdentityDigest =
    "9290563ea1c3249a146151519b5031ad7bf80a727cd08427f286d1d0f267958d";
constexpr std::string_view kCompositionIdentityHex =
    "426c6f6f6d50726f636573734672616d6553656d616e7469634964656e7469747900000101020304"
    "0506070811121314151617182122232425262728fffffffffffffffd00000000000000043132333435"
    "36373801fffffffffffffffe00000000000000050000000200000002fffffffffffffff6ffffffff"
    "ffffffec00000002000000020000000400000003000000106c696e5f7265633730395f7363656e65"
    "01010100000021626c6f6f6d2e70726f636573732e726762613332662e73656d616e7469632e7632"
    "0000000700000008000000090000000adb7f7d3db2e78643177715f34b8dcca6fd50399509094b9d"
    "a0a9873016ba5ac9";

constexpr std::string_view kProxyPixelDigest =
    "cf2e8263f93e403ebdaed5b6e0ed25f2a11766ff3001e17067334c46cade71d5";
constexpr std::string_view kProxyIdentityDigest =
    "793b01a3fb5c7186b4593f650d049149fd2ebb0954332bcf229a8394a3b2edea";
constexpr std::string_view kProxyIdentityHex =
    "426c6f6f6d50726f636573734672616d6553656d616e7469634964656e7469747900000101020304"
    "0506070811121314151617182122232425262728fffffffffffffffd00000000000000043132333435"
    "3637380200000003000000010000000000000064ffffffffffffff380000000300000001ffffffff"
    "ffffff9c00000000000000c800000003000000010000000400000009000000106c696e5f72656337"
    "30395f7363656e6501010100000021626c6f6f6d2e70726f636573732e726762613332662e73656d"
    "616e7469632e76320000000700000008000000090000000acf2e8263f93e403ebdaed5b6e0ed25f2"
    "a11766ff3001e17067334c46cade71d5";

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

[[nodiscard]] std::uint8_t hexDigit(const char character) {
    if (character >= '0' && character <= '9') {
        return static_cast<std::uint8_t>(character - '0');
    }
    if (character >= 'a' && character <= 'f') {
        return static_cast<std::uint8_t>(character - 'a' + 10);
    }
    std::abort();
}

[[nodiscard]] bool matchesHex(const std::span<const std::byte> bytes,
                              const std::string_view expected) {
    if (expected.size() != bytes.size() * 2U) {
        return false;
    }
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        const auto value = static_cast<std::uint8_t>((hexDigit(expected[index * 2U]) << 4U) |
                                                     hexDigit(expected[index * 2U + 1U]));
        if (bytes[index] != static_cast<std::byte>(value)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool hasDigest(const core::Sha256Digest& digest, const std::string_view expected) {
    const auto hex = digest.toLowercaseHex();
    return std::string_view(hex.data(), hex.size()) == expected;
}

[[nodiscard]] bool hasDigest(const std::optional<core::Sha256Digest>& digest,
                             const std::string_view expected) {
    return digest.has_value() && hasDigest(*digest, expected);
}

[[nodiscard]] render::Rgba32f pixel(const float red, const float green, const float blue,
                                    const float alpha) {
    const auto value = render::Rgba32f::fromPremultiplied(red, green, blue, alpha);
    if (!value) {
        std::abort();
    }
    return *value.value();
}

[[nodiscard]] render::ImageWindow window(const std::int64_t originX, const std::int64_t originY,
                                         const std::uint32_t width, const std::uint32_t height) {
    const auto value = render::ImageWindow::create(originX, originY, width, height);
    if (!value) {
        std::abort();
    }
    return *value.value();
}

[[nodiscard]] render::Rgba32fImage image(const render::ImageWindow dataWindow,
                                         const render::ImageWindow displayWindow,
                                         const core::PixelAspectRatio pixelAspect,
                                         const std::span<const render::Rgba32f> pixels) {
    const auto descriptor =
        render::Rgba32fImageDescriptor::create(dataWindow, displayWindow, pixelAspect);
    if (!descriptor || descriptor.value()->layout().pixelCount != pixels.size()) {
        std::abort();
    }
    auto builder = render::Rgba32fImageBuilder::create(
        *descriptor.value(), descriptor.value()->layout().pixelStorageBytes);
    if (!builder) {
        std::abort();
    }
    for (std::uint32_t rowIndex = 0; rowIndex < dataWindow.extent().height(); ++rowIndex) {
        const auto y = dataWindow.originY() + static_cast<std::int64_t>(rowIndex);
        auto row = builder.value()->row(y);
        if (!row) {
            std::abort();
        }
        const auto offset = static_cast<std::size_t>(rowIndex) * dataWindow.extent().width();
        std::ranges::copy(pixels.subspan(offset, dataWindow.extent().width()),
                          row.value()->begin());
    }
    auto frozen = std::move(*builder.value()).freeze();
    if (!frozen) {
        std::abort();
    }
    return std::move(*frozen.value());
}

[[nodiscard]] std::shared_ptr<const runtime::CompiledCompositionPlan>
plan(const std::uint32_t width = 2, const std::uint32_t height = 2,
     const core::PixelAspectRatio pixelAspect = core::PixelAspectRatio::square()) {
    const auto format = document::CompositionFormat::create(width, height, pixelAspect);
    if (!format) {
        std::abort();
    }
    std::vector<runtime::CompiledOperation> operations;
    operations.emplace_back(runtime::CompiledSolid{kInputNodeId, kColorParameterId, {}});
    operations.emplace_back(
        runtime::CompiledCompositionOutput{kOutputNodeId, runtime::OperationIndex::fromRaw(0)});
    return std::make_shared<const runtime::CompiledCompositionPlan>(
        runtime::CompiledCompositionPlanDefinition{.sourceRevision = kSourceRevision,
                                                   .projectId = kProjectId,
                                                   .compositionId = kCompositionId,
                                                   .format = *format,
                                                   .operations = std::move(operations),
                                                   .output = runtime::OperationIndex::fromRaw(1),
                                                   .scalarCurves = {},
                                                   .vec2Curves = {},
                                                   .planSemanticsVersion = 7,
                                                   .animationSamplingSemanticsVersion = 8});
}

[[nodiscard]] runtime::ProcessFrameIdentity
frameIdentity(const std::shared_ptr<const runtime::CompiledCompositionPlan>& compiledPlan,
              runtime::EvaluationResolution resolution = runtime::CompositionFormatResolution{}) {
    const auto time = core::RationalTime::create(-6, 8);
    if (!time) {
        std::abort();
    }
    return {.plan = compiledPlan,
            .time = *time,
            .output = runtime::OperationIndex::fromRaw(1),
            .resolution = resolution,
            .quality = runtime::EvaluationQuality::Reference,
            .colorIntent = runtime::EvaluationColorIntent::LinearRec709Scene,
            .provider = runtime::EvaluationProvider::CpuReference,
            .evaluatorSemanticsVersion = 9,
            .animationSamplingSemanticsVersion = 8,
            .imagePrimitiveSemanticsVersion = 10};
}

struct Fixture final {
    runtime::ProcessFrameIdentity identity;
    render::Rgba32fImage processImage;
};

[[nodiscard]] Fixture compositionFixture() {
    const auto pixelAspect = core::PixelAspectRatio::create(4, 3);
    if (!pixelAspect) {
        std::abort();
    }
    const std::array pixels{pixel(-0.0F, 0.0F, -1.5F, 1.0F), pixel(2.0F, 0.5F, 0.25F, 0.5F),
                            pixel(-3.25F, 4.5F, -0.125F, 0.75F), render::Rgba32f::transparent()};
    return {frameIdentity(plan(2, 2, *pixelAspect)),
            image(window(-2, 5, 2, 2), window(-10, -20, 2, 2), *pixelAspect, pixels)};
}

[[nodiscard]] Fixture proxyFixture() {
    const auto formatAspect = core::PixelAspectRatio::create(4, 3);
    const auto proxyAspect = core::PixelAspectRatio::create(4, 9);
    const auto proxyExtent = render::ImageExtent::create(3, 1);
    if (!formatAspect || !proxyAspect || !proxyExtent) {
        std::abort();
    }
    const std::array pixels{pixel(1.0F, 2.0F, 3.0F, 1.0F), pixel(-0.0F, 0.0F, 0.5F, 0.5F),
                            pixel(8.0F, -4.0F, 2.0F, 1.0F)};
    return {
        frameIdentity(plan(2, 2, *formatAspect), runtime::ProxyResolution{*proxyExtent.value()}),
        image(window(100, -200, 3, 1), window(-100, 200, 3, 1), *proxyAspect, pixels)};
}

[[nodiscard]] std::shared_ptr<const runtime::CompiledCompositionPlan>
shellPlan(const std::uint32_t width, const std::uint32_t height) {
    const auto format = document::CompositionFormat::create(width, height);
    if (!format) {
        std::abort();
    }
    std::vector<runtime::CompiledOperation> operations;
    operations.emplace_back(
        runtime::CompiledSolid{kInputNodeId, kColorParameterId, {0.0, 0.0, 0.0, 0.0}});
    operations.emplace_back(runtime::CompiledLayerOutput{
        kShellLayerNodeId, kShellLayerId, runtime::OperationIndex::fromRaw(0),
        runtime::CompiledVec2Parameter{
            kShellPositionParameterId,
            document::Vec2d{static_cast<double>(width) / 2.0, static_cast<double>(height) / 2.0}},
        runtime::CompiledScalarParameter{kShellOpacityParameterId, 1.0}});
    operations.emplace_back(runtime::CompiledLayerStack{
        kShellStackNodeId, {{kShellSlotId, kShellLayerId, runtime::OperationIndex::fromRaw(1)}}});
    operations.emplace_back(
        runtime::CompiledCompositionOutput{kOutputNodeId, runtime::OperationIndex::fromRaw(2)});
    return std::make_shared<const runtime::CompiledCompositionPlan>(
        runtime::CompiledCompositionPlanDefinition{.sourceRevision = kSourceRevision,
                                                   .projectId = kProjectId,
                                                   .compositionId = kCompositionId,
                                                   .format = *format,
                                                   .operations = std::move(operations),
                                                   .output = runtime::OperationIndex::fromRaw(3)});
}

[[nodiscard]] std::shared_ptr<const runtime::ProcessFrame>
evaluateShell(const std::uint32_t width, const std::uint32_t height) {
    const auto compiledPlan = shellPlan(width, height);
    const runtime::EvaluationRequest request{
        .time = core::RationalTime::fromInteger(0),
        .output = compiledPlan->output(),
        .resolution = runtime::CompositionFormatResolution{},
        .quality = runtime::EvaluationQuality::Reference,
        .colorIntent = runtime::EvaluationColorIntent::LinearRec709Scene,
        .pixelStorageByteLimit = static_cast<std::size_t>(width) * height * 64U};
    const runtime::CpuCompositionEvaluator evaluator;
    const auto result = evaluator.evaluate(compiledPlan, request, {});
    if (result.status() != runtime::EvaluationStatus::Evaluated || result.frame() == nullptr) {
        std::abort();
    }
    return result.frame();
}

[[nodiscard]] std::shared_ptr<const runtime::ProcessFrame> publishFixture(Fixture fixture) {
    auto published = std::const_pointer_cast<runtime::ProcessFrame>(evaluateShell(1, 1));
    auto& identity = const_cast<runtime::ProcessFrameIdentity&>(published->identity());
    auto& processImage = const_cast<render::Rgba32fImage&>(published->processImage());
    identity = std::move(fixture.identity);
    processImage = std::move(fixture.processImage);
    return published;
}

[[nodiscard]] PreparationResult
prepare(const std::shared_ptr<const runtime::ProcessFrame>& frame,
        const output::ProcessFrameSemanticIdentityProgressCallback& progress = {}) {
    const output::ProcessFrameSemanticIdentityV1Preparer preparer;
    return preparer.prepare(frame, {}, progress);
}

[[nodiscard]] std::shared_ptr<const Identity>
preparedIdentity(const std::shared_ptr<const runtime::ProcessFrame>& frame) {
    const auto result = prepare(frame);
    if (result.status() != output::ProcessFrameSemanticIdentityPreparationStatus::Prepared ||
        result.identity() == nullptr) {
        std::abort();
    }
    return result.identity();
}

void testGoldenVectorsAndLifetime(Expectations& expectations) {
    auto compositionFrame = publishFixture(compositionFixture());
    const auto* exactFrame = compositionFrame.get();
    auto compositionResult = prepare(compositionFrame);
    expectations.expect(compositionResult.status() ==
                                output::ProcessFrameSemanticIdentityPreparationStatus::Prepared &&
                            compositionResult.error() ==
                                output::ProcessFrameSemanticIdentityErrorCode::None &&
                            compositionResult.identity() != nullptr,
                        "composition preparation publishes one successful immutable product");
    if (compositionResult.identity() != nullptr) {
        const auto& identity = *compositionResult.identity();
        expectations.expect(
            identity.processFrame().get() == exactFrame &&
                identity.canonicalBytes().size() ==
                    output::kCompositionProcessFrameSemanticIdentityV1Bytes &&
                hasDigest(identity.processPixelDigest(), kCompositionPixelDigest),
            "composition product binds the exact frame, byte count, and pixel digest");
        expectations.expect(matchesHex(identity.canonicalBytes(), kCompositionIdentityHex),
                            "composition product matches the independent canonical byte vector");
        expectations.expect(hasDigest(core::Sha256Hasher::hash(identity.canonicalBytes()),
                                      kCompositionIdentityDigest),
                            "composition product matches the independent record digest");
    }

    auto retained = compositionResult.identity();
    compositionFrame.reset();
    compositionResult = prepare(nullptr);
    expectations.expect(retained != nullptr && retained->processFrame().get() == exactFrame,
                        "the product owns the exact process frame after all caller ownership ends");

    const auto proxyResult = prepare(publishFixture(proxyFixture()));
    expectations.expect(
        proxyResult.status() == output::ProcessFrameSemanticIdentityPreparationStatus::Prepared &&
            proxyResult.identity() != nullptr &&
            proxyResult.identity()->canonicalBytes().size() ==
                output::kProxyProcessFrameSemanticIdentityV1Bytes &&
            hasDigest(proxyResult.identity()->processPixelDigest(), kProxyPixelDigest) &&
            matchesHex(proxyResult.identity()->canonicalBytes(), kProxyIdentityHex) &&
            hasDigest(core::Sha256Hasher::hash(proxyResult.identity()->canonicalBytes()),
                      kProxyIdentityDigest),
        "proxy product matches independent size, pixel, byte, and record-digest vectors");
}

void testProgressAndRepeatability(Expectations& expectations) {
    const auto frame = publishFixture(compositionFixture());
    std::vector<output::ProcessFrameSemanticIdentityProgress> progress;
    auto first = prepare(frame, [&progress](const auto update) { progress.push_back(update); });
    const auto second = prepare(frame, [](const auto&) { throw 7; });
    bool monotonic = !progress.empty();
    for (std::size_t index = 1; index < progress.size(); ++index) {
        const auto previousStage = static_cast<std::uint8_t>(progress[index - 1].stage);
        const auto stage = static_cast<std::uint8_t>(progress[index].stage);
        monotonic = monotonic && stage >= previousStage &&
                    progress[index].completed <= progress[index].total;
        if (stage == previousStage) {
            monotonic = monotonic && progress[index].completed >= progress[index - 1].completed &&
                        progress[index].total == progress[index - 1].total;
        }
    }
    expectations.expect(
        first.identity() != nullptr && second.identity() != nullptr && progress.size() == 7U &&
            progress.front() ==
                output::ProcessFrameSemanticIdentityProgress{
                    output::ProcessFrameSemanticIdentityProgressStage::Preflight, 0, 1} &&
            progress.back() ==
                output::ProcessFrameSemanticIdentityProgress{
                    output::ProcessFrameSemanticIdentityProgressStage::Encoding, 1, 1} &&
            monotonic,
        "progress is monotonic across exact preflight, row hashing, and encoding stages");
    expectations.expect(
        first.identity() != nullptr && second.identity() != nullptr &&
            std::ranges::equal(first.identity()->canonicalBytes(),
                               second.identity()->canonicalBytes()) &&
            first.identity()->processPixelDigest() == second.identity()->processPixelDigest(),
        "repeat preparation and throwing monitoring callbacks do not change identity");

    // This deliberately constructs from a non-const rvalue. PreparationResult suppresses a
    // consuming move, so construction must copy and leave the source publication coherent.
    // NOLINTBEGIN(bugprone-use-after-move)
    // NOLINTNEXTLINE(performance-move-const-arg)
    const auto rvalueCopy = std::move(first);
    expectations.expect(
        first.status() == output::ProcessFrameSemanticIdentityPreparationStatus::Prepared &&
            first.identity() != nullptr &&
            rvalueCopy.status() ==
                output::ProcessFrameSemanticIdentityPreparationStatus::Prepared &&
            rvalueCopy.identity() == first.identity(),
        "rvalue copying a publication result cannot create an incoherent moved-from result");
    // NOLINTEND(bugprone-use-after-move)
}

void testPixelBitsAndPreflightPrecedence(Expectations& expectations) {
    auto originalFixture = compositionFixture();
    const auto aspect = originalFixture.processImage.descriptor()->pixelAspect();
    std::array reversed{render::Rgba32f::transparent(), render::Rgba32f::transparent(),
                        render::Rgba32f::transparent(), render::Rgba32f::transparent()};
    std::ranges::reverse_copy(originalFixture.processImage.pixels(), reversed.begin());
    auto reorderedFixture = compositionFixture();
    reorderedFixture.processImage =
        image(window(-2, 5, 2, 2), window(-10, -20, 2, 2), aspect, reversed);
    const auto original = preparedIdentity(publishFixture(std::move(originalFixture)));
    const auto reordered = preparedIdentity(publishFixture(std::move(reorderedFixture)));
    expectations.expect(original->processPixelDigest() != reordered->processPixelDigest(),
                        "changing increasing-Y/X pixel order changes the bounded pixel digest");

    const std::array positivePixels{pixel(0.0F, 0.0F, 0.0F, 1.0F)};
    const std::array negativePixels{pixel(-0.0F, 0.0F, 0.0F, 1.0F)};
    const auto onePixelPlan = plan(1, 1);
    auto positive = preparedIdentity(publishFixture(
        {frameIdentity(onePixelPlan), image(window(0, 0, 1, 1), window(0, 0, 1, 1),
                                            core::PixelAspectRatio::square(), positivePixels)}));
    auto negative = preparedIdentity(publishFixture(
        {frameIdentity(onePixelPlan), image(window(0, 0, 1, 1), window(0, 0, 1, 1),
                                            core::PixelAspectRatio::square(), negativePixels)}));
    expectations.expect(positive->processPixelDigest() != negative->processPixelDigest(),
                        "positive and negative binary32 zero remain distinct process bits");

    auto negativeTransparentRgb = std::const_pointer_cast<runtime::ProcessFrame>(publishFixture(
        {frameIdentity(onePixelPlan),
         image(window(0, 0, 1, 1), window(0, 0, 1, 1), core::PixelAspectRatio::square(),
               std::array{render::Rgba32f::transparent()})}));
    auto negativeTransparentRgbPixels = negativeTransparentRgb->processImage().pixels();
    const_cast<render::Rgba32f&>(negativeTransparentRgbPixels.front()) =
        std::bit_cast<render::Rgba32f>(std::array<float, 4>{-0.0F, 0.0F, 0.0F, 0.0F});
    expectations.expect(
        prepare(negativeTransparentRgb).error() ==
            output::ProcessFrameSemanticIdentityErrorCode::InvalidPixel,
        "transparent RGB signed zero is rejected instead of claiming canonical positive zero");

    auto negativeTransparentAlpha = std::const_pointer_cast<runtime::ProcessFrame>(publishFixture(
        {frameIdentity(onePixelPlan),
         image(window(0, 0, 1, 1), window(0, 0, 1, 1), core::PixelAspectRatio::square(),
               std::array{render::Rgba32f::transparent()})}));
    auto negativeTransparentAlphaPixels = negativeTransparentAlpha->processImage().pixels();
    const_cast<render::Rgba32f&>(negativeTransparentAlphaPixels.front()) =
        std::bit_cast<render::Rgba32f>(std::array<float, 4>{0.0F, 0.0F, 0.0F, -0.0F});
    expectations.expect(
        prepare(negativeTransparentAlpha).error() ==
            output::ProcessFrameSemanticIdentityErrorCode::InvalidPixel,
        "negative-zero alpha is rejected from the canonical transparent representation");

    auto invalidFrame =
        std::const_pointer_cast<runtime::ProcessFrame>(publishFixture(compositionFixture()));
    auto& invalidIdentity = const_cast<runtime::ProcessFrameIdentity&>(invalidFrame->identity());
    invalidIdentity.plan.reset();
    auto pixels = invalidFrame->processImage().pixels();
    const auto invalidPixel = std::bit_cast<render::Rgba32f>(
        std::array<float, 4>{std::numeric_limits<float>::quiet_NaN(), 0.0F, 0.0F, 1.0F});
    const_cast<render::Rgba32f&>(pixels.front()) = invalidPixel;
    const auto invalid = prepare(invalidFrame);
    expectations.expect(
        invalid.status() == output::ProcessFrameSemanticIdentityPreparationStatus::Failed &&
            invalid.identity() == nullptr &&
            invalid.error() == output::ProcessFrameSemanticIdentityErrorCode::MissingPlan,
        "complete cheap preflight fails before reading an invalid pixel");
}

void testBoundsMalformedAndCancelledPublication(Expectations& expectations) {
    const auto oversizedPlan = plan(output::kOutputAnalysisMaximumDimensionV1 + 1U, 1);
    const std::array onePixel{render::Rgba32f::transparent()};
    auto oversized = publishFixture(
        {frameIdentity(oversizedPlan),
         image(window(0, 0, 1, 1), window(0, 0, output::kOutputAnalysisMaximumDimensionV1 + 1U, 1),
               core::PixelAspectRatio::square(), onePixel)});
    const auto limited = prepare(oversized);
    expectations.expect(
        limited.status() == output::ProcessFrameSemanticIdentityPreparationStatus::Failed &&
            limited.identity() == nullptr &&
            limited.error() == output::ProcessFrameSemanticIdentityErrorCode::ResourceLimitExceeded,
        "output bounds are rejected before pixel hashing or product allocation");

    auto movedFrame =
        std::const_pointer_cast<runtime::ProcessFrame>(publishFixture(compositionFixture()));
    auto& movedImage = const_cast<render::Rgba32fImage&>(movedFrame->processImage());
    auto retainedImage = std::move(movedImage);
    const auto malformed = prepare(movedFrame);
    expectations.expect(
        retainedImage.isValid() &&
            malformed.status() == output::ProcessFrameSemanticIdentityPreparationStatus::Failed &&
            malformed.identity() == nullptr &&
            malformed.error() == output::ProcessFrameSemanticIdentityErrorCode::InvalidImage,
        "a moved-from image fails coherently and publishes no partial identity");

    const auto missing = prepare(nullptr);
    expectations.expect(
        missing.status() == output::ProcessFrameSemanticIdentityPreparationStatus::Failed &&
            missing.identity() == nullptr &&
            missing.error() == output::ProcessFrameSemanticIdentityErrorCode::MissingFrame,
        "a missing frame has an explicit failed result with no product");

    const auto largeFrame = evaluateShell(64, 64);
    runtime::TaskSchedulerConfig config = runtime::TaskSchedulerConfig::defaults();
    config.cpuWorkerCount = 1;
    config.blockingIoWorkerCount = 1;
    runtime::TaskScheduler scheduler(config);
    std::mutex mutex;
    std::condition_variable condition;
    bool reachedRow = false;
    bool released = false;
    std::atomic_bool cancelled = false;
    std::atomic_bool published = false;
    const output::ProcessFrameSemanticIdentityV1Preparer preparer;
    auto submission = scheduler.submit<void>(
        runtime::TaskRequest(
            "Semantic identity cancellation",
            {.kind = runtime::TaskOwnerKind::Composition, .id = runtime::TaskOwnerId::fromRaw(1)}),
        [largeFrame, &preparer, &mutex, &condition, &reachedRow, &released, &cancelled,
         &published](runtime::TaskContext& context) {
            const auto result = preparer.prepare(
                largeFrame, context.cancellation(),
                [&mutex, &condition, &reachedRow,
                 &released](const output::ProcessFrameSemanticIdentityProgress& update) {
                    if (update.stage !=
                            output::ProcessFrameSemanticIdentityProgressStage::HashingPixels ||
                        update.completed != 64U) {
                        return;
                    }
                    std::unique_lock lock(mutex);
                    reachedRow = true;
                    condition.notify_all();
                    condition.wait(lock, [&released] { return released; });
                });
            cancelled.store(result.status() ==
                                output::ProcessFrameSemanticIdentityPreparationStatus::Cancelled,
                            std::memory_order_release);
            published.store(result.identity() != nullptr, std::memory_order_release);
            return result.status() ==
                           output::ProcessFrameSemanticIdentityPreparationStatus::Cancelled
                       ? runtime::TaskResult<void>::cancelled()
                       : runtime::TaskResult<void>::succeeded();
        });
    {
        std::unique_lock lock(mutex);
        expectations.expect(submission.accepted() &&
                                condition.wait_for(lock, 2s, [&reachedRow] { return reachedRow; }),
                            "cancellation fixture reaches a deterministic scanline boundary");
    }
    submission.handle.cancel();
    {
        std::lock_guard lock(mutex);
        released = true;
        condition.notify_all();
    }
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    std::optional<runtime::TaskResult<void>> taskResult;
    while (!taskResult && std::chrono::steady_clock::now() < deadline) {
        taskResult = submission.handle.tryTakeResult();
        std::this_thread::yield();
    }
    expectations.expect(taskResult && taskResult->state() == runtime::TaskState::Cancelled &&
                            cancelled.load(std::memory_order_acquire) &&
                            !published.load(std::memory_order_acquire),
                        "cancellation between rows publishes no partial identity product");
    scheduler.beginShutdown();
    while (!scheduler.isQuiescent() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    expectations.expect(scheduler.isQuiescent(), "cancellation fixture shuts down cleanly");
}

void testClosedSemanticValidationAndProviderNeutrality(Expectations& expectations) {
    auto invalidQuality = compositionFixture();
    invalidQuality.identity.quality = std::bit_cast<runtime::EvaluationQuality>(std::uint8_t{0xFF});
    expectations.expect(
        prepare(publishFixture(std::move(invalidQuality))).error() ==
            output::ProcessFrameSemanticIdentityErrorCode::UnsupportedEvaluationQuality,
        "unknown evaluation-quality values fail closed during preflight");

    auto invalidColor = compositionFixture();
    invalidColor.identity.colorIntent =
        std::bit_cast<runtime::EvaluationColorIntent>(std::uint8_t{0xFF});
    expectations.expect(prepare(publishFixture(std::move(invalidColor))).error() ==
                            output::ProcessFrameSemanticIdentityErrorCode::UnsupportedColorIntent,
                        "unknown color-intent values fail closed during preflight");

    auto invalidOutput = compositionFixture();
    invalidOutput.identity.output = runtime::OperationIndex::fromRaw(0);
    expectations.expect(prepare(publishFixture(std::move(invalidOutput))).error() ==
                            output::ProcessFrameSemanticIdentityErrorCode::InvalidOutput,
                        "a non-terminal selected operation is rejected");

    auto invalidSemantics = compositionFixture();
    invalidSemantics.identity.evaluatorSemanticsVersion = 0;
    expectations.expect(prepare(publishFixture(std::move(invalidSemantics))).error() ==
                            output::ProcessFrameSemanticIdentityErrorCode::InvalidSemanticsVersion,
                        "zero semantic versions are rejected before hashing");

    const auto baseline = preparedIdentity(publishFixture(compositionFixture()));
    auto providerNeutral = compositionFixture();
    providerNeutral.identity.provider =
        std::bit_cast<runtime::EvaluationProvider>(std::uint8_t{0xFF});
    const auto changedProvider = preparedIdentity(publishFixture(std::move(providerNeutral)));
    expectations.expect(
        std::ranges::equal(baseline->canonicalBytes(), changedProvider->canonicalBytes()),
        "execution-provider provenance remains outside portable semantic bytes");
}

} // namespace

int main() {
    Expectations expectations;
    testGoldenVectorsAndLifetime(expectations);
    testProgressAndRepeatability(expectations);
    testPixelBitsAndPreflightPrecedence(expectations);
    testBoundsMalformedAndCancelledPublication(expectations);
    testClosedSemanticValidationAndProviderNeutrality(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
