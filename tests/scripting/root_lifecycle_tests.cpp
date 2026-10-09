#include <native.hpp>
#include <root_retirements_private.hpp>

#include <bloom/render/packed_rgba8_storage.hpp>
#include <nanobind/stl/shared_ptr.h>
#include <nanobind/stl/string.h>
#include <packed_host_retirement_private.hpp>

#include <chrono>
#include <condition_variable>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace bloom::scripting::python {
struct PythonRootRetirementsTestAccess final {
    static auto provider(const NativeHost& host) { return host.gpuExportProvider_; }
    static std::weak_ptr<Session> session(const NativeHost& host) { return host.session_; }
    static void beforePublish(PythonRootRetirements& roots, void (*hook)(void*)) {
        roots.impl_->beforeRegistration = hook;
    }
};
} // namespace bloom::scripting::python
namespace bloom::render::detail {
struct PackedHostRetirementTestAccess final {
    static void beforeFree(PackedHostRetirementOwner& owner, void (*callback)(void*) noexcept,
                           void* data) {
        owner.impl_->state->beforeFree = callback;
        owner.impl_->state->testContext = data;
    }
};
} // namespace bloom::render::detail
namespace {
using namespace bloom;
using namespace bloom::scripting::python;
using namespace std::chrono_literals;
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
struct Gate final {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false, released = false;
    ~Gate() { release(); }
    static void hold(void* data) noexcept {
        auto& gate = *static_cast<Gate*>(data);
        std::unique_lock lock(gate.mutex);
        gate.entered = true;
        gate.changed.notify_all();
        gate.changed.wait(lock, [&] { return gate.released; });
    }
    void await() {
        std::unique_lock lock(mutex);
        require(changed.wait_for(lock, 2s, [&] { return entered; }),
                "real retirement free reached");
    }
    void release() {
        {
            const std::lock_guard lock(mutex);
            released = true;
        }
        changed.notify_all();
    }
};
void execute(EmbeddedPython& runtime, const char* source) {
    const auto result = runtime.execute(source);
    if (!result.succeeded)
        throw std::runtime_error(result.output);
}
void embeddedGcAndIncompleteFinalize() {
    std::shared_ptr<render::PackedHostRetirementOwner> retiredOwner;
    {
        EmbeddedPython runtime;
        Gate gate; // Exception unwind releases actual free BEFORE runtime shutdown/finalization.
        execute(runtime, "import _bloom, gc\n_bloom._test_host = _bloom.Host()\n");
        std::shared_ptr<PythonRootRetirements> roots;
        std::weak_ptr<scripting::Session> weakSession;
        std::weak_ptr<host::GpuExportProvider> weakProvider;
        {
            const nb::gil_scoped_acquire acquire;
            roots = moduleRootRetirements();
            auto host = nb::cast<std::shared_ptr<NativeHost>>(
                nb::module_::import_("_bloom").attr("_test_host"));
            weakSession = PythonRootRetirementsTestAccess::session(*host);
            weakProvider = PythonRootRetirementsTestAccess::provider(*host);
        }
        execute(runtime, "del _bloom._test_host\ngc.collect()\n");
        require(weakSession.expired() && !weakProvider.expired(),
                "early Python GC destroys heavy Session but retains light closing provider");
        static_cast<void>(roots->collectCompleted()); // This C++ caller has no GIL.
        require(weakProvider.expired(), "proven early GC record pruned outside GIL");
        const auto beforeFault = roots->snapshot();
        PythonRootRetirementsTestAccess::beforePublish(
            *roots, [](void*) { throw std::runtime_error("post-scheduler constructor failure"); });
        const auto fault = runtime.execute("_bloom.Host()\n");
        PythonRootRetirementsTestAccess::beforePublish(*roots, nullptr);
        require(!fault.succeeded &&
                    fault.output.find("post-scheduler constructor failure") != std::string::npos &&
                    roots->snapshot().participants == beforeFault.participants,
                "actual Python constructor exception cleans scheduler off GIL without abort");
        execute(runtime, "import threading, time\n"
                         "_bloom._test_host = _bloom.Host()\n"
                         "_bloom._test_ticket = _bloom._test_host.track_task('Retained ticket')\n"
                         "_bloom._test_ticket.finish('')\n"
                         "_bloom._stop = threading.Event()\n"
                         "_bloom._ticks = 0\n"
                         "def _tick():\n"
                         "    while not _bloom._stop.is_set():\n"
                         "        _bloom._ticks += 1\n"
                         "        time.sleep(0.001)\n"
                         "_bloom._test_thread = threading.Thread(target=_tick)\n"
                         "_bloom._test_thread.start()\n");
        std::shared_ptr<host::GpuExportProvider> provider;
        {
            const nb::gil_scoped_acquire acquire;
            auto host = nb::cast<std::shared_ptr<NativeHost>>(
                nb::module_::import_("_bloom").attr("_test_host"));
            provider = PythonRootRetirementsTestAccess::provider(*host);
        }
        retiredOwner = provider->ownedPackedHostRetirementOwner();
        require(retiredOwner && retiredOwner->prepare(), "owned Host worker starts off GIL");
        render::detail::PackedHostRetirementTestAccess::beforeFree(*retiredOwner, &Gate::hold,
                                                                   &gate);
        render::GpuProcessingPhaseBudget budget(1, retiredOwner);
        budget.setHostCapacity(1024, true);
        auto grant = budget.reserve({.fixedHostBytes = 1024, .sourceFree = true, .native = false});
        require(grant.lease != nullptr, "external C++ payload attempt admitted");
        auto storage = render::PackedRgba8Storage::create(32, grant.lease, 128);
        require(storage.hasValue() && storage.value()->promote(),
                "actual published payload exists");
        grant.lease.reset();
        const auto revision = budget.snapshot().admissionRevision;
        storage.value()->reset();
        gate.await();
        std::uint64_t ticksBefore = 0;
        {
            const nb::gil_scoped_acquire acquire;
            auto module = nb::module_::import_("_bloom");
            ticksBefore = nb::cast<std::uint64_t>(module.attr("_ticks"));
            require(!nb::cast<bool>(module.attr("_shutdown_owned_roots")(1)),
                    "bounded actual Python helper reports incomplete and does not finalize");
        }
        std::this_thread::sleep_for(20ms); // GIL released: independent Python continuation can run.
        {
            const nb::gil_scoped_acquire acquire;
            auto module = nb::module_::import_("_bloom");
            require(
                nb::cast<std::uint64_t>(module.attr("_ticks")) > ticksBefore &&
                    roots->snapshot().participants > 0 && !retiredOwner->exitAcknowledged() &&
                    budget.snapshot().ownedPackedHostBytes == 128 &&
                    budget.snapshot().admissionRevision == revision,
                "incomplete keeps interpreter responsive and provider/scheduler/backing retained");
            module.attr("_stop").attr("set")();
            module.attr("_test_thread").attr("join")();
        }
        gate.release();
        {
            const nb::gil_scoped_acquire acquire;
            auto module = nb::module_::import_("_bloom");
            require(nb::cast<bool>(module.attr("_shutdown_owned_roots")(2000)),
                    "actual free and worker exit collected before finalization");
            auto ticket = module.attr("_test_ticket");
            require(nb::cast<std::uint64_t>(ticket.attr("id")) > 0,
                    "retained completed TaskTicket remains safe after scheduler collection");
            static_cast<void>(ticket.attr("cancelled"));
            ticket.attr("cancel")();
            ticket.attr("progress")(0, nb::none(), "Retired");
            ticket.attr("finish")("");
            bool rejected = false;
            try {
                module.attr("_test_host").attr("track_task")("Closed");
            } catch (const nb::python_error&) {
                rejected = true;
            }
            require(rejected, "retained Host safely rejects scheduler methods after close");
        }
        require(roots->snapshot().participants == 0 && retiredOwner->snapshot().collected &&
                    budget.snapshot().totalHostClaimBytes == 0 &&
                    budget.snapshot().admissionRevision == revision + 1,
                "exact physical free/one revision and all records collected before PyFinalize");
    }
    require(Py_IsInitialized() == 0 && retiredOwner->snapshot().collected,
            "EmbeddedPython finalizes only after actual owned root exit and collection");
}
} // namespace
int main() {
    try {
        embeddedGcAndIncompleteFinalize();
        std::cout << "Embedded/manual/GC/constructor/GIL-progress/false-timeout/retained-ticket "
                     "lifecycle passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
