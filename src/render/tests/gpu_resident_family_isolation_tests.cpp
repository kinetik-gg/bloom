// Mixed-family bounded-pool isolation regression.
//
// Every one of the five render families owns a DISTINCT tagged resident pool. This vector proves it
// with the two families that historically shared one store: Solid/CoveredSolidV1 and
// GpuImageUpload. It concurrently populates both on the same owner/device, foreign-releases them,
// then drains exactly one family and asserts only that family's orphan is retired and its counters
// move. If the two families shared a store, one family's drain would reinterpret the other's
// private Impl (a type-confusion UB), and the other family's accounting would move wrongly; this
// test fails or crashes in that case. The remaining three families are checked to stay untouched
// throughout, so all five pools are shown independent.
//
// The private fault seams are Vulkan-free, so this target exists only where the Vulkan backend is
// built; a hardware-free build prints an explicit skip and --require-device fails closed.

#include "gpu_composite_native_support.hpp"

#include "gpu_affine_fault.hpp"
#include "gpu_blend_fault.hpp"
#include "gpu_composite_fault.hpp"
#include "gpu_image_upload_fault.hpp"
#include "gpu_solid_fault.hpp"

#include <bloom/render/gpu_affine.hpp>
#include <bloom/render/gpu_blend.hpp>
#include <bloom/render/gpu_composite.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace bloom::render::composite_proof;
using bloom::render::GpuDevice;
using bloom::render::GpuDeviceCreationOptions;
using bloom::render::GpuImageUpload;
using bloom::render::GpuImageUploadDiagnosticCode;
using bloom::render::GpuImageUploadPollResult;
using bloom::render::GpuSolid;
using bloom::render::GpuSolidDiagnosticCode;
using bloom::render::GpuSolidParameters;
using bloom::render::GpuSolidPollResult;
using bloom::render::ImageWindow;
using bloom::render::Rgba32fImage;

namespace affine_detail = bloom::render::affine_detail;
namespace blend_detail = bloom::render::blend_detail;
namespace composite_detail = bloom::render::composite_detail;
namespace solid_detail = bloom::render::solid_detail;
namespace upload_detail = bloom::render::upload_detail;

struct Baseline final {
    std::size_t solidInUse = 0;
    std::size_t uploadInUse = 0;
    std::size_t compositeInUse = 0;
    std::size_t blendInUse = 0;
    std::size_t affineInUse = 0;
    std::size_t solidOrphaned = 0;
    std::size_t uploadOrphaned = 0;
    std::size_t compositeOrphaned = 0;
    std::size_t blendOrphaned = 0;
    std::size_t affineOrphaned = 0;
    std::uint64_t solidRetired = 0;
    std::uint64_t uploadRetired = 0;
};

[[nodiscard]] Baseline capture() noexcept {
    Baseline value;
    value.solidInUse = solid_detail::solidResidentInUse();
    value.uploadInUse = upload_detail::uploadResidentInUse();
    value.compositeInUse = composite_detail::compositeResidentInUse();
    value.blendInUse = blend_detail::blendResidentInUse();
    value.affineInUse = affine_detail::affineResidentInUse();
    value.solidOrphaned = solid_detail::solidResidentOrphaned();
    value.uploadOrphaned = upload_detail::uploadResidentOrphaned();
    value.compositeOrphaned = composite_detail::compositeResidentOrphaned();
    value.blendOrphaned = blend_detail::blendResidentOrphaned();
    value.affineOrphaned = affine_detail::affineResidentOrphaned();
    value.solidRetired = solid_detail::solidResidentRetired();
    value.uploadRetired = upload_detail::uploadResidentRetired();
    return value;
}

