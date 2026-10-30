#include "native.hpp"

#include <nanobind/stl/optional.h>
#include <nanobind/stl/shared_ptr.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/unique_ptr.h>

#include <iostream>
#include <thread>

namespace bloom::scripting::python {

void bindHost(nb::module_& module) {
    auto roots = PythonRootRetirements::create();
    // Constructor failure leaves ownership with this scope; successful construction transfers
    // the exact holder to the capsule, including if later module attribute assignment fails.
    auto capsule = [&] {
        auto* holder = new std::shared_ptr<PythonRootRetirements>(roots);
        try {
            return nb::capsule(holder, [](void* pointer) noexcept {
                delete static_cast<std::shared_ptr<PythonRootRetirements>*>(pointer);
            });
        } catch (...) {
            delete holder;
            throw;
        }
    }();
    module.attr("_root_retirements") = capsule;
    nb::class_<TaskTicket>(module, "TaskTicket")
        .def_prop_ro("id", &TaskTicket::id)
        .def_prop_ro("cancelled", &TaskTicket::cancelled)
        .def("cancel", &TaskTicket::cancel)
        .def("progress", &TaskTicket::progress)
        .def("finish", &TaskTicket::finish);
    nb::class_<NativeHost>(module, "Host")
        .def(nb::new_([roots](const std::string& project) {
                 return std::make_shared<NativeHost>(project, roots);
             }),
             nb::arg("project") = "")
        .def("snapshot", &NativeHost::snapshot)
        .def("schemas", &NativeHost::schemas)
        .def("transact", &NativeHost::transact)
        .def("history", &NativeHost::history)
        .def("save", &NativeHost::save)
        .def("set_color_settings", &NativeHost::setColorSettings, nb::arg("config"),
             nb::arg("working_space") = nb::none())
        .def("render", &NativeHost::render)
        .def("events", &NativeHost::events)
        .def("tasks", &NativeHost::tasks)
        .def("track_task", &NativeHost::trackTask, nb::keep_alive<0, 1>())
        .def("cancel", &NativeHost::cancel)
        .def("context", &NativeHost::context)
        .def_prop_ro("cancellation_requested", &NativeHost::cancellationRequested)
        .def_prop_ro("cancellation_epoch", &NativeHost::cancellationEpoch)
        .def_ro("headless", &NativeHost::headless);
    module.attr("version") = BLOOM_PYTHON_VERSION;
    module.attr("facade_version") = "1.0";
    module.def(
        "_shutdown_owned_roots",
        [roots](const std::int64_t timeout) {
            if (timeout < 0 || timeout > 60000)
                throw nb::value_error("Shutdown timeout must be in 0..60000 milliseconds");
            beginPythonRootShutdown(roots);
            const nb::gil_scoped_release release;
            return roots->shutdownAndWait(std::chrono::milliseconds(timeout));
        },
        nb::arg("timeout_ms") = 10000);
    module.def("_collect_retired_roots", [roots] {
        const nb::gil_scoped_release release;
        return roots->collectCompleted();
    });
    module.def("_root_retirement_snapshot", [roots] {
        const auto snapshot = roots->snapshot();
        nb::dict result;
        result["roots"] = snapshot.roots;
        result["participants"] = snapshot.participants;
        result["active_calls"] = snapshot.activeCalls;
        result["closing"] = snapshot.closing;
        return result;
    });
    module.def("_finish_owned_roots", [roots] { finishPythonRootShutdown(roots); });
    // Also covers direct `import _bloom; Host()` without importing the Python package.
    nb::module_::import_("atexit").attr("register")(module.attr("_finish_owned_roots"));
}

std::shared_ptr<PythonRootRetirements> moduleRootRetirements() {
    auto capsule = nb::cast<nb::capsule>(nb::module_::import_("_bloom").attr("_root_retirements"));
    return *static_cast<std::shared_ptr<PythonRootRetirements>*>(capsule.data());
}
void beginPythonRootShutdown(const std::shared_ptr<PythonRootRetirements>& roots) {
    roots->beginShutdown(); // Cancels native tickets before waiting for Python task producers.
    const auto modules = nb::cast<nb::dict>(nb::module_::import_("sys").attr("modules"));
    if (modules.contains("bloom.tasks"))
        modules["bloom.tasks"].attr("_shutdown")();
    if (modules.contains("bloom._state"))
        modules["bloom._state"].attr("_release_for_shutdown")();
}
void preparePythonRootShutdown(const std::shared_ptr<PythonRootRetirements>& roots) noexcept {
    for (;;) {
        try {
            beginPythonRootShutdown(roots);
        } catch (const nb::python_error& error) {
            std::cerr << "Python producer shutdown incomplete: " << error.what() << '\n';
            const nb::gil_scoped_release release;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue; // An exception is not a producer barrier or permission to finalize.
        } catch (const std::exception& error) {
            std::cerr << "Python root shutdown incomplete: " << error.what() << '\n';
            const nb::gil_scoped_release release;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        return;
    }
}
void finishPythonRootShutdown(const std::shared_ptr<PythonRootRetirements>& roots) noexcept {
    preparePythonRootShutdown(roots);
    for (;;) {
        const nb::gil_scoped_release release;
        if (roots->shutdownAndWait(std::chrono::seconds(10)))
            return;
        std::cerr << "Python root retirement pending; interpreter/module retained\n";
    }
}

} // namespace bloom::scripting::python

NB_MODULE(_bloom, module) { bloom::scripting::python::bindHost(module); }
