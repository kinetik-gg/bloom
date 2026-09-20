// CPU-only refusal-semantics test for ViewerGpuPresenter, using the private ViewerGpuPort seam.
//
// This is explicitly NOT native acceptance (a fake port cannot count as that; the real fixture is
// viewer_gpu_presenter_native_tests.cpp). It proves the branches the real coordinator cannot be
// made to produce on demand: a duplicate-surface attach is retained (never acked safe), and a
// quarantined / unproven target is retained with no fake ack. A bare device-less VkInstance lets
// the real QWindow / VkSurfaceKHR path run without a GpuDevice.

#include <bloom/render/gpu_presentation_types.hpp>
#include <bloom/runtime/gpu_presentation_coordinator.hpp>
#include <bloom/ui/viewer_gpu_presenter.hpp>

#include "viewer_gpu_presenter_port.hpp"

#include "gpu_native_test_environment.hpp"
#include "viewer_gpu_presenter_native_test_support.hpp"

#include <QApplication>
#include <QEventLoop>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <vulkan/vulkan.h>

#include <dlfcn.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using bloom::render::GpuBorrowedInstanceView;
using bloom::render::GpuBorrowedSurface;
using bloom::render::GpuPresentationEpoch;
using bloom::runtime::GpuPresentationPortCode;
using bloom::runtime::GpuPresentationPortResult;
using bloom::runtime::GpuPresentationTargetId;
using bloom::runtime::GpuPresentationTargetSnapshot;
using bloom::runtime::GpuPresentationTargetState;
using bloom::runtime::GpuPresentationUpdate;
using bloom::runtime::kInvalidPresentationTarget;
using bloom::ui::ViewerGpuPort;
using bloom::ui::ViewerGpuPresenter;

void pumpQt(const int milliseconds) {
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}

// Drives the presenter's own poll at the bounded Qt event loop until the fixture-observed condition
// holds. A fixed sleep is not authoritative here: the attach path needs the real QWindow exposed
// and a real VkSurfaceKHR, both of which the platform publishes asynchronously.
template <typename Predicate>
[[nodiscard]] bool waitForPresenter(ViewerGpuPresenter& presenter, Predicate predicate,
                                    const int timeoutMilliseconds = 5000) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMilliseconds);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        static_cast<void>(presenter.pollNow());
        pumpQt(5);
    }
    return predicate();
}

class Expectations final {
  public:
    void expect(const bool condition, const std::string& message) {
        if (!condition) {
            ++failures_;
            std::cerr << "FAIL: " << message << '\n';
        }
    }
    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

// A bare, device-less VkInstance with the Wayland surface extension, so Qt can create a real
// VkSurfaceKHR for the QWindow without a GpuDevice.
class BareInstance final {
  public:
    [[nodiscard]] bool create(const std::string& loaderPath) {
        lib_ = dlopen(loaderPath.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (lib_ == nullptr) {
            return false;
        }
        auto createInstance =
            reinterpret_cast<PFN_vkCreateInstance>(dlsym(lib_, "vkCreateInstance"));
        destroyInstance_ =
            reinterpret_cast<PFN_vkDestroyInstance>(dlsym(lib_, "vkDestroyInstance"));
        if (createInstance == nullptr || destroyInstance_ == nullptr) {
            return false;
        }
        const char* extensions[] = {"VK_KHR_surface", "VK_KHR_wayland_surface"};
        VkApplicationInfo applicationInfo{};
        applicationInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        applicationInfo.apiVersion = VK_API_VERSION_1_0;
        VkInstanceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.pApplicationInfo = &applicationInfo;
        createInfo.enabledExtensionCount = 2;
        createInfo.ppEnabledExtensionNames = extensions;
        return createInstance(&createInfo, nullptr, &instance_) == VK_SUCCESS;
    }
    ~BareInstance() {
        if (instance_ != VK_NULL_HANDLE && destroyInstance_ != nullptr) {
            destroyInstance_(instance_, nullptr);
        }
        if (lib_ != nullptr) {
            dlclose(lib_);
        }
    }
    [[nodiscard]] std::uint64_t bits() const noexcept {
        return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(instance_));
    }

