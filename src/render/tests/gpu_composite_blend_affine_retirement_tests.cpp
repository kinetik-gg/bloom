// Bounded native-resource-retirement tests for GpuComposite, GpuBlend, and GpuAffine.
//
// These vectors prove the process-global bounded resident pools: many pre-created instances beyond
// capacity, foreign-thread destruction boundedness, retention of an unproven fence, exact
// owner-thread/device-generation isolation across two real device threads, and owner-drain
// recovery. The private fault seams are Vulkan-free, so this target exists only where the Vulkan
// backend is built; a hardware-free build prints an explicit skip and --require-device fails
// closed.
//
// The three families each own a DISTINCT tagged pool. Sharing one pool would let a drain callback
// reinterpret another family's private Impl, so the tests assert per-family accounting.

#include "gpu_composite_native_support.hpp"

#include "gpu_affine_fault.hpp"
#include "gpu_blend_fault.hpp"
#include "gpu_composite_fault.hpp"

#include <bloom/render/gpu_affine.hpp>
#include <bloom/render/gpu_blend.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace bloom::render::composite_proof;
using bloom::core::BlendMode;
using bloom::render::GpuAffine;
using bloom::render::GpuAffineDiagnosticCode;
using bloom::render::GpuAffineMatrix;
using bloom::render::GpuAffinePollResult;
using bloom::render::GpuBlend;
using bloom::render::GpuBlendDiagnosticCode;
using bloom::render::GpuBlendPollResult;
using bloom::render::GpuComposite;
using bloom::render::GpuCompositeDiagnosticCode;
using bloom::render::GpuCompositePollResult;
using bloom::render::GpuDevice;
using bloom::render::GpuDeviceCreationOptions;
using bloom::render::GpuImage;
using bloom::render::GpuImageUpload;
using bloom::render::Rgba32fImage;

namespace affine_detail = bloom::render::affine_detail;
namespace blend_detail = bloom::render::blend_detail;
namespace composite_detail = bloom::render::composite_detail;

[[nodiscard]] std::optional<std::shared_ptr<const GpuImage>> makeSource(GpuImageUpload& uploader) {
    const auto sourceWindow = window(0, 0, 8, 4);
    auto image =
        makeImage(sourceWindow, sourceWindow, PixelAspectRatio::square(), semanticPixels(8, 4));
    if (!image) {
        return std::nullopt;
    }
    auto resident = upload(uploader, std::make_shared<const Rgba32fImage>(std::move(*image)));
    if (!resident) {
        return std::nullopt;
    }
    return std::make_shared<const GpuImage>(std::move(*resident));
}

[[nodiscard]] bool beginComposite(GpuComposite& composite,
                                  const std::shared_ptr<const GpuImage>& source) {
    if (composite.beginTranslation({source, window(0, 0, 8, 4), 0.0, 0.0, 1.0F}, kBudget).code !=
        GpuCompositeDiagnosticCode::None) {
        return false;
    }
    GpuCompositePollResult poll = GpuCompositePollResult::Pending;
    while (poll == GpuCompositePollResult::Pending) {
        poll = composite.poll();
    }
    return poll == GpuCompositePollResult::Ready;
}

[[nodiscard]] bool submitComposite(GpuComposite& composite,
                                   const std::shared_ptr<const GpuImage>& source) {
    return composite.beginTranslation({source, window(0, 0, 8, 4), 0.0, 0.0, 1.0F}, kBudget).code ==
           GpuCompositeDiagnosticCode::None;
}

[[nodiscard]] bool beginBlendOp(GpuBlend& blend, const std::shared_ptr<const GpuImage>& source) {
    if (blend.beginBlend({source, source, BlendMode::Normal}, kBudget).code !=
        GpuBlendDiagnosticCode::None) {
        return false;
    }
    GpuBlendPollResult poll = GpuBlendPollResult::Pending;
    while (poll == GpuBlendPollResult::Pending) {
        poll = blend.poll();
    }
    return poll == GpuBlendPollResult::Ready;
}

[[nodiscard]] bool submitBlendOp(GpuBlend& blend, const std::shared_ptr<const GpuImage>& source) {
    return blend.beginBlend({source, source, BlendMode::Normal}, kBudget).code ==
           GpuBlendDiagnosticCode::None;
}

