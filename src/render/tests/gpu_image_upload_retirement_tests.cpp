// Bounded native-resource-retirement tests for GpuImageUpload.
//
// These vectors prove the process-global bounded resident pool: many pre-created instances beyond
// capacity, foreign-thread destruction boundedness, retention of an unproven fence, exact
// owner-thread/device-generation isolation across two real device threads, and owner-drain
// recovery. The private fault seam is Vulkan-free, so this target exists only where the Vulkan
// backend is built; a hardware-free build prints an explicit skip and --require-device fails
// closed.

#include "gpu_image_upload_fault.hpp"

#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/render/gpu_image_upload.hpp>
#include <bloom/render/image.hpp>
#include <bloom/render/image_types.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using bloom::core::PixelAspectRatio;
using bloom::render::GpuDevice;
using bloom::render::GpuDeviceCreationOptions;
using bloom::render::GpuImageUpload;
using bloom::render::GpuImageUploadDiagnosticCode;
using bloom::render::GpuImageUploadParameters;
using bloom::render::GpuImageUploadPollResult;
using bloom::render::ImageWindow;
using bloom::render::Rgba32f;
using bloom::render::Rgba32fImage;
using bloom::render::Rgba32fImageBuilder;
using bloom::render::Rgba32fImageDescriptor;

namespace upload_detail = bloom::render::upload_detail;

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
    [[nodiscard]] bool ok() const noexcept { return failures_ == 0; }

  private:
    std::size_t failures_ = 0;
};

struct Options final {
    std::filesystem::path loader_path;
    bool require_device = false;
    bool valid = true;
};

[[nodiscard]] Options parseOptions(const int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--loader") {
            if (index + 1 >= argc) {
                std::cerr << "--loader requires a path argument\n";
                options.valid = false;
                return options;
            }
            options.loader_path = argv[++index];
        } else if (argument == "--require-device") {
            options.require_device = true;
        } else {
            std::cerr << "unknown argument: " << argument << '\n';
            options.valid = false;
            return options;
        }
    }
    return options;
}

// A real production host image with awkward geometry and values.
[[nodiscard]] std::shared_ptr<const Rgba32fImage> makeSource() {
    const auto window = ImageWindow::create(-1, 3, 5, 3);
    if (!window) {
        return nullptr;
    }
    const auto descriptor = Rgba32fImageDescriptor::create(*window.value(), *window.value(),
                                                           PixelAspectRatio::square());
    if (!descriptor) {
        return nullptr;
    }
    auto builder = Rgba32fImageBuilder::create(*descriptor.value(), 1ULL << 32ULL);
    if (!builder) {
        return nullptr;
    }
    const std::uint32_t width = window.value()->extent().width();
    for (std::int64_t y = window.value()->originY(); y < window.value()->maxYExclusive(); ++y) {
        auto row = builder.value()->row(y);
        if (!row || row.value()->size() != width) {
            return nullptr;
        }
        auto pixels = *row.value();
        for (std::uint32_t x = 0; x < width; ++x) {
            const float r = static_cast<float>(x) * 0.25F - 0.5F;
            const float g = static_cast<float>(y - window.value()->originY()) * 0.125F;
            const float b = 0.75F;
            const float a = (x % 2U == 0U) ? 0.5F : 1.0F;
            const auto pixel = Rgba32f::fromPremultiplied(r, g, b, a);
            if (!pixel) {
                return nullptr;
            }
            pixels[x] = *pixel.value();
        }
    }
    auto frozen = std::move(*builder.value()).freeze();
    if (!frozen) {
        return nullptr;
    }
    return std::make_shared<const Rgba32fImage>(std::move(*frozen.value()));
}

[[nodiscard]] GpuImageUploadParameters
makeParameters(const std::shared_ptr<const Rgba32fImage>& source) {
    GpuImageUploadParameters parameters;
    parameters.source = source;
    return parameters;
}

[[nodiscard]] bool pollToCompletion(GpuImageUpload& upload) {
    GpuImageUploadPollResult poll = GpuImageUploadPollResult::Pending;
    while (poll == GpuImageUploadPollResult::Pending) {
        poll = upload.poll();
    }
    return poll == GpuImageUploadPollResult::Ready;
}