void expectOtherFamiliesUntouched(Expectations& expectations, const Baseline& baseline,
                                  const std::string& label) {
    expectations.expect(composite_detail::compositeResidentInUse() == baseline.compositeInUse &&
                            blend_detail::blendResidentInUse() == baseline.blendInUse &&
                            affine_detail::affineResidentInUse() == baseline.affineInUse,
                        label + ": composite/blend/affine in-use counters are untouched");
    expectations.expect(composite_detail::compositeResidentOrphaned() ==
                                baseline.compositeOrphaned &&
                            blend_detail::blendResidentOrphaned() == baseline.blendOrphaned &&
                            affine_detail::affineResidentOrphaned() == baseline.affineOrphaned,
                        label + ": composite/blend/affine orphan counters are untouched");
}

[[nodiscard]] bool runSolidReady(GpuSolid& solid) {
    const auto window = ImageWindow::create(0, 0, 8, 4);
    if (!window) {
        return false;
    }
    const auto pixel = bloom::render::solidPixelFromStraightLinearRec709Scene(
        bloom::core::Color4d{0.2, 0.4, 0.6, 1.0});
    if (!pixel) {
        return false;
    }
    const GpuSolidParameters parameters{*pixel.value(), *window.value(), *window.value(),
                                        bloom::core::PixelAspectRatio::square()};
    if (solid.begin(parameters, kBudget).code != GpuSolidDiagnosticCode::None) {
        return false;
    }
    GpuSolidPollResult poll = GpuSolidPollResult::Pending;
    while (poll == GpuSolidPollResult::Pending) {
        poll = solid.poll();
    }
    return poll == GpuSolidPollResult::Ready;
}

[[nodiscard]] bool runUploadReady(GpuImageUpload& upload,
                                  const std::shared_ptr<const Rgba32fImage>& source) {
    if (upload.begin({source}, kBudget).code != GpuImageUploadDiagnosticCode::None) {
        return false;
    }
    GpuImageUploadPollResult poll = GpuImageUploadPollResult::Pending;
    while (poll == GpuImageUploadPollResult::Pending) {
        poll = upload.poll();
    }
    return poll == GpuImageUploadPollResult::Ready;
}

template <typename Op>
void hold(std::vector<std::shared_ptr<void>>& owners, std::unique_ptr<Op> op) {
    owners.push_back(
        std::shared_ptr<void>(op.release(), [](void* const raw) { delete static_cast<Op*>(raw); }));
}

} // namespace

