// Deterministic fault-injection and boundedness tests for the PointResampleV1 reservation:
//  * forced job-allocation and submit failures fail closed with no occupied reservation and stay
//    reusable;
//  * a forced fence timeout quarantines the exact submission (bounded, retained), refuses new
//    admission until it retires, then recovers after genuine owner retirement;
//  * a forced device loss fails closed and frees the reservation;
//  * many pre-created instances never grow retained native resources beyond the single reservation;
//  * a forced small X workgroup limit exercises the real 2D dispatch tail with bit-exact parity;
//  * a foreign-thread destruction retains the one occupied reservation (fail closed).
//
// Links the test-only fault library; production targets never link it.

#include "gpu_composite_native_support.hpp"

#include "gpu_point_resample_fault.hpp"

#include <bloom/render/gpu_point_resample.hpp>

#include <bit>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace bloom::render::composite_proof;
using bloom::core::PixelAspectRatio;
using bloom::render::GpuPointResample;
using bloom::render::GpuPointResampleDiagnosticCode;
using bloom::render::GpuPointResamplePollResult;
using bloom::render::GpuPointResampleRequest;
using bloom::render::ImageWindow;
using bloom::render::Rgba32f;
using bloom::render::Rgba32fImage;
using bloom::render::Rgba32fImageDescriptor;
using bloom::render::point_resample_detail::PointResampleFault;
using bloom::render::point_resample_detail::pointResampleLiveResourceSetsForTest;
using bloom::render::point_resample_detail::pointResampleQuarantineOccupiedForTest;
using bloom::render::point_resample_detail::retirePointResampleQuarantineForOwnerForTest;
using bloom::render::point_resample_detail::setPointResampleFaultForTest;
using bloom::render::point_resample_detail::setPointResampleForcedMaxWorkGroupCountXForTest;

[[nodiscard]] std::shared_ptr<const GpuImage>
makeSource(GpuImageUpload& uploader, const std::uint32_t width, const std::uint32_t height) {
    auto image = makeImage(window(0, 0, width, height), window(0, 0, width, height),
                           PixelAspectRatio::square(), denseHdrPixels(width, height));
    if (!image) {
        return nullptr;
    }
    auto resident = upload(uploader, std::make_shared<const Rgba32fImage>(std::move(*image)));
    if (!resident) {
        return nullptr;
    }
    return std::make_shared<const GpuImage>(std::move(*resident));
}

[[nodiscard]] std::optional<GpuPointResampleRequest>
makeRequest(const std::shared_ptr<const GpuImage>& source, const std::uint32_t outputWidth,
            const std::uint32_t outputHeight, const double horizontalScale,
            const double verticalScale) {
    const auto dataWindow = ImageWindow::create(0, 0, outputWidth, outputHeight);
    if (!dataWindow) {
        return std::nullopt;
    }
    const auto descriptor = Rgba32fImageDescriptor::create(
        *dataWindow.value(), window(3, -5, outputWidth + 6, outputHeight + 10),
        PixelAspectRatio::create(2, 1).value());
    if (!descriptor) {
        return std::nullopt;
    }
    return GpuPointResampleRequest{source, *descriptor.value(), horizontalScale, verticalScale};
}

[[nodiscard]] GpuPointResamplePollResult drain(GpuPointResample& resampler) {
    auto poll = resampler.poll();
    while (poll == GpuPointResamplePollResult::Pending) {
        poll = resampler.poll();
    }
    return poll;
}

[[nodiscard]] std::int32_t oracleAxis(const std::uint32_t index, const std::uint32_t extent,
                                      const double scale) {
    const double mapped = static_cast<double>(index) / scale;
    const double maximum = static_cast<double>(extent - 1U);
    return static_cast<std::int32_t>(
        static_cast<std::uint32_t>(mapped > maximum ? maximum : mapped));
}