[[nodiscard]] bool runUpload(GpuImageUpload& upload, const GpuImageUploadParameters& parameters) {
    if (upload.begin(parameters, 1ULL << 32ULL).code != GpuImageUploadDiagnosticCode::None) {
        return false;
    }
    return pollToCompletion(upload);
}

// Trigger the non-blocking owner-thread drain through the public create(), which drains orphaned
// foreign-released residents before constructing the (lazy, native-free) new instance.
void drainOwnerOrphans(GpuDevice& device) { (void)GpuImageUpload::create(device); }

void testBoundedPool(Expectations& expectations, GpuDevice& device) {
    const auto source = makeSource();
    expectations.expect(source != nullptr, "the pool fixture source builds");
    if (source == nullptr) {
        return;
    }
    const auto parameters = makeParameters(source);
    const std::size_t capacity = upload_detail::uploadResidentCapacity();
    expectations.expect(capacity > 0, "the upload resident pool has a positive bounded capacity");
    const std::size_t inUseBefore = upload_detail::uploadResidentInUse();
    const std::size_t orphanedBefore = upload_detail::uploadResidentOrphaned();
    const std::uint64_t retiredBefore = upload_detail::uploadResidentRetired();
    expectations.expect(inUseBefore <= capacity, "the upload pool invariant holds before the test");
    const std::size_t available = capacity - inUseBefore;
    expectations.expect(available > 0, "the upload pool has at least one free slot");
    if (available == 0) {
        return;
    }

    std::vector<std::unique_ptr<GpuImageUpload>> producers;
    for (int index = 0; index < 32; ++index) {
        auto created = GpuImageUpload::create(device);
        if (!created) {
            break;
        }
        producers.push_back(std::move(created.upload));
    }
    expectations.expect(producers.size() == 32, "32 idle uploads are created");
    expectations.expect(upload_detail::uploadResidentInUse() == inUseBefore,
                        "idle uploads allocate no resident slot");

    std::size_t ready = 0;
    for (auto& producer : producers) {
        if (!runUpload(*producer, parameters)) {
            break;
        }
        ++ready;
    }
    expectations.expect(ready == available, "exactly the available upload slots become Ready");
    expectations.expect(upload_detail::uploadResidentInUse() == capacity,
                        "the upload pool is full");

    const std::uint64_t refusalsBefore = upload_detail::uploadResidentRefusals();
    auto extra = GpuImageUpload::create(device);
    expectations.expect(extra.hasValue(),
                        "an idle upload is still creatable when the pool is full");
    if (extra) {
        const auto refused = extra.upload->begin(parameters, 1ULL << 32ULL);
        expectations.expect(refused.code == GpuImageUploadDiagnosticCode::DeviceUnavailable,
                            "a full upload pool refuses a begin cleanly");
        expectations.expect(upload_detail::uploadResidentRefusals() == refusalsBefore + 1,
                            "the upload refusal is counted");
        expectations.expect(upload_detail::uploadResidentInUse() == capacity,
                            "a refused upload begin allocates no resident slot");
    }

    std::thread worker([&producers]() { producers.clear(); });
    worker.join();
    expectations.expect(upload_detail::uploadResidentInUse() == capacity,
                        "foreign destruction preserves the upload pool bound");
    expectations.expect(upload_detail::uploadResidentOrphaned() == orphanedBefore + available,
                        "every upload resident holder is orphaned in its own slot");

    drainOwnerOrphans(device);
    expectations.expect(upload_detail::uploadResidentOrphaned() == orphanedBefore,
                        "the upload owner drain empties the orphaned set");
    expectations.expect(upload_detail::uploadResidentInUse() == inUseBefore,
                        "the upload drain returns every orphaned slot");
    expectations.expect(upload_detail::uploadResidentRetired() == retiredBefore + available,
                        "every orphaned upload resident was retired on the owner thread");

    auto recovered = GpuImageUpload::create(device);
    expectations.expect(recovered.hasValue(), "an upload is creatable after recovery");
    if (recovered) {
        expectations.expect(runUpload(*recovered.upload, parameters),
                            "an upload begin works after recovery");
    }
}