  private:
    void* lib_ = nullptr;
    PFN_vkDestroyInstance destroyInstance_ = nullptr;
    VkInstance instance_ = VK_NULL_HANDLE;
};

struct FakePort final : ViewerGpuPort {
    GpuBorrowedInstanceView view;
    GpuPresentationPortCode attachCode = GpuPresentationPortCode::Accepted;
    GpuPresentationPortCode updateCode = GpuPresentationPortCode::Accepted;
    GpuPresentationPortCode retireCode = GpuPresentationPortCode::Accepted;
    GpuPresentationPortCode forgetCode = GpuPresentationPortCode::Accepted;
    bool alive = true;
    std::vector<GpuPresentationTargetId> forgotten;
    GpuPresentationTargetId nextId = 1;
    std::vector<GpuBorrowedSurface> attaches;
    std::map<GpuPresentationTargetId, GpuPresentationTargetSnapshot> snapshots;

    [[nodiscard]] GpuBorrowedInstanceView instanceView() const override { return view; }

    [[nodiscard]] GpuPresentationPortResult attach(const GpuBorrowedSurface& surface, std::uint32_t,
                                                   std::uint32_t) override {
        attaches.push_back(surface);
        GpuPresentationPortResult result;
        result.code = attachCode;
        if (attachCode == GpuPresentationPortCode::Accepted) {
            result.target = nextId++;
            GpuPresentationTargetSnapshot snapshot;
            snapshot.id = result.target;
            snapshot.state = GpuPresentationTargetState::Attaching;
            snapshots[result.target] = snapshot;
        } else if (attachCode == GpuPresentationPortCode::DuplicateSurface) {
            result.message = "duplicate";
        }
        return result;
    }
    [[nodiscard]] GpuPresentationPortResult
    update(GpuPresentationTargetId target, std::uint64_t sequence, GpuPresentationUpdate) override {
        GpuPresentationPortResult result;
        result.target = target;
        result.sequence = sequence;
        result.code = updateCode;
        return result;
    }
    [[nodiscard]] GpuPresentationPortResult resize(GpuPresentationTargetId target,
                                                   std::uint64_t sequence, std::uint32_t,
                                                   std::uint32_t) override {
        GpuPresentationPortResult result;
        result.target = target;
        result.sequence = sequence;
        result.code = updateCode;
        return result;
    }
    [[nodiscard]] GpuPresentationPortResult retire(GpuPresentationTargetId target,
                                                   std::uint64_t sequence) override {
        GpuPresentationPortResult result;
        result.target = target;
        result.sequence = sequence;
        result.code = retireCode;
        if (retireCode == GpuPresentationPortCode::Accepted) {
            snapshots[target].state = GpuPresentationTargetState::Retiring;
        }
        return result;
    }
    [[nodiscard]] GpuPresentationPortResult forget(GpuPresentationTargetId target) override {
        forgotten.push_back(target);
        GpuPresentationPortResult result;
        result.target = target;
        result.code = forgetCode;
        return result;
    }
    [[nodiscard]] GpuPresentationTargetSnapshot
    status(GpuPresentationTargetId target) const override {
        const auto found = snapshots.find(target);
        return found == snapshots.end() ? GpuPresentationTargetSnapshot{} : found->second;
    }
    [[nodiscard]] bool ownerAlive() const noexcept override { return alive; }

    void setState(const GpuPresentationTargetId target, const GpuPresentationTargetState state,
                  const bool safe, const std::string& message = {}) {
        snapshots[target].id = target;
        snapshots[target].state = state;
        snapshots[target].surfaceSafeToDestroy = safe;
        snapshots[target].message = message;
    }
};

[[nodiscard]] ViewerGpuPresenter::Config configFor(const std::string& loader) {
    ViewerGpuPresenter::Config config;
    config.vulkan_loader_path = loader;
    config.target_width = 128U;
    config.target_height = 96U;
    return config;
}

} // namespace