[[nodiscard]] bool beginAffineOp(GpuAffine& affine, const std::shared_ptr<const GpuImage>& source) {
    if (affine.beginAffineMatrix({source, window(0, 0, 8, 4), GpuAffineMatrix{}, 1.0F}, kBudget)
            .code != GpuAffineDiagnosticCode::None) {
        return false;
    }
    GpuAffinePollResult poll = GpuAffinePollResult::Pending;
    while (poll == GpuAffinePollResult::Pending) {
        poll = affine.poll();
    }
    return poll == GpuAffinePollResult::Ready;
}

[[nodiscard]] bool submitAffineOp(GpuAffine& affine,
                                  const std::shared_ptr<const GpuImage>& source) {
    return affine.beginAffineMatrix({source, window(0, 0, 8, 4), GpuAffineMatrix{}, 1.0F}, kBudget)
               .code == GpuAffineDiagnosticCode::None;
}

// Generic bounded-pool vector: 32 idle instances allocate no slot; exactly the free slots become
// Ready; the next begin is refused before any native allocation; foreign destruction preserves the
// bound; the owner drain retires every orphan and admission recovers.
template <typename Op, typename CreateFn, typename BeginFn, typename DrainFn>
void runBoundedPool(Expectations& expectations, const std::string& name, const std::size_t capacity,
                    std::size_t (*inUse)(), std::size_t (*orphaned)(), std::uint64_t (*refusals)(),
                    std::uint64_t (*retired)(), CreateFn create, BeginFn begin, DrainFn drain) {
    const std::size_t inUseBefore = inUse();
    const std::size_t orphanedBefore = orphaned();
    const std::uint64_t retiredBefore = retired();
    expectations.expect(capacity > 0, name + ": positive bounded capacity");
    expectations.expect(inUseBefore <= capacity, name + ": pool invariant holds before the test");
    const std::size_t available = capacity - inUseBefore;
    expectations.expect(available > 0, name + ": at least one free slot");
    if (available == 0) {
        return;
    }

    std::vector<std::shared_ptr<void>> producers;
    for (int index = 0; index < 32; ++index) {
        std::unique_ptr<Op> created = create();
        if (!created) {
            break;
        }
        producers.push_back(std::shared_ptr<void>(
            created.release(), [](void* const raw) { delete static_cast<Op*>(raw); }));
    }
    expectations.expect(producers.size() == 32, name + ": 32 idle instances are created");
    expectations.expect(inUse() == inUseBefore, name + ": idle instances allocate no slot");

    std::size_t ready = 0;
    for (auto& producer : producers) {
        if (!begin(*static_cast<Op*>(producer.get()))) {
            break;
        }
        ++ready;
    }
    expectations.expect(ready == available, name + ": exactly the available slots become Ready");
    expectations.expect(inUse() == capacity, name + ": the pool is full");

    const std::uint64_t refusalsBefore = refusals();
    std::unique_ptr<Op> extra = create();
    expectations.expect(extra != nullptr, name + ": an idle instance is still creatable when full");
    if (extra) {
        expectations.expect(!begin(*extra), name + ": a full pool refuses a begin cleanly");
        expectations.expect(refusals() == refusalsBefore + 1, name + ": the refusal is counted");
        expectations.expect(inUse() == capacity, name + ": a refused begin allocates no slot");
    }

    std::thread worker([&producers]() { producers.clear(); });
    worker.join();
    expectations.expect(inUse() == capacity, name + ": foreign destruction preserves the bound");
    expectations.expect(orphaned() == orphanedBefore + available,
                        name + ": every resident holder is orphaned in its own slot");

    drain();
    expectations.expect(orphaned() == orphanedBefore, name + ": the owner drain empties orphans");
    expectations.expect(inUse() == inUseBefore, name + ": the drain returns every orphaned slot");
    expectations.expect(retired() == retiredBefore + available,
                        name + ": every orphaned resident was retired on the owner thread");

    std::unique_ptr<Op> recovered = create();
    expectations.expect(recovered != nullptr, name + ": an instance is creatable after recovery");
    if (recovered) {
        expectations.expect(begin(*recovered), name + ": a begin works after recovery");
    }
}