void testUnprovenRetention(Expectations& expectations, GpuDevice& device) {
    const auto source = makeSource();
    expectations.expect(source != nullptr, "the retention fixture source builds");
    if (source == nullptr) {
        return;
    }
    const auto parameters = makeParameters(source);
    const std::size_t orphanedBefore = upload_detail::uploadResidentOrphaned();
    const std::size_t inUseBefore = upload_detail::uploadResidentInUse();
    const std::uint64_t retiredBefore = upload_detail::uploadResidentRetired();

    auto created = GpuImageUpload::create(device);
    expectations.expect(created.hasValue(), "the retention upload is created");
    if (!created) {
        return;
    }
    auto upload = std::move(created.upload);
    expectations.expect(upload->begin(parameters, 1ULL << 32ULL).code ==
                            GpuImageUploadDiagnosticCode::None,
                        "the retention upload begin is accepted");
    expectations.expect(upload->hasUnretiredSubmission(),
                        "the retention upload submission is unretired");

    std::thread worker([&upload]() { upload.reset(); });
    worker.join();
    expectations.expect(upload_detail::uploadResidentOrphaned() == orphanedBefore + 1,
                        "the pending upload is orphaned");
    expectations.expect(upload_detail::uploadResidentInUse() == inUseBefore + 1,
                        "the pending upload still holds its slot");

    upload_detail::setUploadRetirementFaultForTest(
        upload_detail::UploadRetirementFault::ForceFenceTimeout);
    drainOwnerOrphans(device);
    expectations.expect(upload_detail::uploadResidentOrphaned() == orphanedBefore + 1,
                        "an unproven upload fence retains its orphan");
    expectations.expect(upload_detail::uploadResidentInUse() == inUseBefore + 1,
                        "an unproven upload fence retains its slot");
    expectations.expect(upload_detail::uploadResidentRetired() == retiredBefore,
                        "nothing is retired while the upload fence is unproven");

    upload_detail::setUploadRetirementFaultForTest(upload_detail::UploadRetirementFault::None);
    bool retired = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!retired && std::chrono::steady_clock::now() < deadline) {
        drainOwnerOrphans(device);
        retired = upload_detail::uploadResidentOrphaned() == orphanedBefore;
        if (!retired) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    expectations.expect(retired, "the proven upload fence retires the orphan");
    expectations.expect(upload_detail::uploadResidentInUse() == inUseBefore,
                        "the proven upload drain returns the slot");
    expectations.expect(upload_detail::uploadResidentRetired() == retiredBefore + 1,
                        "the proven upload orphan was retired exactly once");
}

struct OwnerThreadContext final {
    std::filesystem::path loaderPath;
    GpuImageUploadParameters parameters;
    std::unique_ptr<GpuImageUpload> upload;
    std::atomic<bool> ready{false};
    std::atomic<bool> drainNow{false};
    std::atomic<bool> drained{false};
    std::size_t orphanedAfter = 0;
    std::size_t inUseAfter = 0;
    std::uint64_t retiredAfter = 0;
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
    auto upload = GpuImageUpload::create(*device.device);
    if (!upload) {
        context.error = "upload: " + upload.diagnostic.message;
        context.ready.store(true);
        return;
    }
    if (!runUpload(*upload.upload, context.parameters)) {
        context.error = "upload did not reach Ready";
        context.ready.store(true);
        return;
    }
    context.upload = std::move(upload.upload);
    context.created = true;
    context.ready.store(true);
    while (!context.drainNow.load()) {
        std::this_thread::yield();
    }
    (void)GpuImageUpload::create(*device.device);
    context.orphanedAfter = upload_detail::uploadResidentOrphaned();
    context.inUseAfter = upload_detail::uploadResidentInUse();
    context.retiredAfter = upload_detail::uploadResidentRetired();
    context.drained.store(true);
    // `device` is destroyed here, on its owner thread.
}

