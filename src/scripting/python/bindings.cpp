#include "native.hpp"

#include <nanobind/stl/optional.h>
#include <nanobind/stl/shared_ptr.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/unique_ptr.h>

namespace bloom::scripting::python {

void bindHost(nb::module_& module) {
    nb::class_<TaskTicket>(module, "TaskTicket")
        .def_prop_ro("id", &TaskTicket::id)
        .def_prop_ro("cancelled", &TaskTicket::cancelled)
        .def("cancel", &TaskTicket::cancel)
        .def("progress", &TaskTicket::progress)
        .def("finish", &TaskTicket::finish);
    nb::class_<NativeHost>(module, "Host")
        .def(nb::init<const std::string&>(), nb::arg("project") = "")
        .def("snapshot", &NativeHost::snapshot)
        .def("schemas", &NativeHost::schemas)
        .def("transact", &NativeHost::transact)
        .def("history", &NativeHost::history)
        .def("save", &NativeHost::save)
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
}

} // namespace bloom::scripting::python

NB_MODULE(_bloom, module) { bloom::scripting::python::bindHost(module); }
