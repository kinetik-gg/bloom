#include "gpu_composite_native_support.hpp"
#include "gpu_composite_private.hpp"
#include "gpu_path_coverage_private.hpp"

#include <bloom/render/gpu_affine.hpp>
#include <bloom/render/gpu_blend.hpp>
#include <bloom/render/gpu_point_resample.hpp>

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using namespace bloom::render;
constexpr std::uint64_t kBudget = 16ULL * 1024 * 1024;
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
template <class Operation, class Poll> Poll wait(Operation& operation, Poll pending) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    auto status = operation.poll();
    while (status == pending && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
        status = operation.poll();
    }
    return status;
}
std::shared_ptr<GpuProcessingPhaseLease> admit(GpuDevice& device, GpuProcessingPhaseBudget& budget,
                                               std::uint64_t bytes = kBudget / 2) {
    auto grant =
        budget.reserve({.fixedHostBytes = 1, .deviceWorkingBytes = bytes, .sourceFree = true});
    require(grant.lease && device.retainProcessingPhase(grant.lease),
            "exact operation phase retained at request capacity one");
    return std::move(grant.lease);
}
void retire(GpuDevice& device, GpuProcessingPhaseBudget& budget,
            std::shared_ptr<GpuProcessingPhaseLease>& phase) {
    device.retireProcessingPhase(phase->id(), true);
    phase.reset();
    const auto snapshot = budget.snapshot();
    require(snapshot.admittedPhases == 0 && snapshot.totalHostClaimBytes == 0 &&
                snapshot.totalDeviceClaimBytes == 0 && snapshot.ownedNativeBuffers == 0 &&
                snapshot.ownedNativeImages == 0,
            "retired native operation conserves all phase and backing claims");
}
void checkPixels(const GpuImage& image, const Rgba32f expected) {
    const auto result = readbackResidentImage(image, kBudget);
    require(result.hasValue() && !result.pixels.empty(), "real native output readable for oracle");
    for (const auto pixel : result.pixels)
        require(composite_proof::closeEnough(pixel.red(), expected.red()) &&
                    composite_proof::closeEnough(pixel.green(), expected.green()) &&
                    composite_proof::closeEnough(pixel.blue(), expected.blue()) &&
                    composite_proof::closeEnough(pixel.alpha(), expected.alpha()),
                "all native components match the independent constant CPU reference");
}
void helperOwnership(GpuDevice& device) {
    const auto state = GpuRendererAccess::state(device);
    GpuProcessingPhaseBudget budget(1);
    budget.setHostCapacity(kBudget, true);
    budget.setDeviceCapacity(kBudget, true);
    auto phase = admit(device, budget);
    const std::array<std::uint32_t, 16> values{};
    CompositeBuffer first;
    require(createCompositeBuffer(state, sizeof(values), false, values.data(), first),
            "device metadata and mapped staging are allocated through production factory");
    require(budget.snapshot().ownedNativeBuffers == 2 && first.storageToken.bytes() > 0 &&
                first.stagingStorageToken.bytes() > 0,
            "metadata and initialization staging each own one exact backing claim");
    const auto total = first.storageToken.bytes() + first.stagingStorageToken.bytes();
    CompositeBuffer moved(std::move(first));
    first = CompositeBuffer{};
    require(first.storageToken.bytes() == 0 && first.stagingStorageToken.bytes() == 0 &&
                budget.snapshot().ownedNativeBufferBytes == total,
            "composite buffer move carries both physical owners exactly once");
    moved.releaseStaging();
    require(budget.snapshot().ownedNativeBuffers == 1,
            "proven unused staging destruction precedes its token foldback");
    moved.release();
    retire(device, budget, phase);
    phase = admit(device, budget, total - 1);
    const auto attempts = state->nativeBufferBackingAttempts;
    require(!createCompositeBuffer(state, sizeof(values), false, values.data(), first) &&
                budget.snapshot().ownedNativeBuffers == 0 &&
                state->nativeBufferBackingAttempts == attempts + 1,
            "partial metadata preparation refuses staging and destroys already-created backing");
    const auto refusal = device.nativeBufferStorageRefusal();
    require(refusal && refusal->phaseId == phase->id() &&
                refusal->domain == GpuProcessingPhaseDomain::Device,
            "partial preparation keeps exact buffer capacity retry provenance");
    retire(device, budget, phase);
}
void operationCycles(GpuDevice& device) {
    const auto window = composite_proof::window(0, 0, 64, 32);
    const auto color = composite_proof::pixel(0.25F, 0.5F, 1.0F, 1.0F);
    auto solid = GpuSolid::create(device);
    require(solid.hasValue(), "rootless input producer created");
    auto sourceImage = composite_proof::solid(*solid.solid, {0.25, 0.5, 1.0, 1.0}, window, window,
                                              bloom::core::PixelAspectRatio::square());
    if (!sourceImage)
        throw std::runtime_error("native source preparation failed outside owned attempt");
    auto source = std::make_shared<const GpuImage>(std::move(*sourceImage));
    auto composite = GpuComposite::create(device);
    auto affine = GpuAffine::create(device);
    auto blend = GpuBlend::create(device);
    auto point = GpuPointResample::create(device);
    require(composite && affine && blend && point, "real operation wrappers created");
    GpuProcessingPhaseBudget budget(1);
    budget.setHostCapacity(kBudget, true);
    budget.setDeviceCapacity(kBudget, true);
    const auto state = GpuRendererAccess::state(device);
    VmaTotalStatistics baseline{};
    vmaCalculateStatistics(state->allocator, &baseline);
    for (int iteration = 0; iteration != 10; ++iteration) {
        auto phase = admit(device, budget);
        require(composite.composite->beginTranslation({source, window, 0, 0, 0.5F, std::nullopt}, kBudget).code ==
                        GpuCompositeDiagnosticCode::None &&
                    budget.snapshot().ownedNativeBuffers == 5,
                "translation owns two axes, two staging allocations and status");
        if (iteration % 2 != 0)
            composite.composite->cancel();
        const auto status = wait(*composite.composite, GpuCompositePollResult::Pending);
        if (iteration % 2 != 0) {
            require(status == GpuCompositePollResult::Failure &&
                        budget.snapshot().ownedNativeBuffers == 0,
                    "cancelled translation frees all buffers only after genuine completion");
        } else {
            require(status == GpuCompositePollResult::Ready &&
                        budget.snapshot().ownedNativeBuffers == 3,
                    "completed translation retires both initialization buffers");
            auto image = composite.composite->takeImage();
            checkPixels(image, composite_proof::pixel(0.125F, 0.25F, 0.5F, 0.5F));
        }
        retire(device, budget, phase);
        phase = admit(device, budget);
        require(composite.composite->beginSourceOver({source, source}, kBudget).code ==
                        GpuCompositeDiagnosticCode::None &&
                    budget.snapshot().ownedNativeBuffers == 1,
                "source-over status is charged");
        require(wait(*composite.composite, GpuCompositePollResult::Pending) ==
                    GpuCompositePollResult::Ready,
                "source-over real fence retires");
        {
            auto image = composite.composite->takeImage();
            checkPixels(image, color);
        }
        retire(device, budget, phase);
        phase = admit(device, budget);
        require(affine.affine->beginAffineMatrix({source, window, {}, 0.5F}, kBudget).code ==
                        GpuAffineDiagnosticCode::None &&
                    budget.snapshot().ownedNativeBuffers == 2,
                "affine compact map and status are charged");
        require(wait(*affine.affine, GpuAffinePollResult::Pending) == GpuAffinePollResult::Ready,
                "affine real fence retires");
        {
            auto image = affine.affine->takeImage();
            checkPixels(image, composite_proof::pixel(0.125F, 0.25F, 0.5F, 0.5F));
        }
        retire(device, budget, phase);
        phase = admit(device, budget);
        require(blend.blend->beginBlend({source, source, bloom::core::BlendMode::Normal}, kBudget)
                            .code == GpuBlendDiagnosticCode::None &&
                    budget.snapshot().ownedNativeBuffers == 1,
                "blend status is charged");
        require(wait(*blend.blend, GpuBlendPollResult::Pending) == GpuBlendPollResult::Ready,
                "blend real fence retires");
        {
            auto image = blend.blend->takeImage();
            checkPixels(image, color);
        }
        retire(device, budget, phase);
        phase = admit(device, budget);
        const auto proxyWindow = composite_proof::window(0, 0, 32, 16);
        const auto descriptor = Rgba32fImageDescriptor::create(
            proxyWindow, window, bloom::core::PixelAspectRatio::square());
        require(descriptor.hasValue() &&
                    point.resampler->begin({source, *descriptor.value(), 0.5, 0.5}, kBudget).code ==
                        GpuPointResampleDiagnosticCode::None &&
                    budget.snapshot().ownedNativeBuffers == 2,
                "point-resample owns axis metadata plus initialization staging");
        require(wait(*point.resampler, GpuPointResamplePollResult::Pending) ==
                        GpuPointResamplePollResult::Ready &&
                    budget.snapshot().ownedNativeBuffers == 0,
                "point-resample releases all job buffers at genuine completion");
        {
            auto image = point.resampler->take();
            checkPixels(image, color);
        }
        retire(device, budget, phase);
        VmaTotalStatistics settled{};
        vmaCalculateStatistics(state->allocator, &settled);
        require(settled.total.statistics.allocationBytes ==
                        baseline.total.statistics.allocationBytes &&
                    settled.total.statistics.allocationCount ==
                        baseline.total.statistics.allocationCount,
                "each complete operation cycle returns actual VMA payloads/count to baseline");
    }
}
void coverageOwnership(GpuDevice& device) {
    const auto window = composite_proof::window(0, 0, 32, 16);
    const auto color = composite_proof::pixel(0.25F, 0.5F, 1.0F, 1.0F);
    const auto raster = PathRaster::create(rectanglePath(20.5, 10.5), {}, 1, 1);
    require(raster.hasValue(), "fractional CPU raster reference prepared");
    const auto geometry =
        raster.value()->coverageGeometry(0, 0, 32, 16, PathFillRule::NonZero, false);
    require(geometry.hasValue(), "bounded coverage geometry prepared");
    std::vector<std::uint8_t> expected(512);
    for (std::int64_t y = 0; y != 16; ++y)
        require(raster.value()->coverageRow(
                    0, y, std::span(expected).subspan(static_cast<std::size_t>(y) * 32, 32),
                    PathFillRule::NonZero, false),
                "independent CPU fractional coverage row prepared");
    GpuProcessingPhaseBudget budget(1);
    budget.setHostCapacity(kBudget, true);
    budget.setDeviceCapacity(kBudget, true);
    auto producer = GpuPathCoverage::create(device);
    auto solid = GpuSolid::create(device);
    require(producer && solid, "coverage and consumer wrappers created");
    auto phase = admit(device, budget);
    require(producer.coverage->begin({window, window}, *geometry.value(), kBudget).code ==
                    GpuPathCoverageDiagnosticCode::None &&
                budget.snapshot().ownedNativeBuffers == 3,
            "ranges, spans and output mask each retain actual native charge");
    require(wait(*producer.coverage, GpuPathCoveragePollResult::Pending) ==
                    GpuPathCoveragePollResult::Ready &&
                budget.snapshot().ownedNativeBuffers == 1,
            "completed coverage retires geometry and publishes only resident mask");
    require(producer.coverage->readback(kBudget).coverage == expected,
            "native coverage exactly matches independent CPU raster");
    const auto maskBytes = gpuPathCoverageMask(*producer.coverage)->storageToken.bytes();
    device.retireProcessingPhase(phase->id(), true);
    phase.reset();
    require(budget.snapshot().admittedPhases == 0 &&
                budget.snapshot().totalDeviceClaimBytes == maskBytes,
            "published mask retains its exact claim without pinning originating phase");
    phase = admit(device, budget);
    require(
        solid.solid->beginCoveredResident({color, window, window}, *producer.coverage, 1, kBudget)
                    .code == GpuSolidDiagnosticCode::None &&
            budget.snapshot().ownedNativeBuffers == 2,
        "consumer shares resident mask once and owns one palette buffer");
    producer.coverage.reset();
    require(budget.snapshot().ownedNativeBuffers == 2,
            "consumer retains mask backing after producer destruction");
    require(wait(*solid.solid, GpuSolidPollResult::Pending) == GpuSolidPollResult::Ready,
            "covered solid real fence retires");
    {
        auto image = solid.solid->takeImage();
        const auto actual = readbackResidentImage(image, kBudget);
        std::vector<Rgba32f> reference(512, Rgba32f::transparent());
        require(
            !coverageSolidRow(expected, color, reference) && actual.hasValue() &&
                actual.pixels == reference && budget.snapshot().ownedNativeBuffers == 0,
            "covered native pixels are bit-exact and taking result releases palette/shared mask");
    }
    retire(device, budget, phase);
    producer = GpuPathCoverage::create(device);
    phase = admit(device, budget);
    require(producer.coverage->begin({window, window}, *geometry.value(), kBudget).code ==
                    GpuPathCoverageDiagnosticCode::None &&
                wait(*producer.coverage, GpuPathCoveragePollResult::Pending) ==
                    GpuPathCoveragePollResult::Ready,
            "second completed mask prepared for foreign last release");
    auto mask = gpuPathCoverageMask(*producer.coverage);
    producer.coverage.reset();
    device.retireProcessingPhase(phase->id(), true);
    phase.reset();
    const auto held = budget.snapshot();
    std::thread foreign([mask = std::move(mask)]() mutable { mask.reset(); });
    foreign.join();
    require(budget.snapshot().ownedNativeBuffers == 1 &&
                budget.snapshot().ownedNativeBufferBytes == held.ownedNativeBufferBytes,
            "foreign final mask release retains backing and exact charge for native owner");
    path_coverage_detail::drainMaskOwnersOnOwnerThread();
    require(budget.snapshot().ownedNativeBuffers == 0 &&
                budget.snapshot().totalDeviceClaimBytes == 0 &&
                budget.snapshot().totalHostClaimBytes == 0,
            "exact owner drains foreign mask backing before independent charge release");
}
void unknownForeignComposite(GpuDevice& device) {
    const auto window = composite_proof::window(0, 0, 32, 16);
    auto solid = GpuSolid::create(device);
    auto sourceImage = composite_proof::solid(*solid.solid, {0.25, 0.5, 1, 1}, window, window,
                                              bloom::core::PixelAspectRatio::square());
    if (!sourceImage)
        throw std::runtime_error("foreign-retirement input preparation failed");
    auto source = std::make_shared<const GpuImage>(std::move(*sourceImage));
    auto operation = GpuComposite::create(device);
    GpuProcessingPhaseBudget budget(1);
    budget.setHostCapacity(kBudget, true);
    budget.setDeviceCapacity(kBudget, true);
    auto phase = admit(device, budget);
    require(operation.composite->beginTranslation({source, window, 0, 0, 1, std::nullopt}, kBudget).code ==
                GpuCompositeDiagnosticCode::None,
            "real rooted submission prepared before foreign destruction");
    composite_detail::compositeRetirementFault().store(
        static_cast<std::uint8_t>(composite_detail::CompositeRetirementFault::ForceFenceTimeout));
    const auto held = budget.snapshot();
    const auto state = GpuRendererAccess::state(device);
    const auto destroyed = state->nativeBufferBackingsDestroyed;
    std::thread foreign(
        [operation = std::move(operation.composite)]() mutable { operation.reset(); });
    foreign.join();
    auto probe = GpuComposite::create(device);
    require(
        probe && budget.snapshot().ownedNativeBuffers == held.ownedNativeBuffers &&
            budget.snapshot().ownedNativeBufferBytes == held.ownedNativeBufferBytes &&
            state->nativeBufferBackingsDestroyed == destroyed,
        "unknown foreign submission preserves entire native backing and tokens in original slot");
    composite_detail::compositeRetirementFault().store(
        static_cast<std::uint8_t>(composite_detail::CompositeRetirementFault::None));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (budget.snapshot().ownedNativeBuffers != 0 &&
           std::chrono::steady_clock::now() < deadline) {
        probe = GpuComposite::create(device);
        std::this_thread::yield();
    }
    require(budget.snapshot().ownedNativeBuffers == 0 &&
                state->nativeBufferBackingsDestroyed == destroyed + held.ownedNativeBuffers,
            "genuine owner proof destroys all orphaned backing before token foldback");
    retire(device, budget, phase);
}
} // namespace

int main(int argc, char** argv) {
    try {
        const auto options = composite_proof::parseOptions(argc, argv);
        if (!options.valid)
            return 2;
        auto device = GpuDevice::create({.loader_path = options.loader_path});
        if (!device) {
            std::cout << "Unsupported: " << device.diagnostic.message << '\n';
            return options.require_device ? 1 : 77;
        }
        helperOwnership(*device.device);
        operationCycles(*device.device);
        coverageOwnership(*device.device);
        unknownForeignComposite(*device.device);
        std::cout
            << "PASS: rooted composite, affine, blend, resample, coverage and palette buffers\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
