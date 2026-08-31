#pragma once

// Shared fixture/harness support for the flat OpenEXR writer and reopen-verifier test binaries
// (issue #99). Mirrors the golden idiom `process_frame_semantic_identity_tests.cpp` already uses
// to obtain a real `runtime::ProcessFrame`: evaluate a trivial one-pixel plan through the actual
// `CpuCompositionEvaluator` (the only producer of `ProcessFrame`, whose constructor is private),
// then move-assign an arbitrary fixture identity/image into the published frame -- `ProcessFrame`
// exposes public move assignment even though its constructor is private, and its data members are
// reachable through `const_cast` from a test that already holds a `shared_ptr<const ProcessFrame>`
// it uniquely owns. This header duplicates that pattern locally rather than reusing the existing
// test file, which owns its own private anonymous-namespace helpers.

#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/output/output_analysis.hpp>
#include <bloom/output/output_analysis_analyzer.hpp>
#include <bloom/output/process_frame_semantic_identity.hpp>
#include <bloom/render/image.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/evaluation.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace bloom_output_flat_exr_test_support {

namespace core = bloom::core;
namespace document = bloom::document;
namespace output = bloom::output;
namespace render = bloom::render;
namespace runtime = bloom::runtime;

constexpr auto kProjectId = document::ProjectId::fromRaw(0xA1A2A3A4A5A6A7A8ULL);
constexpr auto kCompositionId = document::CompositionId::fromRaw(0xB1B2B3B4B5B6B7B8ULL);
constexpr auto kOutputNodeId = document::NodeId::fromRaw(0xC1C2C3C4C5C6C7C8ULL);
constexpr auto kInputNodeId = document::NodeId::fromRaw(0xD1D2D3D4D5D6D7D8ULL);
constexpr auto kColorParameterId = document::ParameterId::fromRaw(0xE1E2E3E4E5E6E7E8ULL);
constexpr auto kSourceRevision = document::Revision::fromRaw(0xF1F2F3F4F5F6F7F8ULL);
constexpr auto kShellLayerNodeId = document::NodeId::fromRaw(0x61);
constexpr auto kShellLayerId = document::LayerId::fromRaw(0x62);
constexpr auto kShellStackNodeId = document::NodeId::fromRaw(0x63);
constexpr auto kShellSlotId = document::LayerSlotId::fromRaw(0x64);
constexpr auto kShellPositionParameterId = document::ParameterId::fromRaw(0x65);
constexpr auto kShellOpacityParameterId = document::ParameterId::fromRaw(0x66);

[[nodiscard]] inline render::Rgba32f pixel(const float red, const float green, const float blue,
                                           const float alpha) {
    const auto value = render::Rgba32f::fromPremultiplied(red, green, blue, alpha);
    if (!value) {
        std::abort();
    }
    return *value.value();
}

[[nodiscard]] inline render::ImageWindow window(const std::int64_t originX,
                                                const std::int64_t originY,
                                                const std::uint32_t width,
                                                const std::uint32_t height) {
    const auto value = render::ImageWindow::create(originX, originY, width, height);
    if (!value) {
        std::abort();
    }
    return *value.value();
}