int main(const int argc, char** argv) {
    try {
        const Options options = parseOptions(argc, argv);
        if (!options.valid) {
            return 2;
        }
        Expectations expectations;

        GpuDeviceCreationOptions createOptions;
        createOptions.loader_path = options.loader_path;
        auto device = GpuDevice::create(createOptions);
        if (!device) {
            if (options.require_device) {
                std::cerr << "FAIL: required device unavailable: " << device.diagnostic.message
                          << '\n';
                return 1;
            }
            std::cout << "SKIP: no compatible Vulkan device available: "
                      << device.diagnostic.message << '\n';
            return expectations.failures() == 0 ? 0 : 1;
        }
        GpuDevice& gpu = *device.device;

        const Baseline baseline = capture();
        expectations.expect(solid_detail::solidResidentCapacity() > 0 &&
                                upload_detail::uploadResidentCapacity() > 0 &&
                                composite_detail::compositeResidentCapacity() > 0 &&
                                blend_detail::blendResidentCapacity() > 0 &&
                                affine_detail::affineResidentCapacity() > 0,
                            "all five families report a positive bounded capacity");

        // Populate BOTH families to Ready on the same owner/device.
        constexpr int kPerFamily = 2;
        std::vector<std::shared_ptr<void>> owners;
        for (int index = 0; index < kPerFamily; ++index) {
            auto created = GpuSolid::create(gpu);
            expectations.expect(created.hasValue(), "mixed: solid created");
            if (!created) {
                return 1;
            }
            expectations.expect(runSolidReady(*created.solid), "mixed: solid reaches Ready");
            hold(owners, std::move(created.solid));
        }

        const auto sourceWindow = window(0, 0, 8, 4);
        auto sourceImage =
            makeImage(sourceWindow, sourceWindow, PixelAspectRatio::square(), semanticPixels(8, 4));
        expectations.expect(sourceImage.has_value(), "mixed: upload source builds");
        if (!sourceImage) {
            return 1;
        }
        auto source = std::make_shared<const Rgba32fImage>(std::move(*sourceImage));
        for (int index = 0; index < kPerFamily; ++index) {
            auto created = GpuImageUpload::create(gpu);
            expectations.expect(created.hasValue(), "mixed: upload created");
            if (!created) {
                return 1;
            }
            expectations.expect(runUploadReady(*created.upload, source),
                                "mixed: upload reaches Ready");
            hold(owners, std::move(created.upload));
        }

        expectations.expect(
            solid_detail::solidResidentInUse() == baseline.solidInUse + kPerFamily &&
                upload_detail::uploadResidentInUse() == baseline.uploadInUse + kPerFamily,
            "mixed: both families hold their own ready slots");
        expectOtherFamiliesUntouched(expectations, baseline, "mixed populate");

        // Foreign-release both families at once.
        std::thread worker([&owners]() { owners.clear(); });
        worker.join();
        expectations.expect(
            solid_detail::solidResidentOrphaned() == baseline.solidOrphaned + kPerFamily &&
                upload_detail::uploadResidentOrphaned() == baseline.uploadOrphaned + kPerFamily,
            "mixed: both families orphan their own slots");
        expectOtherFamiliesUntouched(expectations, baseline, "mixed foreign release");

        // Drain ONLY Solid. Upload must remain entirely untouched, and no cross-cast may occur.
        (void)GpuSolid::create(gpu);
        expectations.expect(solid_detail::solidResidentOrphaned() == baseline.solidOrphaned,
                            "mixed: the solid drain retires exactly the solid orphans");
        expectations.expect(solid_detail::solidResidentRetired() ==
                                baseline.solidRetired + kPerFamily,
                            "mixed: the solid drain retired its own count");
        expectations.expect(upload_detail::uploadResidentOrphaned() ==
                                    baseline.uploadOrphaned + kPerFamily &&
                                upload_detail::uploadResidentRetired() == baseline.uploadRetired,
                            "mixed: the solid drain leaves every upload orphan untouched");
        expectations.expect(upload_detail::uploadResidentInUse() ==
                                baseline.uploadInUse + kPerFamily,
                            "mixed: the solid drain does not release an upload slot");
        expectOtherFamiliesUntouched(expectations, baseline, "mixed solid-only drain");

        // Drain Upload: it retires only its own remaining orphans.
        (void)GpuImageUpload::create(gpu);
        expectations.expect(upload_detail::uploadResidentOrphaned() == baseline.uploadOrphaned,
                            "mixed: the upload drain retires exactly the upload orphans");
        expectations.expect(upload_detail::uploadResidentRetired() ==
                                baseline.uploadRetired + kPerFamily,
                            "mixed: the upload drain retired its own count");
        expectations.expect(solid_detail::solidResidentInUse() == baseline.solidInUse &&
                                solid_detail::solidResidentOrphaned() == baseline.solidOrphaned,
                            "mixed: the upload drain leaves solid accounting at baseline");
        expectOtherFamiliesUntouched(expectations, baseline, "mixed upload drain");

        // All five pools are back to their independent baselines.
        expectations.expect(solid_detail::solidResidentInUse() == baseline.solidInUse &&
                                upload_detail::uploadResidentInUse() == baseline.uploadInUse,
                            "mixed: both drained families return to baseline");
        expectations.expect(!GpuSolid::teardownDrainIncomplete() &&
                                !GpuImageUpload::teardownDrainIncomplete(),
                            "mixed: neither family reports incomplete retirement");

        if (expectations.failures() != 0) {
            std::cerr << "FAIL: retirement pool isolation expectations failed\n";
            return 1;
        }
        std::cout << "PASS: mixed-family retirement pool isolation\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