// Generic unproven-retention vector: an unproven fence is retained intact until the rightful owner
// proves retirement.
template <typename Op, typename CreateFn, typename SubmitFn, typename DrainFn, typename SetFaultFn>
void runRetention(Expectations& expectations, const std::string& name, std::size_t (*inUse)(),
                  std::size_t (*orphaned)(), std::uint64_t (*retired)(), CreateFn create,
                  SubmitFn submit, DrainFn drain, SetFaultFn setFault) {
    const std::size_t inUseBefore = inUse();
    const std::size_t orphanedBefore = orphaned();
    const std::uint64_t retiredBefore = retired();

    std::unique_ptr<Op> op = create();
    expectations.expect(op != nullptr, name + ": the retention instance is created");
    if (!op) {
        return;
    }
    expectations.expect(submit(*op), name + ": the retention begin is accepted");
    expectations.expect(op->hasUnretiredSubmission(), name + ": the submission is unretired");

    std::thread worker([&op]() { op.reset(); });
    worker.join();
    expectations.expect(orphaned() == orphanedBefore + 1,
                        name + ": the pending instance is orphaned");
    expectations.expect(inUse() == inUseBefore + 1,
                        name + ": the pending instance still holds its slot");

    setFault(true);
    drain();
    expectations.expect(orphaned() == orphanedBefore + 1,
                        name + ": an unproven fence retains its orphan");
    expectations.expect(inUse() == inUseBefore + 1, name + ": an unproven fence retains its slot");
    expectations.expect(retired() == retiredBefore, name + ": nothing is retired while unproven");

    setFault(false);
    bool done = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!done && std::chrono::steady_clock::now() < deadline) {
        drain();
        done = orphaned() == orphanedBefore;
        if (!done) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    expectations.expect(done, name + ": the proven fence retires the orphan");
    expectations.expect(inUse() == inUseBefore, name + ": the proven drain returns the slot");
    expectations.expect(retired() == retiredBefore + 1,
                        name + ": the proven orphan was retired exactly once");
}

// Generic cleanup-and-retry vector: an injected pipeline-creation failure after the complete native
// resource set is built must free every child before its parent, return the bounded slot, and leave
// the instance able to rebuild and run on retry.
template <typename Op, typename CreateFn, typename SubmitFn, typename BeginFn, typename SetFaultFn>
void runCleanupRetry(Expectations& expectations, const std::string& name, std::size_t (*inUse)(),
                     std::uint64_t (*refusals)(), CreateFn create, SubmitFn submit, BeginFn begin,
                     SetFaultFn setFault) {
    const std::size_t inUseBefore = inUse();
    const std::uint64_t refusalsBefore = refusals();

    std::unique_ptr<Op> op = create();
    expectations.expect(op != nullptr, name + ": cleanup-and-retry instance created");
    if (!op) {
        return;
    }

    setFault(true);
    expectations.expect(!submit(*op), name + ": the injected creation failure refuses the begin");
    expectations.expect(inUse() == inUseBefore, name + ": the failed creation returns its slot");
    expectations.expect(refusals() == refusalsBefore,
                        name + ": a creation failure is not a refusal");

    setFault(false);
    expectations.expect(begin(*op), name + ": the retry rebuilds the native set and reaches Ready");
    expectations.expect(inUse() == inUseBefore + 1, name + ": the retry holds exactly one slot");
}

// Two real device owner threads. B's owner drain must skip A's orphans (wrong owner thread) and
// leave them untouched; A's owner drain then frees its own. This is the exact-owner guard that
// prevents one device thread from querying a fence or tearing down another device's Vulkan state.
struct OwnerThreadContext final {
    std::filesystem::path loaderPath;
    // Only the native operations are shared with the main thread (it foreign-releases them). The
    // device, uploader, and source image stay local to the owner thread so every native destructor
    // runs on the thread that owns it.
    std::unique_ptr<GpuComposite> composite;
    std::unique_ptr<GpuBlend> blend;
    std::unique_ptr<GpuAffine> affine;
    std::atomic<bool> ready{false};
    std::atomic<bool> drainNow{false};
    std::atomic<bool> drained{false};
    std::size_t compositeOrphaned = 0;
    std::size_t blendOrphaned = 0;
    std::size_t affineOrphaned = 0;
    bool created = false;
    std::string error;
};

