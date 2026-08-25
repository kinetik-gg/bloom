#include <bloom/core/sha256.hpp>
#include <bloom/output/process_frame_semantic_identity.hpp>
#include <bloom/render/cpu_image_primitives.hpp>
#include <bloom/runtime/compiled_plan.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <source_location>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace core = bloom::core;
namespace document = bloom::document;
namespace output = bloom::output;
namespace render = bloom::render;
namespace runtime = bloom::runtime;

constexpr auto kProjectId = document::ProjectId::fromRaw(0x0102030405060708ULL);
constexpr auto kCompositionId = document::CompositionId::fromRaw(0x1112131415161718ULL);
constexpr auto kOutputNodeId = document::NodeId::fromRaw(0x3132333435363738ULL);
constexpr auto kInputNodeId = document::NodeId::fromRaw(0x4142434445464748ULL);
constexpr auto kColorParameterId = document::ParameterId::fromRaw(0x5152535455565758ULL);
constexpr auto kSourceRevision = document::Revision::fromRaw(0x2122232425262728ULL);

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
    if (expected.size() != bytes.size() * 2) {
        return false;
    }
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        const auto value = static_cast<std::uint8_t>((hexDigit(expected[index * 2]) << 4U) |
                                                     hexDigit(expected[index * 2 + 1]));
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

template <typename Result>
[[nodiscard]] bool hasError(const Result& result,
                            const output::ProcessFrameSemanticIdentityErrorCode code) {
    return result.error() == code;
}