int main(int argc, char** argv) {
    std::string loader;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--loader" && index + 1 < argc) {
            loader = argv[++index];
        }
    }
    const auto native_environment = bloom::ui::test::NativeWaylandEnvironment::inspect();
    if (!native_environment.available()) {
        return native_environment.exitStatus(/*require_device=*/false);
    }
    QApplication application(argc, argv);
    if (loader.empty()) {
        std::cout << "SKIP: --loader <libvulkan.so.1> is required\n";
        return 0;
    }
    BareInstance bare;
    if (!bare.create(loader)) {
        std::cout << "SKIP: the bare Vulkan instance could not be created\n";
        return 0;
    }

    Expectations expectations;
    QWidget host;
    host.resize(400, 300);
    auto* layout = new QVBoxLayout(&host);
    layout->setContentsMargins(0, 0, 0, 0);
    host.show();

    // 1. Duplicate-surface refusal is retained, never acked safe.
    {
        auto port = std::make_shared<FakePort>();
        port->view.valid = true;
        port->view.instance_bits = bare.bits();
        port->view.epoch = GpuPresentationEpoch{42};
        port->attachCode = GpuPresentationPortCode::DuplicateSurface;
        auto presenter = std::make_unique<ViewerGpuPresenter>(port, configFor(loader));
        presenter->setContainerParent(&host);
        expectations.expect(presenter->initialize(), "the duplicate probe initializes");
        layout->addWidget(presenter->container());
        presenter->container()->show();
        expectations.expect(waitForPresenter(*presenter,
                                             [&] {
                                                 return presenter->state() ==
                                                        ViewerGpuPresenter::State::Retained;
                                             }),
                            "a duplicate-surface attach is Retained: " +
                                bloom::ui::test::describeViewerGpuPresenter(*presenter));
        expectations.expect(!presenter->surfaceSafeToDestroy(),
                            "a duplicate-surface rejection is not safe to destroy");
        bool called = false;
        bool safe = true;
        static_cast<void>(
            presenter->prepareForMutation([&](const ViewerGpuPresenter::MutationResult& result) {
                called = true;
                safe = result.outcome == ViewerGpuPresenter::MutationOutcome::SafeToMutate;
            }));
        expectations.expect(called && !safe, "a retained duplicate never acks SafeToMutate");
        expectations.expect(!presenter->attached(), "a refused attach owns no native target");
        expectations.expect(port->forgotten.empty(), "a refusal never forgets a record");
    }

    // 2. A quarantined / unproven target is retained with no fake ack. The presenter is
    // deliberately
    //    leaked: per the ownership contract a retained surface must not be destroyed.
    {
        auto port = std::make_shared<FakePort>();
        port->view.valid = true;
        port->view.instance_bits = bare.bits();
        port->view.epoch = GpuPresentationEpoch{43};
        auto* presenter = new ViewerGpuPresenter(port, configFor(loader));
        presenter->setContainerParent(&host);
        expectations.expect(presenter->initialize(), "the quarantine probe initializes");
        layout->addWidget(presenter->container());
        presenter->container()->show();
        expectations.expect(waitForPresenter(*presenter, [&] { return presenter->attached(); }),
                            "the quarantine probe attaches a real target: " +
                                bloom::ui::test::describeViewerGpuPresenter(*presenter));
        expectations.expect(presenter->state() == ViewerGpuPresenter::State::Attaching,
                            "the quarantine probe reports Attaching after a real attach: " +
                                bloom::ui::test::describeViewerGpuPresenter(*presenter));
        port->setState(presenter->targetId(), GpuPresentationTargetState::Active, false);
        expectations.expect(waitForPresenter(*presenter,
                                             [&] {
                                                 return presenter->state() ==
                                                        ViewerGpuPresenter::State::Active;
                                             }),
                            "the quarantine probe becomes Active: " +
                                bloom::ui::test::describeViewerGpuPresenter(*presenter));
        bool called = false;
        bool safe = true;
        static_cast<void>(
            presenter->prepareForMutation([&](const ViewerGpuPresenter::MutationResult& result) {
                called = true;
                safe = result.outcome == ViewerGpuPresenter::MutationOutcome::SafeToMutate;
            }));
        expectations.expect(presenter->state() == ViewerGpuPresenter::State::Retiring,
                            "the retire is admitted");
        port->setState(presenter->targetId(), GpuPresentationTargetState::Quarantined, false);
        static_cast<void>(presenter->pollNow());
        expectations.expect(called && !safe,
                            "a quarantined target retains the surface and never acks safe");
        expectations.expect(presenter->state() == ViewerGpuPresenter::State::Retained,
                            "a quarantined target is Retained");
        expectations.expect(port->forgotten.empty(), "a quarantined target is never forgotten");
        // Intentionally not deleted: the retained surface must outlive the UI mutation request.
    }

    // 3. A proven Retired record is forgotten exactly once, only after its terminal diagnostic is
    //    preserved; a later poll does not forget again.
    {
        auto port = std::make_shared<FakePort>();
        port->view.valid = true;
        port->view.instance_bits = bare.bits();
        port->view.epoch = GpuPresentationEpoch{44};
        auto presenter = std::make_unique<ViewerGpuPresenter>(port, configFor(loader));
        presenter->setContainerParent(&host);
        expectations.expect(presenter->initialize(), "the retired probe initializes");
        layout->addWidget(presenter->container());
        presenter->container()->show();
        expectations.expect(waitForPresenter(*presenter, [&] { return presenter->attached(); }),
                            "the retired probe attaches a real target: " +
                                bloom::ui::test::describeViewerGpuPresenter(*presenter));
        const GpuPresentationTargetId id = presenter->targetId();
        port->setState(id, GpuPresentationTargetState::Active, false);
        expectations.expect(waitForPresenter(*presenter,
                                             [&] {
                                                 return presenter->state() ==
                                                        ViewerGpuPresenter::State::Active;
                                             }),
                            "the retired probe becomes Active: " +
                                bloom::ui::test::describeViewerGpuPresenter(*presenter));
        bool called = false;
        bool safe = false;
        static_cast<void>(
            presenter->prepareForMutation([&](const ViewerGpuPresenter::MutationResult& result) {
                called = true;
                safe = result.outcome == ViewerGpuPresenter::MutationOutcome::SafeToMutate;
            }));
        port->setState(id, GpuPresentationTargetState::Retired, true, "engine proof");
        expectations.expect(waitForPresenter(*presenter, [&] { return called; }),
                            "the proven Retired acks SafeToMutate: " +
                                bloom::ui::test::describeViewerGpuPresenter(*presenter));
        expectations.expect(called && safe, "the proven Retired acks SafeToMutate");
        expectations.expect(port->forgotten.size() == 1U && port->forgotten[0] == id,
                            "the Retired record is forgotten exactly once");
        expectations.expect(presenter->diagnostic() == "engine proof",
                            "the terminal diagnostic is preserved before reclaim");
        static_cast<void>(presenter->pollNow());
        expectations.expect(port->forgotten.size() == 1U, "a later poll does not forget again");
    }

    // 4. A Rejected target that proves no native target was created is also forgotten.
    {
        auto port = std::make_shared<FakePort>();
        port->view.valid = true;
        port->view.instance_bits = bare.bits();
        port->view.epoch = GpuPresentationEpoch{45};
        auto presenter = std::make_unique<ViewerGpuPresenter>(port, configFor(loader));
        presenter->setContainerParent(&host);
        expectations.expect(presenter->initialize(), "the rejected probe initializes");
        layout->addWidget(presenter->container());
        presenter->container()->show();
        expectations.expect(waitForPresenter(*presenter, [&] { return presenter->attached(); }),
                            "the rejected probe attaches a real target: " +
                                bloom::ui::test::describeViewerGpuPresenter(*presenter));
        const GpuPresentationTargetId id = presenter->targetId();
        port->setState(id, GpuPresentationTargetState::Rejected, true, "stale epoch");
        expectations.expect(waitForPresenter(*presenter,
                                             [&] {
                                                 return presenter->state() ==
                                                        ViewerGpuPresenter::State::Unsupported;
                                             }),
                            "a Rejected target is terminal: " +
                                bloom::ui::test::describeViewerGpuPresenter(*presenter));
        expectations.expect(presenter->surfaceSafeToDestroy(),
                            "a Rejected target with safe proof is safe to destroy");
        expectations.expect(port->forgotten.size() == 1U && port->forgotten[0] == id,
                            "the Rejected terminal record is forgotten");
    }

    if (expectations.failures() == 0) {
        std::cout << "PASS: ViewerGpuPresenter refusal/forget semantics (refusals retained and "
                     "never forgotten, proven terminals forgotten once)\n";
    }
    return expectations.failures() == 0 ? 0 : 1;
}