void testJobAllocationFault(Expectations& expectations, GpuDevice& device) {
    auto uploader = bloom::render::GpuImageUpload::create(device);
    const auto source = uploader ? makeSource(*uploader.upload, 8, 4) : nullptr;
    const auto request = source ? makeRequest(source, 8, 4, 1.0, 1.0) : std::nullopt;
    if (!request) {
        expectations.expect(false, "alloc-fault: fixture built");
        return;
    }
    auto resampler = GpuPointResample::create(device);
    if (!resampler) {
        expectations.expect(false, "alloc-fault: instance created");
        return;
    }
    setPointResampleFaultForTest(PointResampleFault::FailJobAllocation);
    const auto refused = resampler.resampler->begin(*request, kBudget);
    expectations.expect(refused.code == GpuPointResampleDiagnosticCode::AllocationFailed,
                        "alloc-fault: a forced allocation failure is typed");
    expectations.expect(!pointResampleQuarantineOccupiedForTest(),
                        "alloc-fault: no reservation is left occupied");
    expectations.expect(pointResampleLiveResourceSetsForTest() == 0,
                        "alloc-fault: no native resource set is retained");
    setPointResampleFaultForTest(PointResampleFault::None);
    expectations.expect(resampler.resampler->begin(*request, kBudget).code ==
                            GpuPointResampleDiagnosticCode::None,
                        "alloc-fault: the instance is reusable after the fault clears");
    expectations.expect(drain(*resampler.resampler) == GpuPointResamplePollResult::Ready,
                        "alloc-fault: the recovered job completes");
    static_cast<void>(resampler.resampler->take());
}

void testSubmitFault(Expectations& expectations, GpuDevice& device) {
    auto uploader = bloom::render::GpuImageUpload::create(device);
    const auto source = uploader ? makeSource(*uploader.upload, 8, 4) : nullptr;
    const auto request = source ? makeRequest(source, 8, 4, 1.0, 1.0) : std::nullopt;
    if (!request) {
        expectations.expect(false, "submit-fault: fixture built");
        return;
    }
    auto resampler = GpuPointResample::create(device);
    if (!resampler) {
        expectations.expect(false, "submit-fault: instance created");
        return;
    }
    setPointResampleFaultForTest(PointResampleFault::FailSubmit);
    const auto refused = resampler.resampler->begin(*request, kBudget);
    expectations.expect(refused.code == GpuPointResampleDiagnosticCode::DeviceUnavailable,
                        "submit-fault: a forced submit failure is typed");
    expectations.expect(!pointResampleQuarantineOccupiedForTest() &&
                            pointResampleLiveResourceSetsForTest() == 0,
                        "submit-fault: nothing is retained");
    setPointResampleFaultForTest(PointResampleFault::None);
    expectations.expect(resampler.resampler->begin(*request, kBudget).code ==
                            GpuPointResampleDiagnosticCode::None,
                        "submit-fault: the instance is reusable after the fault clears");
    expectations.expect(drain(*resampler.resampler) == GpuPointResamplePollResult::Ready,
                        "submit-fault: the recovered job completes");
    static_cast<void>(resampler.resampler->take());
}