template <typename Enum> [[nodiscard]] Enum enumWithBits(const std::uint8_t bits) noexcept {
    static_assert(sizeof(Enum) == sizeof(bits));
    Enum value{};
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

[[nodiscard]] render::Rgba32f pixel(const float red, const float green, const float blue,
                                    const float alpha) {
    const auto result = render::Rgba32f::fromPremultiplied(red, green, blue, alpha);
    if (!result) {
        std::abort();
    }
    return *result.value();
}

[[nodiscard]] render::ImageWindow window(const std::int64_t originX, const std::int64_t originY,
                                         const std::uint32_t width, const std::uint32_t height) {
    const auto result = render::ImageWindow::create(originX, originY, width, height);
    if (!result) {
        std::abort();
    }
    return *result.value();
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
plan(const core::PixelAspectRatio pixelAspect = core::PixelAspectRatio::square()) {
    const auto format = document::CompositionFormat::create(2, 2, pixelAspect);
    if (!format.has_value()) {
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

[[nodiscard]] std::shared_ptr<const runtime::CompiledCompositionPlan>
publishPlan(runtime::CompiledCompositionPlanDefinition definition) {
    return std::make_shared<const runtime::CompiledCompositionPlan>(std::move(definition));
}

[[nodiscard]] runtime::ProcessFrameIdentity
identity(const std::shared_ptr<const runtime::CompiledCompositionPlan>& compiledPlan,
         runtime::EvaluationResolution resolution = runtime::CompositionFormatResolution{}) {
    const auto time = core::RationalTime::create(-6, 8);
    if (!time.has_value()) {
        std::abort();
    }
    return runtime::ProcessFrameIdentity{
        .plan = compiledPlan,
        .time = *time,
        .output = runtime::OperationIndex::fromRaw(1),
        .resolution = resolution,
        .quality = runtime::EvaluationQuality::Reference,
        .colorIntent = runtime::EvaluationColorIntent::LinearRec709Scene,
        .provider = runtime::EvaluationProvider::CpuReference,
        .evaluatorSemanticsVersion = 9,
        .animationSamplingSemanticsVersion = 8,
        .imagePrimitiveSemanticsVersion = 10,
    };
}

struct Fixture final {
    runtime::ProcessFrameIdentity frameIdentity;
    render::Rgba32fImage processImage;
};

[[nodiscard]] Fixture compositionFixture() {
    const auto pixelAspect = core::PixelAspectRatio::create(4, 3);
    if (!pixelAspect.has_value()) {
        std::abort();
    }
    const std::array pixels{
        pixel(-0.0F, 0.0F, -1.5F, 1.0F),
        pixel(2.0F, 0.5F, 0.25F, 0.5F),
        pixel(-3.25F, 4.5F, -0.125F, 0.75F),
        render::Rgba32f::transparent(),
    };
    return {identity(plan(*pixelAspect)),
            image(window(-2, 5, 2, 2), window(-10, -20, 2, 2), *pixelAspect, pixels)};
}

[[nodiscard]] Fixture proxyFixture() {
    const auto formatAspect = core::PixelAspectRatio::create(4, 3);
    const auto proxyAspect = core::PixelAspectRatio::create(4, 9);
    const auto proxyExtent = render::ImageExtent::create(3, 1);
    if (!formatAspect.has_value() || !proxyAspect.has_value() || !proxyExtent) {
        std::abort();
    }
    const std::array pixels{
        pixel(1.0F, 2.0F, 3.0F, 1.0F),
        pixel(-0.0F, 0.0F, 0.5F, 0.5F),
        pixel(8.0F, -4.0F, 2.0F, 1.0F),
    };
    return {identity(plan(*formatAspect), runtime::ProxyResolution{*proxyExtent.value()}),
            image(window(100, -200, 3, 1), window(-100, 200, 3, 1), *proxyAspect, pixels)};
}

void testCompositionGoldenVector(Expectations& expectations) {
    auto fixture = compositionFixture();
    const auto validation =
        output::validateProcessFrameSemanticIdentityV1(fixture.frameIdentity, fixture.processImage);
    expectations.expect(validation &&
                            validation.requiredBytes() ==
                                output::kCompositionProcessFrameSemanticIdentityV1Bytes &&
                            hasDigest(validation.processPixelDigest(), kCompositionPixelDigest),
                        "composition validation returns the exact size and pixel digest");

    std::array<std::byte, output::kCompositionProcessFrameSemanticIdentityV1Bytes> bytes{};
    const auto written = output::writeProcessFrameSemanticIdentityV1(fixture.frameIdentity,
                                                                     fixture.processImage, bytes);
    expectations.expect(written && written.writtenBytes() == bytes.size() &&
                            hasDigest(written.processPixelDigest(), kCompositionPixelDigest),
                        "composition identity writes the complete record");
    expectations.expect(matchesHex(bytes, kCompositionIdentityHex),
                        "composition identity matches the independent hardcoded byte vector");
    expectations.expect(hasDigest(core::Sha256Hasher::hash(bytes), kCompositionIdentityDigest),
                        "composition identity matches the independent hardcoded record digest");
}

void testProxyGoldenVector(Expectations& expectations) {
    auto fixture = proxyFixture();
    std::array<std::byte, output::kProxyProcessFrameSemanticIdentityV1Bytes> bytes{};
    const auto written = output::writeProcessFrameSemanticIdentityV1(fixture.frameIdentity,
                                                                     fixture.processImage, bytes);
    expectations.expect(written && written.requiredBytes() == bytes.size() &&
                            hasDigest(written.processPixelDigest(), kProxyPixelDigest),
                        "proxy identity writes the proxy discriminator, extent, and digest");
    expectations.expect(matchesHex(bytes, kProxyIdentityHex),
                        "proxy identity matches the independent hardcoded byte vector");
    expectations.expect(hasDigest(core::Sha256Hasher::hash(bytes), kProxyIdentityDigest),
                        "proxy identity matches the independent hardcoded record digest");
}

void testPixelBitsOriginAndOrder(Expectations& expectations) {
    auto fixture = compositionFixture();
    const auto original = output::hashProcessPixelStreamV1(fixture.processImage);
    expectations.expect(original && hasDigest(original.digest(), kCompositionPixelDigest),
                        "component bits stream in RGBA order, including signed zero and HDR");

    const auto pixelAspect = fixture.processImage.descriptor()->pixelAspect();
    std::array reversed{render::Rgba32f::transparent(), render::Rgba32f::transparent(),
                        render::Rgba32f::transparent(), render::Rgba32f::transparent()};
    std::ranges::reverse_copy(fixture.processImage.pixels(), reversed.begin());
    auto reordered = image(window(-2, 5, 2, 2), window(-10, -20, 2, 2), pixelAspect, reversed);
    const auto reorderedDigest = output::hashProcessPixelStreamV1(reordered);
    expectations.expect(reorderedDigest && reorderedDigest.digest() != original.digest(),
                        "changing increasing-Y/X pixel order changes the stream digest");

    auto translated = image(window(std::numeric_limits<std::int64_t>::min(), -1, 2, 2),
                            window(std::numeric_limits<std::int64_t>::max() - 2, -20, 2, 2),
                            pixelAspect, fixture.processImage.pixels());
    const auto translatedDigest = output::hashProcessPixelStreamV1(translated);
    std::array<std::byte, output::kCompositionProcessFrameSemanticIdentityV1Bytes>
        translatedIdentity{};
    const auto translatedWrite = output::writeProcessFrameSemanticIdentityV1(
        fixture.frameIdentity, translated, translatedIdentity);
    expectations.expect(translatedDigest && translatedDigest.digest() == original.digest(),
                        "signed window origins do not enter the process pixel stream");
    expectations.expect(translatedWrite && matchesHex(std::span(translatedIdentity).first(34),
                                                      kCompositionIdentityHex.substr(0, 68)),
                        "hostile signed origins remain encodable without changing the domain");

    const std::array positiveZero{pixel(0.0F, 0.0F, 0.0F, 1.0F)};
    const std::array negativeZero{pixel(-0.0F, 0.0F, 0.0F, 1.0F)};
    auto positiveImage = image(window(0, 0, 1, 1), window(0, 0, 1, 1),
                               core::PixelAspectRatio::square(), positiveZero);
    auto negativeImage = image(window(0, 0, 1, 1), window(0, 0, 1, 1),
                               core::PixelAspectRatio::square(), negativeZero);
    const auto positiveDigest = output::hashProcessPixelStreamV1(positiveImage);
    const auto negativeDigest = output::hashProcessPixelStreamV1(negativeImage);
    expectations.expect(positiveDigest && negativeDigest &&
                            positiveDigest.digest() != negativeDigest.digest(),
                        "positive and negative binary32 zero remain distinct stream bits");
}

void testTransactionalCapacity(Expectations& expectations) {
    auto fixture = compositionFixture();
    constexpr auto sentinel = std::byte{0xA5};
    std::array<std::byte, output::kCompositionProcessFrameSemanticIdentityV1Bytes - 1> shortBytes{};
    shortBytes.fill(sentinel);
    const auto shortWrite = output::writeProcessFrameSemanticIdentityV1(
        fixture.frameIdentity, fixture.processImage, shortBytes);
    expectations.expect(
        hasError(shortWrite, output::ProcessFrameSemanticIdentityErrorCode::InsufficientCapacity) &&
            shortWrite.requiredBytes() == output::kCompositionProcessFrameSemanticIdentityV1Bytes &&
            shortWrite.writtenBytes() == 0 &&
            std::ranges::all_of(shortBytes,
                                [sentinel](const auto value) { return value == sentinel; }),
        "one-byte-short capacity reports the exact need and writes nothing");

    std::array<std::byte, output::kProxyProcessFrameSemanticIdentityV1Bytes + 9> roomy{};
    roomy.fill(sentinel);
    const auto write = output::writeProcessFrameSemanticIdentityV1(fixture.frameIdentity,
                                                                   fixture.processImage, roomy);
    expectations.expect(
        write && std::ranges::all_of(std::span(roomy).subspan(write.writtenBytes()),
                                     [sentinel](const auto value) { return value == sentinel; }),
        "successful writing changes only the exact canonical record span");

    auto invalid = fixture.frameIdentity;
    invalid.plan.reset();
    std::array<std::byte, output::kProxyProcessFrameSemanticIdentityV1Bytes> invalidBytes{};
    invalidBytes.fill(sentinel);
    const auto invalidWrite =
        output::writeProcessFrameSemanticIdentityV1(invalid, fixture.processImage, invalidBytes);
    expectations.expect(
        hasError(invalidWrite, output::ProcessFrameSemanticIdentityErrorCode::MissingPlan) &&
            std::ranges::all_of(invalidBytes,
                                [sentinel](const auto value) { return value == sentinel; }),
        "invalid input is rejected before caller storage is touched");
}

void testPlanAndImageConsistency(Expectations& expectations) {
    auto fixture = compositionFixture();

    auto missingPlan = fixture.frameIdentity;
    missingPlan.plan.reset();
    expectations.expect(
        hasError(output::validateProcessFrameSemanticIdentityV1(missingPlan, fixture.processImage),
                 output::ProcessFrameSemanticIdentityErrorCode::MissingPlan),
        "a missing compiled plan is rejected");

    auto invalidIdDefinition = fixture.frameIdentity.plan->copyDefinition();
    invalidIdDefinition.projectId = {};
    const auto invalidIdPlan = publishPlan(std::move(invalidIdDefinition));
    auto invalidId = fixture.frameIdentity;
    invalidId.plan = invalidIdPlan;
    expectations.expect(
        hasError(output::validateProcessFrameSemanticIdentityV1(invalidId, fixture.processImage),
                 output::ProcessFrameSemanticIdentityErrorCode::InvalidStableId),
        "zero project identity is rejected");

    auto invalidOutput = fixture.frameIdentity;
    invalidOutput.output = runtime::OperationIndex::fromRaw(0);
    expectations.expect(hasError(output::validateProcessFrameSemanticIdentityV1(
                                     invalidOutput, fixture.processImage),
                                 output::ProcessFrameSemanticIdentityErrorCode::InvalidOutput),
                        "a non-terminal selected output is rejected");

    auto invalidOutputNodeDefinition = fixture.frameIdentity.plan->copyDefinition();
    auto* invalidOutputOperation = std::get_if<runtime::CompiledCompositionOutput>(
        &invalidOutputNodeDefinition.operations.back());
    if (invalidOutputOperation == nullptr) {
        std::abort();
    }
    invalidOutputOperation->sourceNodeId = {};
    const auto invalidOutputNodePlan = publishPlan(std::move(invalidOutputNodeDefinition));
    auto invalidOutputNode = fixture.frameIdentity;
    invalidOutputNode.plan = invalidOutputNodePlan;
    expectations.expect(hasError(output::validateProcessFrameSemanticIdentityV1(
                                     invalidOutputNode, fixture.processImage),
                                 output::ProcessFrameSemanticIdentityErrorCode::InvalidStableId),
                        "a zero stable output-node identity is rejected");

    auto wrongQuality = fixture.frameIdentity;
    wrongQuality.quality = enumWithBits<runtime::EvaluationQuality>(255);
    expectations.expect(
        hasError(output::validateProcessFrameSemanticIdentityV1(wrongQuality, fixture.processImage),
                 output::ProcessFrameSemanticIdentityErrorCode::UnsupportedEvaluationQuality),
        "an unknown evaluation quality is rejected");

    auto wrongColor = fixture.frameIdentity;
    wrongColor.colorIntent = enumWithBits<runtime::EvaluationColorIntent>(255);
    expectations.expect(
        hasError(output::validateProcessFrameSemanticIdentityV1(wrongColor, fixture.processImage),
                 output::ProcessFrameSemanticIdentityErrorCode::UnsupportedColorIntent),
        "an unknown process color intent is rejected");

    const std::array pixels{render::Rgba32f::transparent(), render::Rgba32f::transparent(),
                            render::Rgba32f::transparent(), render::Rgba32f::transparent(),
                            render::Rgba32f::transparent(), render::Rgba32f::transparent()};
    auto wrongExtent = image(window(0, 0, 3, 2), window(0, 0, 3, 2),
                             fixture.processImage.descriptor()->pixelAspect(), pixels);
    expectations.expect(
        hasError(output::validateProcessFrameSemanticIdentityV1(fixture.frameIdentity, wrongExtent),
                 output::ProcessFrameSemanticIdentityErrorCode::InconsistentImage),
        "composition resolution is bound to the process display extent");

    const std::array fourPixels{render::Rgba32f::transparent(), render::Rgba32f::transparent(),
                                render::Rgba32f::transparent(), render::Rgba32f::transparent()};
    auto wrongAspect =
        image(window(0, 0, 2, 2), window(0, 0, 2, 2), core::PixelAspectRatio::square(), fourPixels);
    expectations.expect(
        hasError(output::validateProcessFrameSemanticIdentityV1(fixture.frameIdentity, wrongAspect),
                 output::ProcessFrameSemanticIdentityErrorCode::InconsistentImage),
        "composition resolution is bound to the plan pixel aspect");

    const std::array croppedPixels{pixel(1.0F, 0.0F, 0.0F, 1.0F), pixel(0.0F, 1.0F, 0.0F, 1.0F)};
    auto croppedData = image(window(-4, 7, 1, 2), window(-5, 6, 2, 2),
                             fixture.processImage.descriptor()->pixelAspect(), croppedPixels);
    std::array<std::byte, output::kCompositionProcessFrameSemanticIdentityV1Bytes> croppedBytes{};
    expectations.expect(static_cast<bool>(output::writeProcessFrameSemanticIdentityV1(
                            fixture.frameIdentity, croppedData, croppedBytes)),
                        "independent data and display extents remain valid identity fields");

    auto validMoved = compositionFixture();
    auto owner = std::move(validMoved.processImage);
    expectations.expect(hasError(output::validateProcessFrameSemanticIdentityV1(
                                     validMoved.frameIdentity, validMoved.processImage),
                                 output::ProcessFrameSemanticIdentityErrorCode::InvalidImage) &&
                            owner.isValid(),
                        "a moved-from process image is rejected as malformed");
}

void testSemanticVersionsAndProviderNeutrality(Expectations& expectations) {
    auto fixture = compositionFixture();
    std::array<std::byte, output::kCompositionProcessFrameSemanticIdentityV1Bytes> baseline{};
    const auto baselineWrite = output::writeProcessFrameSemanticIdentityV1(
        fixture.frameIdentity, fixture.processImage, baseline);
    expectations.expect(static_cast<bool>(baselineWrite), "baseline semantic identity is valid");

    auto changedPlanVersion = fixture.frameIdentity;
    auto revisedDefinition = fixture.frameIdentity.plan->copyDefinition();
    revisedDefinition.planSemanticsVersion = 107;
    const auto revisedPlan = publishPlan(std::move(revisedDefinition));
    changedPlanVersion.plan = revisedPlan;
    std::array<std::byte, output::kCompositionProcessFrameSemanticIdentityV1Bytes> revised{};
    const auto revisedWrite = output::writeProcessFrameSemanticIdentityV1(
        changedPlanVersion, fixture.processImage, revised);
    expectations.expect(revisedWrite && revised != baseline,
                        "a different nonzero plan semantic version changes canonical bytes");

    auto changedEvaluator = fixture.frameIdentity;
    changedEvaluator.evaluatorSemanticsVersion = 109;
    const auto evaluatorWrite = output::writeProcessFrameSemanticIdentityV1(
        changedEvaluator, fixture.processImage, revised);
    expectations.expect(evaluatorWrite && revised != baseline,
                        "a different nonzero evaluator semantic version changes canonical bytes");

    auto changedPrimitive = fixture.frameIdentity;
    changedPrimitive.imagePrimitiveSemanticsVersion = 110;
    const auto primitiveWrite = output::writeProcessFrameSemanticIdentityV1(
        changedPrimitive, fixture.processImage, revised);
    expectations.expect(primitiveWrite && revised != baseline,
                        "a different nonzero primitive semantic version changes canonical bytes");

    auto changedAnimation = fixture.frameIdentity;
    auto animationDefinition = fixture.frameIdentity.plan->copyDefinition();
    animationDefinition.animationSamplingSemanticsVersion = 108;
    const auto animationPlan = publishPlan(std::move(animationDefinition));
    changedAnimation.plan = animationPlan;
    changedAnimation.animationSamplingSemanticsVersion = 108;
    const auto animationWrite = output::writeProcessFrameSemanticIdentityV1(
        changedAnimation, fixture.processImage, revised);
    expectations.expect(animationWrite && revised != baseline,
                        "a consistent nonzero animation semantic version changes canonical bytes");

    changedAnimation.animationSamplingSemanticsVersion = 8;
    expectations.expect(
        hasError(
            output::validateProcessFrameSemanticIdentityV1(changedAnimation, fixture.processImage),
            output::ProcessFrameSemanticIdentityErrorCode::InvalidSemanticsVersion),
        "an animation semantic version inconsistent with the plan is rejected");
    auto zeroVersion = fixture.frameIdentity;
    zeroVersion.evaluatorSemanticsVersion = 0;
    expectations.expect(
        hasError(output::validateProcessFrameSemanticIdentityV1(zeroVersion, fixture.processImage),
                 output::ProcessFrameSemanticIdentityErrorCode::InvalidSemanticsVersion),
        "zero semantic versions are rejected");

    auto differentProvider = fixture.frameIdentity;
    differentProvider.provider = enumWithBits<runtime::EvaluationProvider>(255);
    const auto providerWrite = output::writeProcessFrameSemanticIdentityV1(
        differentProvider, fixture.processImage, revised);
    expectations.expect(providerWrite && revised == baseline,
                        "execution provider is ignored by portable semantic bytes");
}

} // namespace

int main() {
    Expectations expectations;
    testCompositionGoldenVector(expectations);
    testProxyGoldenVector(expectations);
    testPixelBitsOriginAndOrder(expectations);
    testTransactionalCapacity(expectations);
    testPlanAndImageConsistency(expectations);
    testSemanticVersionsAndProviderNeutrality(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
