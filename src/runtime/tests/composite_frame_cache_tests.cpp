// Scene-referred composite frame cache: identity mismatches miss, a hit equals a fresh render at
// half precision, and retention is evicted under memory pressure.
#include <bloom/core/rational_time.hpp>
#include <bloom/core/sha256.hpp>
#include <bloom/document/asset.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/media/image.hpp>
#include <bloom/runtime/composite_frame_cache.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/gpu_processing_phase_policy.hpp>

#include <ImathBox.h>
#include <ImfChannelList.h>
#include <ImfFrameBuffer.h>
#include <ImfHeader.h>
#include <ImfOutputFile.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace {
using namespace bloom;
using namespace bloom::runtime;
constexpr std::uint64_t kMiB = 1024ULL * 1024;
constexpr std::uint64_t kGiB = 1024 * kMiB;

void require(const bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

void writeExr(const std::filesystem::path& path, const int width, const int height,
              const float shade) {
    const Imath::Box2i window(Imath::V2i(0, 0), Imath::V2i(width - 1, height - 1));
    Imf::Header header(window, window);
    for (const char* name : {"R", "G", "B", "A"})
        header.channels().insert(name, Imf::Channel(Imf::FLOAT));
    const auto pixels = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    std::array<std::vector<float>, 4> planes;
    for (std::size_t channel = 0; channel < 4; ++channel)
        planes[channel].resize(pixels);
    for (std::size_t index = 0; index < pixels; ++index) {
        // Values that are not representable in half exercise the rounding.
        planes[0][index] = shade + static_cast<float>(index % 97) * 0.0013F;
        planes[1][index] = 0.3333333F + static_cast<float>(index % 13) * 0.0101F;
        planes[2][index] = 1.7F * shade;
        planes[3][index] = 1.0F;
    }
    Imf::FrameBuffer frameBuffer;
    const char* names[] = {"R", "G", "B", "A"};
    for (std::size_t channel = 0; channel < 4; ++channel)
        frameBuffer.insert(names[channel],
                           Imf::Slice(Imf::FLOAT, reinterpret_cast<char*>(planes[channel].data()),
                                      sizeof(float),
                                      sizeof(float) * static_cast<std::size_t>(width)));
    Imf::OutputFile file(path.string().c_str(), header);
    file.setFrameBuffer(frameBuffer);
    file.writePixels(height);
}

document::AssetRecord asset(const std::filesystem::path& path) {
    const auto probe = media::probeImage(path);
    require(probe.value.has_value(), "fixture EXR probes");
    document::AssetRecord record;
    record.id = document::AssetId::fromRaw(0x501);
    record.kind = document::AssetKind::Image;
    record.locator.kind = "file";
    record.locator.portability = "project-relative";
    record.locator.path = path.filename().string();
    record.locator.relinkHint = "file:";
    record.contentDigest = probe.value->contentDigest;
    record.interpretation.colorSpace = document::AssetColorSpace::Raw;
    record.width = probe.value->width;
    record.height = probe.value->height;
    return record;
}

// Image source and a translucent solid over it, through layer outputs and a merge.
std::shared_ptr<const CompiledCompositionPlan> plan(const document::AssetRecord& media,
                                                    const double red) {
    const auto format = document::CompositionFormat::create(64, 32);
    require(format.has_value(), "fixture format");
    std::vector<CompiledOperation> operations;
    const auto layer = [&](const std::uint64_t id, const std::uint32_t source, const double x) {
        operations.emplace_back(CompiledLayerOutput{
            document::NodeId::fromRaw(id), document::LayerId::fromRaw(id + 100),
            OperationIndex::fromRaw(source),
            CompiledVec2Parameter{document::ParameterId::fromRaw(id + 1), document::Vec2d{x, 0.5}},
            CompiledVec2Parameter{document::ParameterId::fromRaw(id + 2), document::kDefaultAnchor},
            CompiledVec2Parameter{document::ParameterId::fromRaw(id + 3), document::kDefaultScale},
            CompiledScalarParameter{document::ParameterId::fromRaw(id + 4), 0.0},
            CompiledScalarParameter{document::ParameterId::fromRaw(id + 5), 0.75},
            document::ParameterId::fromRaw(id + 6), core::kDefaultBlendMode});
    };
    operations.emplace_back(
        CompiledImageSource{document::NodeId::fromRaw(10), media, 0, 0, 0, {}, false});
    layer(20, 0, 0.5);
    operations.emplace_back(
        CompiledSolid{document::NodeId::fromRaw(30),
                      {document::ParameterId::fromRaw(31), core::Color4d{red, 0.25, 0.125, 1.0}},
                      {document::ParameterId::fromRaw(32), 24.0},
                      {document::ParameterId::fromRaw(33), 16.0}});
    layer(40, 2, 0.4);
    operations.emplace_back(
        CompiledMerge{document::NodeId::fromRaw(50),
                      {{document::LayerSlotId::fromRaw(51), document::LayerId::fromRaw(120),
                        OperationIndex::fromRaw(1)},
                       {document::LayerSlotId::fromRaw(52), document::LayerId::fromRaw(140),
                        OperationIndex::fromRaw(3)}}});
    operations.emplace_back(
        CompiledCompositionOutput{document::NodeId::fromRaw(60), OperationIndex::fromRaw(4)});
    return std::make_shared<const CompiledCompositionPlan>(
        CompiledCompositionPlanDefinition{.sourceRevision = document::Revision::fromRaw(7),
                                          .projectId = document::ProjectId::fromRaw(8),
                                          .compositionId = document::CompositionId::fromRaw(9),
                                          .format = *format,
                                          .operations = std::move(operations),
                                          .output = OperationIndex::fromRaw(5)});
}

EvaluationRequest request(const std::shared_ptr<const CompiledCompositionPlan>& compiled) {
    return {.time = core::RationalTime::fromInteger(0),
            .output = compiled->output(),
            .resolution = CompositionFormatResolution{},
            .quality = EvaluationQuality::Reference,
            .colorIntent = EvaluationColorIntent::LinearRec709Scene,
            .pixelStorageByteLimit = 64 * kMiB};
}

std::shared_ptr<const ProcessFrame> render(const std::shared_ptr<const CompiledCompositionPlan>& p,
                                           const std::filesystem::path& base) {
    CpuCompositionEvaluator evaluator;
    evaluator.setAssetBaseDirectory(base);
    auto result = evaluator.evaluate(p, request(p), {});
    require(result.status() == EvaluationStatus::Evaluated && result.frame(),
            "the fixture composition renders");
    return result.frame();
}

void halfConversion() {
    require(floatToHalf(1.0F) == 0x3c00 && halfToFloat(0x3c00) == 1.0F, "one");
    require(floatToHalf(-2.0F) == 0xc000 && halfToFloat(0xc000) == -2.0F, "negative two");
    require(floatToHalf(65504.0F) == 0x7bff && floatToHalf(1.0e6F) == 0x7c00, "max and overflow");
    require(floatToHalf(5.9604645e-8F) == 0x0001 && halfToFloat(0x0001) == 5.9604645e-8F,
            "smallest subnormal");
    require(floatToHalf(1.0F + 1.0F / 2048.0F) == 0x3c00 &&
                floatToHalf(1.0F + 3.0F / 2048.0F) == 0x3c02,
            "ties round to even");
    for (std::uint32_t bits = 0; bits < 0x7c00; ++bits) {
        const auto half = static_cast<std::uint16_t>(bits);
        require(floatToHalf(halfToFloat(half)) == half, "every finite half round-trips exactly");
    }
}

std::shared_ptr<const CompiledCompositionPlan>
identityAndPixels(const std::filesystem::path& directory) {
    const auto mediaPath = directory / "plate.exr";
    writeExr(mediaPath, 64, 32, 0.4F);
    const auto media = asset(mediaPath);
    const auto first = plan(media, 0.8);
    CompositeFrameCache cache;
    const auto key = compositeFrameKeyFor(first, request(first), directory);
    require(key.has_value() && key->media.size() == 1, "a full Reference request is cacheable");
    require(!cache.lookup(*key), "an empty cache misses");
    const auto fresh = render(first, directory);
    const auto stored = CompositeFrame::fromImage(fresh->processImage());
    require(stored && stored->bytes() == 64ULL * 32 * 8, "RGBA half storage is 8 bytes a pixel");
    require(cache.insert(*key, stored), "the rendered frame is retained");

    // An equal plan built separately (an unrelated document edit) hits by deep identity.
    const auto rebuilt = plan(media, 0.8);
    const auto sameKey = compositeFrameKeyFor(rebuilt, request(rebuilt), directory);
    const auto hit = cache.lookup(*sameKey);
    require(hit != nullptr, "the same render identity hits");
    auto widened = hit->toImage();
    require(widened.hasValue(), "a hit widens to float32");
    const auto again = render(rebuilt, directory);
    const auto expected = again->processImage().pixels();
    const auto actual = widened.value()->pixels();
    require(expected.size() == actual.size(), "hit extent equals a fresh render");
    for (std::size_t index = 0; index < expected.size(); ++index) {
        const auto lhs = expected[index].components();
        const auto rhs = actual[index].components();
        for (std::size_t channel = 0; channel < 4; ++channel)
            require(halfToFloat(floatToHalf(lhs[channel])) == rhs[channel],
                    "a hit equals a fresh render bit for bit at half precision");
    }
    require(*widened.value()->descriptor() == *again->processImage().descriptor(),
            "a hit keeps the fresh render's windows and pixel aspect");

    // An edit (different solid colour) misses.
    const auto edited = plan(media, 0.2);
    require(!cache.lookup(*compositeFrameKeyFor(edited, request(edited), directory)),
            "an edited composition misses");
    // Another time misses.
    auto later = request(first);
    later.time = core::RationalTime::fromInteger(1);
    require(!cache.lookup(*compositeFrameKeyFor(first, later, directory)), "another time misses");
    // An OCIO config/working-space change misses.
    auto ocio = request(first);
    const std::array<std::byte, 3> configBytes{std::byte{1}, std::byte{2}, std::byte{3}};
    ocio.colorIntent.ocioConfigRevision = *core::Sha256Hasher::hash(configBytes);
    require(!cache.lookup(*compositeFrameKeyFor(first, ocio, directory)),
            "an OCIO config change misses");
    // Half resolution and an ROI are not cacheable identities at all.
    auto half = request(first);
    const auto halfExtent = render::ImageExtent::create(32, 16);
    half.resolution = ProxyResolution{*halfExtent.value()};
    require(!compositeFrameKeyFor(first, half, directory), "Half resolution is not cacheable");
    auto roi = request(first);
    const auto roiWindow = render::ImageWindow::create(0, 0, 8, 8);
    roi.roi = *roiWindow.value();
    require(!compositeFrameKeyFor(first, roi, directory), "an ROI is not cacheable");
    // The media changes on disk: the stored key's stamp no longer matches and the entry drops.
    writeExr(mediaPath, 64, 32, 0.9F);
    std::filesystem::last_write_time(mediaPath, std::filesystem::last_write_time(mediaPath) +
                                                    std::chrono::seconds(2));
    require(!cache.lookup(*key), "a media file changed on disk misses");
    const auto stats = cache.stats();
    require(stats.staleMediaMisses == 1 && stats.entries == 0 && stats.bytes == 0,
            "the stale entry is dropped and its bytes released");
    return first;
}

void evictionUnderPressure(const std::shared_ptr<const CompiledCompositionPlan>& shared) {
    // A root with 1 MiB of usable host memory: each 64x32 frame is 16 KiB.
    auto policy = std::make_shared<GpuProcessingPhasePolicy>(32);
    MemoryBudgetState sample;
    sample.machine.availableBytes = kMiB;
    sample.process.status = ProcessMemoryStatus::Unsupported;
    sample.process.source = ProcessMemorySource::OtherHostOnly;
    sample.process.sampledAt = GpuProcessingPhasePolicy::Clock::now();
    sample.processSampleFresh = true;
    sample.sampleSerial = 1;
    policy->applyHostSample(sample, 64 * kGiB, sample.process.sampledAt);
    CompositeFrameCache cache(policy);
    const auto madeWindow = render::ImageWindow::create(0, 0, 64, 32);
    const auto window = *madeWindow.value();
    const auto madeDescriptor =
        render::Rgba32fImageDescriptor::create(window, window, core::PixelAspectRatio::square());
    const auto descriptor = *madeDescriptor.value();
    auto builder = render::Rgba32fImageBuilder::create(descriptor, kMiB);
    auto image = std::move(*builder.value()).freeze();
    const auto frame = CompositeFrame::fromImage(*image.value());
    std::vector<CompositeFrameKey> keys;
    for (std::int64_t time = 0; time < 40; ++time) {
        CompositeFrameKey key{.plan = shared,
                              .time = core::RationalTime::fromInteger(time),
                              .output = OperationIndex::fromRaw(0),
                              .colorIntent = EvaluationColorIntent::LinearRec709Scene,
                              .media = {}};
        keys.push_back(key);
        require(cache.insert(key, frame), "frames are retained while usable memory allows");
    }
    require(cache.stats().entries == 40, "forty 16 KiB frames fit 1 MiB of usable memory");
    // An active phase takes most of the usable memory; the next insert evicts LRU frames first.
    auto held = policy->reserve(
        {.pixels = 0, .metadataBytes = kMiB - 64 * 1024, .sourceFree = true, .native = false});
    require(held.lease != nullptr, "active work claims most of usable memory");
    keys.front().time = core::RationalTime::fromInteger(99);
    require(cache.insert(keys.front(), frame), "a new frame still fits after eviction");
    const auto stats = cache.stats();
    require(stats.evictions > 0 && stats.bytes <= 64 * 1024 + frame->bytes(),
            "retention shrinks to the free usable memory under pressure");
    require(!cache.lookup(keys[1]) || stats.entries < 40, "least recently used frames go first");
    require(cache.evict(std::numeric_limits<std::uint64_t>::max()) > 0 &&
                cache.stats().entries == 0,
            "a reclaimer can release all retention");
}
} // namespace

int main() {
    std::array<char, 64> pattern{};
    const std::string prefix = "/tmp/bloom-composite-cache-XXXXXX";
    std::copy(prefix.begin(), prefix.end(), pattern.begin());
    const auto* made = ::mkdtemp(pattern.data());
    if (made == nullptr)
        return EXIT_FAILURE;
    const std::filesystem::path directory = made;
    int status = EXIT_SUCCESS;
    try {
        halfConversion();
        const auto shared = identityAndPixels(directory);
        evictionUnderPressure(shared);
        std::cout << "composite frame cache: all expectations passed\n";
    } catch (const std::exception& error) {
        std::cerr << "composite frame cache: " << error.what() << '\n';
        status = EXIT_FAILURE;
    }
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    return status;
}