[[nodiscard]] inline render::Rgba32fImage image(const render::ImageWindow dataWindow,
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

// Composition Output requires a layer-stack input, not a bare solid, so a real evaluation needs
// solid -> layer output -> layer stack -> composition output (mirroring
// process_frame_semantic_identity_tests.cpp's shellPlan()). This plan's own pixels are never
// observed: publish() immediately overwrites the evaluated frame's image/identity with a fixture.
[[nodiscard]] inline std::shared_ptr<const runtime::CompiledCompositionPlan> shellPlan() {
    const auto format = document::CompositionFormat::create(1, 1);
    if (!format) {
        std::abort();
    }
    std::vector<runtime::CompiledOperation> operations;
    operations.emplace_back(
        runtime::CompiledSolid{kInputNodeId, kColorParameterId, {0.0, 0.0, 0.0, 0.0}});
    operations.emplace_back(runtime::CompiledLayerOutput{
        kShellLayerNodeId, kShellLayerId, runtime::OperationIndex::fromRaw(0),
        runtime::CompiledVec2Parameter{kShellPositionParameterId, document::Vec2d{0.5, 0.5}},
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

// A minimal plan used only to give ProcessFrameIdentity an opaque `.plan` handle whose
// CompositionFormat declares the exact width/height/pixelAspect a fixture's data window claims --
// required because ProcessFrameSemanticIdentityV1Preparer cross-checks a
// CompositionFormatResolution identity's declared format against the actual published image
// (mirroring process_frame_semantic_identity_tests.cpp's plan()). This plan is never evaluated.
[[nodiscard]] inline std::shared_ptr<const runtime::CompiledCompositionPlan>
identityPlan(const std::uint32_t width, const std::uint32_t height,
             const core::PixelAspectRatio pixelAspect) {
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
                                                   .output = runtime::OperationIndex::fromRaw(1)});
}

[[nodiscard]] inline std::shared_ptr<const runtime::ProcessFrame> evaluateShell() {
    const auto compiledPlan = shellPlan();
    const runtime::EvaluationRequest request{.time = core::RationalTime::fromInteger(0),
                                             .output = compiledPlan->output(),
                                             .resolution = runtime::CompositionFormatResolution{},
                                             .quality = runtime::EvaluationQuality::Reference,
                                             .colorIntent =
                                                 runtime::EvaluationColorIntent::LinearRec709Scene,
                                             .pixelStorageByteLimit = 4096};
    const runtime::CpuCompositionEvaluator evaluator;
    const auto result = evaluator.evaluate(compiledPlan, request, {});
    if (result.status() != runtime::EvaluationStatus::Evaluated || result.frame() == nullptr) {
        for (const auto& diagnostic : result.diagnostics()) {
            std::cerr << "evaluateShell diagnostic: "
                      << runtime::evaluationDiagnosticCodeId(diagnostic.code)
                      << " summary=" << diagnostic.summary << " detail=" << diagnostic.detail
                      << '\n';
        }
        std::abort();
    }
    return result.frame();
}

struct Fixture final {
    runtime::ProcessFrameIdentity identity;
    render::Rgba32fImage processImage;
};

// Publishes `fixture` as the retained image/identity of a real (privately-constructed)
// `ProcessFrame`. `ProcessFrame` exposes public move assignment (its constructor is private, its
// copy operations are deleted); a test that owns the only reference may reach past its private
// members via const_cast to move a fixture image/identity into an already-published frame.
[[nodiscard]] inline std::shared_ptr<const runtime::ProcessFrame> publish(Fixture fixture) {
    auto published = std::const_pointer_cast<runtime::ProcessFrame>(evaluateShell());
    auto& identity = const_cast<runtime::ProcessFrameIdentity&>(published->identity());
    auto& processImage = const_cast<render::Rgba32fImage&>(published->processImage());
    identity = std::move(fixture.identity);
    processImage = std::move(fixture.processImage);
    return published;
}

[[nodiscard]] inline runtime::ProcessFrameIdentity
frameIdentity(const std::shared_ptr<const runtime::CompiledCompositionPlan>& compiledPlan) {
    const auto time = core::RationalTime::create(3, 4);
    if (!time) {
        std::abort();
    }
    return {.plan = compiledPlan,
            .time = *time,
            .output = compiledPlan->output(),
            .resolution = runtime::CompositionFormatResolution{},
            .quality = runtime::EvaluationQuality::Reference,
            .colorIntent = runtime::EvaluationColorIntent::LinearRec709Scene,
            .provider = runtime::EvaluationProvider::CpuReference,
            .evaluatorSemanticsVersion = 1,
            .animationSamplingSemanticsVersion = 1,
            .imagePrimitiveSemanticsVersion = 1};
}

// A 3x2 fixture covering negative, HDR, signed-zero, zero-alpha (forced canonical-zero by
// `Rgba32f::fromPremultiplied`), and subnormal samples, at a nonzero, non-equal data/display
// window with a pixel aspect that does NOT round-trip exactly through binary32 (4/3).
[[nodiscard]] inline Fixture roundTripFixture() {
    const auto pixelAspect = core::PixelAspectRatio::create(4, 3);
    if (!pixelAspect) {
        std::abort();
    }
    const auto subnormal = std::numeric_limits<float>::denorm_min();
    const std::array pixels{
        pixel(-0.0F, 0.0F, -3.5F, 1.0F),          // signed zero, negative
        pixel(120.0F, 0.5F, 0.25F, 0.5F),         // HDR
        render::Rgba32f::transparent(),           // zero-alpha, canonical zero
        pixel(subnormal, -subnormal, 0.0F, 1.0F), // subnormal, negative subnormal
        pixel(-8.0F, 16.5F, -0.001F, 0.75F),      // negative/HDR mix
        pixel(0.0F, 0.0F, 0.0F, 1.0F),            // opaque black
    };
    // ProcessFrameSemanticIdentityV1Preparer's CompositionFormatResolution check compares the
    // *display* window's extent (not the data window's) against the declared plan format.
    return {frameIdentity(identityPlan(5, 4, *pixelAspect)),
            image(window(-2, 5, 3, 2), window(-10, -20, 5, 4), *pixelAspect, pixels)};
}

// Same shape as roundTripFixture() but with an exactly-representable pixel aspect (2/1), used by
// the "pixel-aspect rounds exactly" conversion-edge test.
[[nodiscard]] inline Fixture exactPixelAspectFixture() {
    const auto pixelAspect = core::PixelAspectRatio::create(2, 1);
    if (!pixelAspect) {
        std::abort();
    }
    const std::array pixels{pixel(1.0F, 2.0F, 3.0F, 1.0F),  pixel(-1.0F, -2.0F, -3.0F, 1.0F),
                            render::Rgba32f::transparent(), pixel(0.5F, 0.5F, 0.5F, 1.0F),
                            pixel(4.0F, 4.0F, 4.0F, 1.0F),  pixel(-0.0F, 0.0F, 0.0F, 1.0F)};
    return {frameIdentity(identityPlan(3, 2, *pixelAspect)),
            image(window(0, 0, 3, 2), window(0, 0, 3, 2), *pixelAspect, pixels)};
}

// Both members are the exact public products `FlatExrRgba32fLinRec709SceneReopenVerifierV1::
// verify()` accepts; it binds them into the module-private `BoundOutputAnalysisV1` itself, so
// nothing here needs to touch that private type.
struct PreparedSource final {
    std::shared_ptr<const runtime::ProcessFrame> frame;
    std::shared_ptr<const output::ProcessFrameSemanticIdentityV1> processIdentity;
    std::shared_ptr<const output::OutputAnalysisReportV1> report;
};

// Runs the already-implemented pre-approval pipeline this slice consumes but does not
// reimplement: prepare the frame-bound `ProcessFrameSemanticIdentityV1` and analyze it with the
// existing EXR analyzer -- the exact public input shape the reopen verifier requires.
[[nodiscard]] inline PreparedSource prepareSource(Fixture fixture) {
    auto frame = publish(std::move(fixture));

    const output::ProcessFrameSemanticIdentityV1Preparer identityPreparer;
    const auto identityResult = identityPreparer.prepare(frame, {});
    if (identityResult.status() !=
            output::ProcessFrameSemanticIdentityPreparationStatus::Prepared ||
        identityResult.identity() == nullptr) {
        std::abort();
    }

    const auto analyzed = output::analyzeFlatExrRgba32fLinRec709SceneV1(
        {.process = {.state = output::OutputAnalysisProcessSourceStateV1::Ready,
                     .readyIdentity = identityResult.identity(),
                     .missingDescriptor = std::nullopt}});
    if (!analyzed.hasReport()) {
        std::abort();
    }

    return {frame, identityResult.identity(), analyzed.report()};
}

// RAII per-process scratch directory under the platform temp root, isolating this test binary's
// staged artifacts from other parallel ctest processes. Best-effort cleanup on destruction.
class ScratchDirectory final {
  public:
    explicit ScratchDirectory(const std::string& label) {
        static std::atomic<std::uint64_t> counter{0};
        const auto suffix =
            std::to_string(static_cast<unsigned long long>(std::hash<std::string>{}(label))) + "-" +
            std::to_string(counter.fetch_add(1));
        std::error_code errorCode;
        path_ =
            std::filesystem::temp_directory_path(errorCode) / ("bloom-output-exr-test-" + suffix);
        if (errorCode) {
            std::abort();
        }
        std::filesystem::create_directories(path_, errorCode);
        if (errorCode) {
            std::abort();
        }
    }
    ScratchDirectory(const ScratchDirectory&) = delete;
    ScratchDirectory& operator=(const ScratchDirectory&) = delete;
    ScratchDirectory(ScratchDirectory&&) = delete;
    ScratchDirectory& operator=(ScratchDirectory&&) = delete;
    ~ScratchDirectory() {
        std::error_code errorCode;
        std::filesystem::remove_all(path_, errorCode);
    }

    [[nodiscard]] std::filesystem::path file(const std::string& name) const { return path_ / name; }

  private:
    std::filesystem::path path_;
};

// The small "Expectations" idiom every src/output test file already uses (e.g.
// output_facet_descriptor_tests.cpp, process_frame_semantic_identity_tests.cpp), duplicated here
// so both flat OpenEXR test binaries share one copy instead of two more.
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

// Minimal raw byte-level access to a valid staged EXR's classic (non-multipart) header, used by
// the adversarial fixtures to craft corrupted variants of a real staged file. The classic header
// format is a sequence of attribute records -- name (NUL-terminated), type (NUL-terminated),
// size (int32 little-endian), then `size` value bytes -- starting right after the 8-byte
// magic/version field and terminated by one empty (zero) byte. This scanner never touches the
// chunk offset table or pixel data that follow: every adversarial case below either patches an
// attribute's value bytes in place (same length, so nothing after it moves) or inserts a whole
// new attribute record immediately before the header terminator, which the reopen verifier's
// attribute-allowlist check rejects before it would ever reach the (now differently offset) chunk
// table.
class ExrHeaderBytes final {
  public:
    explicit ExrHeaderBytes(const std::filesystem::path& path) : path_(path) {
        std::ifstream stream(path, std::ios::binary);
        if (!stream) {
            std::abort();
        }
        bytes_.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
        if (bytes_.size() < 8) {
            std::abort();
        }
        headerEnd_ = scanToHeaderEnd();
    }

    // Byte offset of one attribute's value (after its name/type/size fields), and that value's
    // byte length. Aborts if the attribute is not present -- every caller here already wrote it.
    [[nodiscard]] std::pair<std::size_t, std::size_t>
    valueRange(const std::string& attributeName) const {
        std::size_t offset = 8;
        while (offset < headerEnd_) {
            const auto recordStart = offset;
            const auto name = readCString(offset);
            const auto type = readCString(offset);
            (void)type;
            const auto size = static_cast<std::size_t>(readInt32(offset));
            const auto valueOffset = offset;
            if (name == attributeName) {
                return {valueOffset, size};
            }
            offset = valueOffset + size;
            if (offset <= recordStart) {
                std::abort();
            }
        }
        std::abort();
    }

    [[nodiscard]] std::size_t headerEnd() const noexcept { return headerEnd_; }

    void patchByte(const std::size_t offset, const unsigned char value) {
        bytes_.at(offset) = value;
    }

    [[nodiscard]] unsigned char byteAt(const std::size_t offset) const {
        return static_cast<unsigned char>(bytes_.at(offset));
    }

    // Inserts `record` (a complete, well-formed attribute record: name\0 type\0 size value...)
    // immediately before the header terminator. Only safe for tests that never read past the
    // (now-invalid) chunk offset table -- every insertion-based adversarial case here is rejected
    // by the attribute-allowlist check before the verifier would reach it.
    void insertAttributeRecord(const std::vector<unsigned char>& record) {
        bytes_.insert(bytes_.begin() + static_cast<std::ptrdiff_t>(headerEnd_), record.begin(),
                      record.end());
    }

    void truncateTo(const std::size_t length) { bytes_.resize(std::min(length, bytes_.size())); }

    void save() const {
        std::ofstream stream(path_, std::ios::binary | std::ios::trunc);
        if (!stream) {
            std::abort();
        }
        stream.write(reinterpret_cast<const char*>(bytes_.data()),
                     static_cast<std::streamsize>(bytes_.size()));
    }

  private:
    [[nodiscard]] std::string readCString(std::size_t& offset) const {
        const auto start = offset;
        while (offset < bytes_.size() && bytes_[offset] != 0) {
            ++offset;
        }
        if (offset >= bytes_.size()) {
            std::abort();
        }
        std::string result(bytes_.begin() + static_cast<std::ptrdiff_t>(start),
                           bytes_.begin() + static_cast<std::ptrdiff_t>(offset));
        ++offset; // consume the NUL
        return result;
    }

    [[nodiscard]] std::int32_t readInt32(std::size_t& offset) const {
        if (offset + 4 > bytes_.size()) {
            std::abort();
        }
        std::uint32_t value = 0;
        for (int index = 0; index < 4; ++index) {
            value |= static_cast<std::uint32_t>(static_cast<unsigned char>(
                         bytes_[offset + static_cast<std::size_t>(index)]))
                     << (8 * index);
        }
        offset += 4;
        return static_cast<std::int32_t>(value);
    }

    [[nodiscard]] std::size_t scanToHeaderEnd() const {
        std::size_t offset = 8;
        while (offset < bytes_.size() && bytes_[offset] != 0) {
            (void)readCString(offset);
            (void)readCString(offset);
            const auto size = static_cast<std::size_t>(readInt32(offset));
            offset += size;
        }
        if (offset >= bytes_.size()) {
            std::abort();
        }
        return offset; // the terminating zero byte itself
    }

    std::filesystem::path path_;
    std::vector<unsigned char> bytes_;
    std::size_t headerEnd_ = 0;
};

} // namespace bloom_output_flat_exr_test_support