void runOwnerThread(OwnerThreadContext& context) {
    GpuDeviceCreationOptions options;
    options.loader_path = context.loaderPath;
    auto device = GpuDevice::create(options);
    if (!device) {
        context.error = "device: " + device.diagnostic.message;
        context.ready.store(true);
        return;
    }
    auto uploader = GpuImageUpload::create(*device.device);
    if (!uploader) {
        context.error = "uploader: " + uploader.diagnostic.message;
        context.ready.store(true);
        return;
    }
    auto source = makeSource(*uploader.upload);
    if (!source) {
        context.error = "source upload";
        context.ready.store(true);
        return;
    }
    auto composite = GpuComposite::create(*device.device);
    auto blend = GpuBlend::create(*device.device);
    auto affine = GpuAffine::create(*device.device);
    if (!composite || !blend || !affine) {
        context.error = "operation creation";
        context.ready.store(true);
        return;
    }
    context.composite = std::move(composite.composite);
    context.blend = std::move(blend.blend);
    context.affine = std::move(affine.affine);
    if (!beginComposite(*context.composite, *source) || !beginBlendOp(*context.blend, *source) ||
        !beginAffineOp(*context.affine, *source)) {
        context.error = "operation begin";
        context.ready.store(true);
        return;
    }
    context.created = true;
    context.ready.store(true);
    while (!context.drainNow.load()) {
        std::this_thread::yield();
    }
    (void)GpuComposite::create(*device.device);
    context.compositeOrphaned = composite_detail::compositeResidentOrphaned();
    (void)GpuBlend::create(*device.device);
    context.blendOrphaned = blend_detail::blendResidentOrphaned();
    (void)GpuAffine::create(*device.device);
    context.affineOrphaned = affine_detail::affineResidentOrphaned();
    context.drained.store(true);
    // device/uploader/source are destroyed here, on their owner thread.
}