void testFenceTimeoutQuarantine(Expectations& expectations, GpuDevice& device) {
    auto uploader = bloom::render::GpuImageUpload::create(device);
    const auto source = uploader ? makeSource(*uploader.upload, 8, 4) : nullptr;
    const auto request = source ? makeRequest(source, 8, 4, 1.0, 1.0) : std::nullopt;
    if (!request) {
        expectations.expect(false, "timeout: fixture built");
        return;
    }
    auto first = GpuPointResample::create(device);
    if (!first) {
        expectations.expect(false, "timeout: instance created");
        return;
    }
    setPointResampleFaultForTest(PointResampleFault::ForceFenceTimeout);
    expectations.expect(first.resampler->begin(*request, kBudget).code ==
                            GpuPointResampleDiagnosticCode::None,
                        "timeout: begin accepted");
    expectations.expect(drain(*first.resampler) == GpuPointResamplePollResult::Failure &&
                            first.resampler->diagnostic().code ==
                                GpuPointResampleDiagnosticCode::NativeTimeout,
                        "timeout: an unproven submission fails closed");
    expectations.expect(pointResampleQuarantineOccupiedForTest(),
                        "timeout: the exact submission is retained in the bounded quarantine");
    expectations.expect(pointResampleLiveResourceSetsForTest() == 1,
                        "timeout: exactly one native resource set is retained");

    // New admission is refused while the quarantine is un-retirable.
    auto second = GpuPointResample::create(device);
    if (!second) {
        expectations.expect(false, "timeout: second instance created");
        return;
    }
    const auto refused = second.resampler->begin(*request, kBudget);
    expectations.expect(refused.code == GpuPointResampleDiagnosticCode::DeviceUnavailable,
                        "timeout: admission is refused while the quarantine is occupied");

    // Clearing the fault lets the genuine owner prove retirement and free the quarantine.
    setPointResampleFaultForTest(PointResampleFault::None);
    bool retired = false;
    for (int attempt = 0; attempt < 400 && !retired; ++attempt) {
        retired = retirePointResampleQuarantineForOwnerForTest();
        if (!retired) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    expectations.expect(retired, "timeout: the owner retires the quarantine after fence proof");
    expectations.expect(!pointResampleQuarantineOccupiedForTest() &&
                            pointResampleLiveResourceSetsForTest() == 0,
                        "timeout: the quarantine is freed after retirement");
    expectations.expect(second.resampler->begin(*request, kBudget).code ==
                            GpuPointResampleDiagnosticCode::None,
                        "timeout: admission recovers after retirement");
    expectations.expect(drain(*second.resampler) == GpuPointResamplePollResult::Ready,
                        "timeout: the recovered job completes");
    static_cast<void>(second.resampler->take());
}

void testDeviceLost(Expectations& expectations, GpuDevice& device) {
    auto uploader = bloom::render::GpuImageUpload::create(device);
    const auto source = uploader ? makeSource(*uploader.upload, 8, 4) : nullptr;
    const auto request = source ? makeRequest(source, 8, 4, 1.0, 1.0) : std::nullopt;
    if (!request) {
        expectations.expect(false, "device-loss: fixture built");
        return;
    }
    auto resampler = GpuPointResample::create(device);
    if (!resampler) {
        expectations.expect(false, "device-loss: instance created");
        return;
    }
    setPointResampleFaultForTest(PointResampleFault::ForceDeviceLost);
    expectations.expect(resampler.resampler->begin(*request, kBudget).code ==
                            GpuPointResampleDiagnosticCode::None,
                        "device-loss: begin accepted");
    expectations.expect(drain(*resampler.resampler) == GpuPointResamplePollResult::Failure &&
                            resampler.resampler->diagnostic().code ==
                                GpuPointResampleDiagnosticCode::DeviceLost,
                        "device-loss: the poll reports DeviceLost");
    expectations.expect(!pointResampleQuarantineOccupiedForTest() &&
                            pointResampleLiveResourceSetsForTest() == 0,
                        "device-loss: the reservation is freed, not quarantined");
    setPointResampleFaultForTest(PointResampleFault::None);

    // A fresh instance on the same (real) device recovers after the fault clears; the lost
    // generation instance stays failed.
    auto recovered = GpuPointResample::create(device);
    if (!recovered) {
        expectations.expect(false, "device-loss: recovery instance created");
        return;
    }
    expectations.expect(recovered.resampler->begin(*request, kBudget).code ==
                            GpuPointResampleDiagnosticCode::None,
                        "device-loss: a fresh instance begins after the fault clears");
    expectations.expect(drain(*recovered.resampler) == GpuPointResamplePollResult::Ready,
                        "device-loss: the fresh instance completes");
    static_cast<void>(recovered.resampler->take());
}

void testManyPrecreatedInstancesBounded(Expectations& expectations, GpuDevice& device) {
    auto uploader = bloom::render::GpuImageUpload::create(device);
    const auto source = uploader ? makeSource(*uploader.upload, 8, 4) : nullptr;
    const auto request = source ? makeRequest(source, 8, 4, 1.0, 1.0) : std::nullopt;
    if (!request) {
        expectations.expect(false, "bounded: fixture built");
        return;
    }
    std::vector<std::unique_ptr<GpuPointResample>> instances;
    for (int index = 0; index < 32; ++index) {
        auto created = GpuPointResample::create(device);
        expectations.expect(created.hasValue(), "bounded: a pre-created instance succeeds");
        instances.push_back(std::move(created.resampler));
    }
    expectations.expect(pointResampleLiveResourceSetsForTest() == 0,
                        "bounded: pre-created instances retain no native resources");

    expectations.expect(instances.front()->begin(*request, kBudget).code ==
                            GpuPointResampleDiagnosticCode::None,
                        "bounded: the first instance begins");
    expectations.expect(pointResampleLiveResourceSetsForTest() == 1,
                        "bounded: exactly one native resource set is live");
    for (std::size_t index = 1; index < instances.size(); ++index) {
        const auto refused = instances[index]->begin(*request, kBudget);
        expectations.expect(refused.code == GpuPointResampleDiagnosticCode::Busy,
                            "bounded: a concurrent instance is refused");
    }
    expectations.expect(pointResampleLiveResourceSetsForTest() == 1,
                        "bounded: refused concurrent instances grow nothing");
    expectations.expect(drain(*instances.front()) == GpuPointResamplePollResult::Ready,
                        "bounded: the first instance completes");
    static_cast<void>(instances.front()->take());
    expectations.expect(pointResampleLiveResourceSetsForTest() == 0,
                        "bounded: the native resource set is freed at retirement");
}

[[nodiscard]] bool bitsEqual(const Rgba32f& lhs, const Rgba32f& rhs) {
    for (std::size_t index = 0; index < 4; ++index) {
        if (std::bit_cast<std::uint32_t>(lhs.components()[index]) !=
            std::bit_cast<std::uint32_t>(rhs.components()[index])) {
            return false;
        }
    }
    return true;
}

void testForced2DTailParity(Expectations& expectations, GpuDevice& device) {
    constexpr std::uint32_t kWidth = 64;
    constexpr std::uint32_t kHeight = 32;
    auto uploader = bloom::render::GpuImageUpload::create(device);
    const auto source = uploader ? makeSource(*uploader.upload, kWidth, kHeight) : nullptr;
    const auto request = source ? makeRequest(source, kWidth, kHeight, 1.0, 1.0) : std::nullopt;
    if (!request) {
        expectations.expect(false, "2d-tail: fixture built");
        return;
    }
    auto resampler = GpuPointResample::create(device);
    if (!resampler) {
        expectations.expect(false, "2d-tail: instance created");
        return;
    }
    // maxX = 3 forces groupsX=3, groupsY=3 for the 2048-pixel frame: a genuine 2D tail.
    setPointResampleForcedMaxWorkGroupCountXForTest(3);
    expectations.expect(resampler.resampler->begin(*request, kBudget).code ==
                            GpuPointResampleDiagnosticCode::None,
                        "2d-tail: begin accepted under a forced small maxX");
    expectations.expect(drain(*resampler.resampler) == GpuPointResamplePollResult::Ready,
                        "2d-tail: the forced 2D dispatch completes");
    auto result = resampler.resampler->take();
    setPointResampleForcedMaxWorkGroupCountXForTest(0);
    const auto readback = bloom::render::readbackResidentImage(result, kBudget);
    expectations.expect(readback.hasValue(), "2d-tail: readback for oracle");
    if (!readback) {
        return;
    }
    const auto pixels = denseHdrPixels(kWidth, kHeight);
    std::vector<Rgba32f> expected(static_cast<std::size_t>(kWidth) * kHeight,
                                  Rgba32f::transparent());
    for (std::uint32_t y = 0; y < kHeight; ++y) {
        const auto sourceY = static_cast<std::uint32_t>(oracleAxis(y, kHeight, 1.0));
        for (std::uint32_t x = 0; x < kWidth; ++x) {
            const auto sourceX = static_cast<std::uint32_t>(oracleAxis(x, kWidth, 1.0));
            expected[static_cast<std::size_t>(y) * kWidth + x] =
                pixels[static_cast<std::size_t>(sourceY) * kWidth + sourceX];
        }
    }
    bool exact = readback.pixels.size() == expected.size();
    for (std::size_t index = 0; exact && index < expected.size(); ++index) {
        exact = bitsEqual(readback.pixels[index], expected[index]);
    }
    expectations.expect(exact, "2d-tail: every pixel is bit-exact under the flattened 2D dispatch");
}

// An idle instance holds no native resources, so a foreign-thread destruction is safe and frees
// nothing that must be retained.
void testIdleForeignThreadDestruction(Expectations& expectations, GpuDevice& device) {
    auto created = GpuPointResample::create(device);
    if (!created) {
        expectations.expect(false, "idle-dtor: instance created");
        return;
    }
    auto* const resampler = created.resampler.release();
    std::thread foreign([resampler] { delete resampler; });
    foreign.join();
    expectations.expect(!pointResampleQuarantineOccupiedForTest() &&
                            pointResampleLiveResourceSetsForTest() == 0,
                        "idle-dtor: an idle foreign destruction retains nothing");
}

// Run last: a foreign-thread destruction of a live submission retains the single occupied
// reservation (fail closed), so no further admission succeeds for the rest of this process.
void testForeignThreadDestructionRetains(Expectations& expectations, GpuDevice& device) {
    auto uploader = bloom::render::GpuImageUpload::create(device);
    const auto source = uploader ? makeSource(*uploader.upload, 8, 4) : nullptr;
    const auto request = source ? makeRequest(source, 8, 4, 1.0, 1.0) : std::nullopt;
    if (!request) {
        expectations.expect(false, "foreign-dtor: fixture built");
        return;
    }
    auto created = GpuPointResample::create(device);
    if (!created) {
        expectations.expect(false, "foreign-dtor: instance created");
        return;
    }
    auto* const resampler = created.resampler.release();
    expectations.expect(resampler->begin(*request, kBudget).code ==
                            GpuPointResampleDiagnosticCode::None,
                        "foreign-dtor: begin accepted on the owner thread");
    std::thread foreign([resampler] { delete resampler; });
    foreign.join();
    expectations.expect(!pointResampleQuarantineOccupiedForTest(),
                        "foreign-dtor: the retained submission is reserved, not quarantined");
    expectations.expect(pointResampleLiveResourceSetsForTest() == 1,
                        "foreign-dtor: exactly one native resource set is retained");
    auto refused = GpuPointResample::create(device);
    if (!refused) {
        expectations.expect(false, "foreign-dtor: probe instance created");
        return;
    }
    const auto began = refused.resampler->begin(*request, kBudget);
    expectations.expect(began.code == GpuPointResampleDiagnosticCode::Busy,
                        "foreign-dtor: admission fails closed while the reservation is held");
}

} // namespace

int main(int argc, char** argv) {
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
            return 0;
        }
        testJobAllocationFault(expectations, *device.device);
        testSubmitFault(expectations, *device.device);
        testFenceTimeoutQuarantine(expectations, *device.device);
        testDeviceLost(expectations, *device.device);
        testManyPrecreatedInstancesBounded(expectations, *device.device);
        testForced2DTailParity(expectations, *device.device);
        testIdleForeignThreadDestruction(expectations, *device.device);
        testForeignThreadDestructionRetains(expectations, *device.device);
        if (expectations.failures() != 0) {
            std::cerr << expectations.failures() << " point-resample fault expectation(s) failed\n";
            return 1;
        }
        std::cout << "PASS: point resample lifecycle faults and 2D dispatch\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