void testOwnerIsolation(Expectations& expectations, const std::filesystem::path& loaderPath) {
    const auto source = makeSource();
    expectations.expect(source != nullptr, "the owner-isolation source builds");
    if (source == nullptr) {
        return;
    }
    const auto parameters = makeParameters(source);
    const std::size_t inUseBefore = upload_detail::uploadResidentInUse();
    const std::size_t orphanedBefore = upload_detail::uploadResidentOrphaned();
    const std::uint64_t retiredBefore = upload_detail::uploadResidentRetired();

    OwnerThreadContext a;
    a.loaderPath = loaderPath;
    a.parameters = parameters;
    OwnerThreadContext b;
    b.loaderPath = loaderPath;
    b.parameters = parameters;
    std::thread threadA([&a] { runOwnerThread(a); });
    std::thread threadB([&b] { runOwnerThread(b); });
    while (!a.ready.load() || !b.ready.load()) {
        std::this_thread::yield();
    }
    expectations.expect(a.created && b.created, "both owner-thread uploads become Ready");
    if (!a.created || !b.created) {
        std::cerr << "owner-thread A: " << a.error << " ; B: " << b.error << '\n';
    }
    if (a.created && b.created) {
        a.upload.reset();
        b.upload.reset();
        expectations.expect(upload_detail::uploadResidentOrphaned() == orphanedBefore + 2,
                            "both owner-thread uploads are orphaned");
        expectations.expect(upload_detail::uploadResidentInUse() == inUseBefore + 2,
                            "both owner-thread uploads hold slots");

        b.drainNow.store(true);
        while (!b.drained.load()) {
            std::this_thread::yield();
        }
        expectations.expect(b.orphanedAfter == orphanedBefore + 1,
                            "B's drain leaves A's orphan untouched");
        expectations.expect(upload_detail::uploadResidentOrphaned() == orphanedBefore + 1,
                            "a wrong-owner drain does not touch another owner's orphan");
        expectations.expect(upload_detail::uploadResidentRetired() == retiredBefore + 1,
                            "B retires exactly its own upload orphan");

        a.drainNow.store(true);
        while (!a.drained.load()) {
            std::this_thread::yield();
        }
        expectations.expect(upload_detail::uploadResidentOrphaned() == orphanedBefore,
                            "A's drain frees A's orphan");
        expectations.expect(upload_detail::uploadResidentInUse() == inUseBefore,
                            "all owner-thread upload slots are returned");
        expectations.expect(upload_detail::uploadResidentRetired() == retiredBefore + 2,
                            "both owners retire their own upload orphans");
    } else {
        a.drainNow.store(true);
        b.drainNow.store(true);
    }
    threadA.join();
    threadB.join();
}

void testCancelHonest(Expectations& expectations, GpuDevice& device) {
    const auto source = makeSource();
    expectations.expect(source != nullptr, "the cancel fixture source builds");
    if (source == nullptr) {
        return;
    }
    const auto parameters = makeParameters(source);
    auto created = GpuImageUpload::create(device);
    expectations.expect(created.hasValue(), "the cancellable upload is created");
    if (!created) {
        return;
    }
    GpuImageUpload& upload = *created.upload;
    expectations.expect(upload.begin(parameters, 1ULL << 32ULL).code ==
                            GpuImageUploadDiagnosticCode::None,
                        "the cancellable upload begin is accepted");
    upload.cancel();
    expectations.expect(pollToCompletion(upload) == false &&
                            upload.diagnostic().code == GpuImageUploadDiagnosticCode::Cancelled,
                        "a cancelled upload fails closed as Cancelled");
    expectations.expect(upload.image() == nullptr, "a cancelled upload publishes no image");
    expectations.expect(!upload.hasUnretiredSubmission(),
                        "a cancelled upload retires its submission honestly");
    expectations.expect(runUpload(upload, parameters), "the upload is reusable after cancellation");
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
            return expectations.ok() ? 0 : 1;
        }

        testBoundedPool(expectations, *device.device);
        testUnprovenRetention(expectations, *device.device);
        testOwnerIsolation(expectations, options.loader_path);
        testCancelHonest(expectations, *device.device);

        if (!expectations.ok()) {
            std::cerr << "FAIL: upload retirement expectations failed\n";
            return 1;
        }
        std::cout << "PASS: upload bounded retirement\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
