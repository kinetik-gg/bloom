#include "deferred_editor_teardown.hpp"
#include "packed_host_process_teardown.hpp"
#include <bloom/render/packed_rgba8_storage.hpp>

#include <bloom/commands/command_stack.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/runtime/gpu_prepared_upload_cache.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/application_shutdown_coordinator.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTimer>
#include <QWidget>

#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

using namespace bloom;
using namespace std::chrono_literals;
namespace {
struct BorrowedLifetime final {
    bool& alive;
    ~BorrowedLifetime() { alive = false; }
};
class DeferredPackedWidget final : public QWidget {
  public:
    DeferredPackedWidget(render::PackedRgba8Storage product, bool& alive, bool& safe)
        : product_(std::move(product)), alive_(alive), safe_(safe) {}
    ~DeferredPackedWidget() override { safe_ = alive_; }

  private:
    render::PackedRgba8Storage product_;
    bool& alive_;
    bool& safe_;
};
struct Gate final {
    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false, released = false;
    void wait() {
        std::unique_lock lock(mutex);
        entered = true;
        condition.wait(lock, [&] { return released; });
    }
    bool hasEntered() {
        std::lock_guard lock(mutex);
        return entered;
    }
    void release() {
        std::lock_guard lock(mutex);
        released = true;
        condition.notify_all();
    }
};
template <typename Predicate> bool until(Predicate predicate) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 2000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        if (predicate())
            return true;
        std::this_thread::yield();
    }
    return predicate();
}
} // namespace
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    int failures = 0;
    const auto expect = [&](bool value, const char* message) {
        if (!value) {
            ++failures;
            std::cerr << message << '\n';
        }
    };
    for (const bool throwing : {false, true}) {
        const auto owner = render::PackedHostRetirementOwner::create();
        runtime::MemoryBudgetLedger sourceLedger(64ULL << 30U, 48ULL << 30U);
        auto sourcePool = std::make_shared<runtime::OperationCache>(1ULL << 20U, sourceLedger);
        auto prepared = std::make_shared<runtime::GpuPreparedUploadCache>(sourcePool, owner);
        const auto window = render::ImageWindow::create(0, 0, 1, 1);
        const auto descriptor = render::Rgba32fImageDescriptor::create(
            *window.value(), *window.value(), core::PixelAspectRatio::square());
        auto imageBuilder = render::Rgba32fImageBuilder::create(*descriptor.value(), 16);
        auto frozen = std::move(*imageBuilder.value()).freeze();
        const auto sourcePin =
            std::make_shared<const render::Rgba32fImage>(std::move(*frozen.value()));
        runtime::TaskScheduler earlyScheduler;
        bool reached = false, dependencyAlive = true, deferredDestroyedSafely = false;
        try {
            bloom::app::PackedHostProcessTeardown teardown(earlyScheduler, owner, sourcePool);
            BorrowedLifetime dependency{dependencyAlive};
            bloom::app::DeferredEditorTeardown deferredEditors;
            auto product = std::async(std::launch::async, [&] {
                               prepared->store("shutdown-source", sourcePin);
                               render::GpuProcessingPhaseBudget budget(1, owner);
                               budget.setHostCapacity(4096, true);
                               auto grant = budget.reserve(
                                   {.fixedHostBytes = 4096, .sourceFree = true, .native = false});
                               if (!grant.lease)
                                   throw std::runtime_error("fixture phase admission");
                               auto storage =
                                   render::PackedRgba8Storage::create(16, grant.lease, 64);
                               if (!storage || !storage.value()->promote())
                                   throw std::runtime_error("fixture rooted storage");
                               return std::move(*storage.value());
                           }).get();
            expect(prepared->entryCount() == 1,
                   "common owner fixture actually retains prepared source before teardown");
            reached = product.ownership() == render::PackedRgba8Ownership::RootedPublished;
            auto* detached = new DeferredPackedWidget(std::move(product), dependencyAlive,
                                                      deferredDestroyedSafely);
            detached->deleteLater();
            if (throwing)
                throw std::runtime_error("ordinary startup exception after real owner start");
            // Actual product dies before the same application's teardown guard.
        } catch (const std::runtime_error&) {
            expect(throwing && reached, "only intended setup exception is recovered");
        }
        expect(deferredDestroyedSafely && !dependencyAlive && reached &&
                   owner->exitAcknowledged() && owner->snapshot().collected &&
                   owner->snapshot().allocations == 0,
               "normal early setup return/exception frees consumers before exit and collection");
        expect(sourcePool->retainedBytes() == 0 && sourcePin->pixels().size() == 1,
               "common teardown retires prepared charges while external source pin remains valid");
        prepared->store("closed-source", sourcePin);
        expect(prepared->entryCount() == 0,
               "common teardown irreversibly closes escaped prepared facade stores");
    }
    auto project =
        document::makeNewProject("Host shutdown", "Main", core::RationalTime::fromInteger(2));
    document::Document document(std::move(project.project));
    commands::CommandStack stack(document);
    ui::CompositionSession session(document, stack, project.initialCompositionId);
    runtime::TaskScheduler scheduler;
    ui::TaskUiBridge bridge(scheduler, nullptr, 1ms);
    Gate producer;
    ui::CompositionPreviewController controller(
        session, scheduler, bridge,
        [&](const document::Snapshot&, const runtime::PreviewRequestIdentity&, std::size_t,
            const std::vector<runtime::SnapshotParameterOverride>&, runtime::TaskContext&) {
            producer.wait();
            return runtime::TaskResult<ui::PreviewPreparationResultHandle>::cancelled();
        });
    ui::ApplicationShutdownCoordinator shutdown(controller, bridge);
    bool nativeComplete = false, hostExit = false, quiescent = false;
    int preHostCalls = 0;
    shutdown.setGpuExportRetirement([] {}, [&] { return nativeComplete; });
    shutdown.setPackedHostRetirement(
        [&] {
            ++preHostCalls;
            expect(scheduler.isQuiescent() && shutdown.gpuExportRetirementSatisfied() &&
                       shutdown.nativeSurfaceRetirementSatisfied(),
                   "host close must follow all producer/native prerequisites");
            controller.releasePublishedFramesForShutdown();
        },
        [&] { return hostExit; });
    QObject::connect(&shutdown, &ui::ApplicationShutdownCoordinator::shutdownQuiescent, &shutdown,
                     [&] { quiescent = true; });
    expect(until([&] { return producer.hasEntered(); }), "real preview producer reached");
    shutdown.beginShutdown();
    nativeComplete = true;
    std::size_t beats = 0;
    QTimer heartbeat;
    QObject::connect(&heartbeat, &QTimer::timeout, &shutdown, [&] { ++beats; });
    heartbeat.start(1);
    expect(until([&] { return beats >= 5; }) && preHostCalls == 0 && !quiescent,
           "active UI stays responsive and does not close host while producer is held");
    producer.release();
    expect(until([&] { return preHostCalls == 1; }) && !quiescent &&
               !shutdown.packedHostRetirementSatisfied(),
           "pre-host close happens once after producer termination, not before actual exit");
    shutdown.beginShutdown();
    const auto previousBeats = beats;
    expect(until([&] { return beats >= previousBeats + 5; }) && preHostCalls == 1 && !quiescent,
           "duplicate close and pending exit do not block or publish false quiescence");
    hostExit = true;
    expect(until([&] { return quiescent; }) && shutdown.packedHostRetirementSatisfied() &&
               preHostCalls == 1,
           "real exit acknowledgement releases final quiescence exactly once");
    heartbeat.stop();
    std::cout << (failures ? "FAIL" : "PASS") << ": packed host shutdown prerequisites\n";
    return failures ? 1 : 0;
}