void testOwnerIsolation(Expectations& expectations, const std::filesystem::path& loaderPath) {
    const std::size_t compositeBefore = composite_detail::compositeResidentOrphaned();
    const std::size_t blendBefore = blend_detail::blendResidentOrphaned();
    const std::size_t affineBefore = affine_detail::affineResidentOrphaned();

    OwnerThreadContext a;
    a.loaderPath = loaderPath;
    OwnerThreadContext b;
    b.loaderPath = loaderPath;
    std::thread threadA([&a] { runOwnerThread(a); });
    std::thread threadB([&b] { runOwnerThread(b); });
    while (!a.ready.load() || !b.ready.load()) {
        std::this_thread::yield();
    }
    expectations.expect(a.created && b.created, "both owner-thread operation sets become Ready");
    if (a.created && b.created) {
        a.composite.reset();
        a.blend.reset();
        a.affine.reset();
        b.composite.reset();
        b.blend.reset();
        b.affine.reset();
        expectations.expect(composite_detail::compositeResidentOrphaned() == compositeBefore + 2,
                            "both owner-thread composites are orphaned");
        expectations.expect(blend_detail::blendResidentOrphaned() == blendBefore + 2,
                            "both owner-thread blends are orphaned");
        expectations.expect(affine_detail::affineResidentOrphaned() == affineBefore + 2,
                            "both owner-thread affines are orphaned");

        b.drainNow.store(true);
        while (!b.drained.load()) {
            std::this_thread::yield();
        }
        expectations.expect(b.compositeOrphaned == compositeBefore + 1,
                            "B's composite drain leaves A's orphan untouched");
        expectations.expect(b.blendOrphaned == blendBefore + 1,
                            "B's blend drain leaves A's orphan untouched");
        expectations.expect(b.affineOrphaned == affineBefore + 1,
                            "B's affine drain leaves A's orphan untouched");
        expectations.expect(composite_detail::compositeResidentOrphaned() == compositeBefore + 1 &&
                                blend_detail::blendResidentOrphaned() == blendBefore + 1 &&
                                affine_detail::affineResidentOrphaned() == affineBefore + 1,
                            "a wrong-owner drain does not touch another owner's orphans");

        a.drainNow.store(true);
        while (!a.drained.load()) {
            std::this_thread::yield();
        }
        expectations.expect(composite_detail::compositeResidentOrphaned() == compositeBefore &&
                                blend_detail::blendResidentOrphaned() == blendBefore &&
                                affine_detail::affineResidentOrphaned() == affineBefore,
                            "A's drain frees its own orphans");
    } else {
        std::cerr << "owner-thread A: " << a.error << " ; B: " << b.error << '\n';
        a.drainNow.store(true);
        b.drainNow.store(true);
    }
    threadA.join();
    threadB.join();
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

        auto uploader = GpuImageUpload::create(*device.device);
        expectations.expect(uploader.hasValue(), "the source uploader is created");
        if (!uploader) {
            return 1;
        }
        auto source = makeSource(*uploader.upload);
        expectations.expect(source.has_value(), "the shared source image is uploaded");
        if (!source) {
            return 1;
        }
        GpuDevice& gpu = *device.device;
        const auto& sourceImage = *source;

        runBoundedPool<GpuComposite>(
            expectations, "composite", composite_detail::compositeResidentCapacity(),
            &composite_detail::compositeResidentInUse, &composite_detail::compositeResidentOrphaned,
            &composite_detail::compositeResidentRefusals,
            &composite_detail::compositeResidentRetired,
            [&gpu]() -> std::unique_ptr<GpuComposite> {
                auto created = GpuComposite::create(gpu);
                return created.hasValue() ? std::move(created.composite) : nullptr;
            },
            [&sourceImage](GpuComposite& composite) {
                return beginComposite(composite, sourceImage);
            },
            [&gpu]() { (void)GpuComposite::create(gpu); });
        runBoundedPool<GpuBlend>(
            expectations, "blend", blend_detail::blendResidentCapacity(),
            &blend_detail::blendResidentInUse, &blend_detail::blendResidentOrphaned,
            &blend_detail::blendResidentRefusals, &blend_detail::blendResidentRetired,
            [&gpu]() -> std::unique_ptr<GpuBlend> {
                auto created = GpuBlend::create(gpu);
                return created.hasValue() ? std::move(created.blend) : nullptr;
            },
            [&sourceImage](GpuBlend& blend) { return beginBlendOp(blend, sourceImage); },
            [&gpu]() { (void)GpuBlend::create(gpu); });
        runBoundedPool<GpuAffine>(
            expectations, "affine", affine_detail::affineResidentCapacity(),
            &affine_detail::affineResidentInUse, &affine_detail::affineResidentOrphaned,
            &affine_detail::affineResidentRefusals, &affine_detail::affineResidentRetired,
            [&gpu]() -> std::unique_ptr<GpuAffine> {
                auto created = GpuAffine::create(gpu);
                return created.hasValue() ? std::move(created.affine) : nullptr;
            },
            [&sourceImage](GpuAffine& affine) { return beginAffineOp(affine, sourceImage); },
            [&gpu]() { (void)GpuAffine::create(gpu); });

        runRetention<GpuComposite>(
            expectations, "composite", &composite_detail::compositeResidentInUse,
            &composite_detail::compositeResidentOrphaned,
            &composite_detail::compositeResidentRetired,
            [&gpu]() -> std::unique_ptr<GpuComposite> {
                auto created = GpuComposite::create(gpu);
                return created.hasValue() ? std::move(created.composite) : nullptr;
            },
            [&sourceImage](GpuComposite& composite) {
                return submitComposite(composite, sourceImage);
            },
            [&gpu]() { (void)GpuComposite::create(gpu); },
            [](const bool force) {
                composite_detail::setCompositeRetirementFaultForTest(
                    force ? composite_detail::CompositeRetirementFault::ForceFenceTimeout
                          : composite_detail::CompositeRetirementFault::None);
            });
        runRetention<GpuBlend>(
            expectations, "blend", &blend_detail::blendResidentInUse,
            &blend_detail::blendResidentOrphaned, &blend_detail::blendResidentRetired,
            [&gpu]() -> std::unique_ptr<GpuBlend> {
                auto created = GpuBlend::create(gpu);
                return created.hasValue() ? std::move(created.blend) : nullptr;
            },
            [&sourceImage](GpuBlend& blend) { return submitBlendOp(blend, sourceImage); },
            [&gpu]() { (void)GpuBlend::create(gpu); },
            [](const bool force) {
                blend_detail::setBlendRetirementFaultForTest(
                    force ? blend_detail::BlendRetirementFault::ForceFenceTimeout
                          : blend_detail::BlendRetirementFault::None);
            });
        runRetention<GpuAffine>(
            expectations, "affine", &affine_detail::affineResidentInUse,
            &affine_detail::affineResidentOrphaned, &affine_detail::affineResidentRetired,
            [&gpu]() -> std::unique_ptr<GpuAffine> {
                auto created = GpuAffine::create(gpu);
                return created.hasValue() ? std::move(created.affine) : nullptr;
            },
            [&sourceImage](GpuAffine& affine) { return submitAffineOp(affine, sourceImage); },
            [&gpu]() { (void)GpuAffine::create(gpu); },
            [](const bool force) {
                affine_detail::setAffineRetirementFaultForTest(
                    force ? affine_detail::AffineRetirementFault::ForceFenceTimeout
                          : affine_detail::AffineRetirementFault::None);
            });

        runCleanupRetry<GpuComposite>(
            expectations, "composite", &composite_detail::compositeResidentInUse,
            &composite_detail::compositeResidentRefusals,
            [&gpu]() -> std::unique_ptr<GpuComposite> {
                auto created = GpuComposite::create(gpu);
                return created.hasValue() ? std::move(created.composite) : nullptr;
            },
            [&sourceImage](GpuComposite& composite) {
                return submitComposite(composite, sourceImage);
            },
            [&sourceImage](GpuComposite& composite) {
                return beginComposite(composite, sourceImage);
            },
            [](const bool fail) {
                composite_detail::setCompositeRetirementFaultForTest(
                    fail ? composite_detail::CompositeRetirementFault::FailPipelineCreation
                         : composite_detail::CompositeRetirementFault::None);
            });
        runCleanupRetry<GpuBlend>(
            expectations, "blend", &blend_detail::blendResidentInUse,
            &blend_detail::blendResidentRefusals,
            [&gpu]() -> std::unique_ptr<GpuBlend> {
                auto created = GpuBlend::create(gpu);
                return created.hasValue() ? std::move(created.blend) : nullptr;
            },
            [&sourceImage](GpuBlend& blend) { return submitBlendOp(blend, sourceImage); },
            [&sourceImage](GpuBlend& blend) { return beginBlendOp(blend, sourceImage); },
            [](const bool fail) {
                blend_detail::setBlendRetirementFaultForTest(
                    fail ? blend_detail::BlendRetirementFault::FailPipelineCreation
                         : blend_detail::BlendRetirementFault::None);
            });
        runCleanupRetry<GpuAffine>(
            expectations, "affine", &affine_detail::affineResidentInUse,
            &affine_detail::affineResidentRefusals,
            [&gpu]() -> std::unique_ptr<GpuAffine> {
                auto created = GpuAffine::create(gpu);
                return created.hasValue() ? std::move(created.affine) : nullptr;
            },
            [&sourceImage](GpuAffine& affine) { return submitAffineOp(affine, sourceImage); },
            [&sourceImage](GpuAffine& affine) { return beginAffineOp(affine, sourceImage); },
            [](const bool fail) {
                affine_detail::setAffineRetirementFaultForTest(
                    fail ? affine_detail::AffineRetirementFault::FailPipelineCreation
                         : affine_detail::AffineRetirementFault::None);
            });

        testOwnerIsolation(expectations, options.loader_path);

        if (expectations.failures() != 0) {
            std::cerr << "FAIL: composite/blend/affine retirement expectations failed\n";
            return 1;
        }
        std::cout << "PASS: composite/blend/affine bounded retirement\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
