#include "runtime.hpp"
#include "native.hpp"

#include <nanobind/stl/shared_ptr.h>
#include <nanobind/stl/string.h>

#include <fstream>
#include <iostream>
#include <stdexcept>

// CPython fixes this symbol spelling for the private _bloom module.
extern "C" PyObject* PyInit__bloom(); // NOLINT(bugprone-reserved-identifier)

namespace bloom::scripting::python {

struct EmbeddedPython::State final {
    PyThreadState* thread = nullptr;
    PyObject* console = nullptr;
    mutable std::mutex mutex;
    std::shared_ptr<NativeHost> host;
};

namespace {

void checkStatus(const PyStatus status) {
    if (PyStatus_Exception(status) != 0) {
        throw std::runtime_error(status.err_msg == nullptr ? "Python initialization failed"
                                                           : status.err_msg);
    }
}

} // namespace

EmbeddedPython::EmbeddedPython(const std::filesystem::path& executable)
    : state_(std::make_unique<State>()) {
    if (Py_IsInitialized() != 0) {
        throw std::runtime_error("Bloom owns one scripting interpreter per process (ADR 0022)");
    }
    if (PyImport_AppendInittab("_bloom", &PyInit__bloom) != 0) {
        throw std::runtime_error("Could not register the Bloom bridge");
    }
    PyConfig config;
    PyConfig_InitIsolatedConfig(&config);
    config.site_import = 0;
    config.install_signal_handlers = 0;
    const auto program = executable.empty() ? std::string("bloom-cli") : executable.string();
    try {
        checkStatus(PyConfig_SetBytesString(&config, &config.program_name, program.c_str()));
        checkStatus(Py_InitializeFromConfig(&config));
    } catch (...) {
        PyConfig_Clear(&config);
        throw;
    }
    PyConfig_Clear(&config);
    // nanobind initializes its native internals when its extension is imported. CPython owns
    // embedding; initialize the built-in bridge before using nanobind's object wrappers.
    PyObject* bridge = PyImport_ImportModule("_bloom");
    if (bridge == nullptr) {
        PyErr_Print();
        Py_FinalizeEx();
        throw std::runtime_error("Could not initialize the built-in Bloom bridge");
    }
    Py_DECREF(bridge);
    try {
        const auto installed = executable.parent_path().parent_path() / "lib/bloom/python";
        const auto package =
            !executable.empty() && std::filesystem::is_directory(installed / "bloom")
                ? installed
                : std::filesystem::path(BLOOM_PYTHON_PACKAGE_PATH);
        auto sys = nb::module_::import_("sys");
        sys.attr("path").attr("insert")(0, package.string());
        auto console = nb::module_::import_("bloom._console").attr("Console")();
        state_->console = console.release().ptr();
    } catch (...) {
        Py_FinalizeEx();
        throw;
    }
    state_->thread = PyEval_SaveThread();
}

EmbeddedPython::~EmbeddedPython() {
    if (state_->thread != nullptr) {
        PyEval_RestoreThread(state_->thread);
        {
            try {
                nb::module_::import_("bloom.tasks").attr("_shutdown")();
            } catch (const nb::python_error& error) {
                std::cerr << error.what() << '\n';
            }
        }
        Py_XDECREF(state_->console);
        {
            const std::lock_guard lock(state_->mutex);
            state_->host.reset();
        }
        Py_FinalizeEx();
    }
}

void EmbeddedPython::bind(std::shared_ptr<NativeHost> host) {
    const nb::gil_scoped_acquire acquire;
    nb::module_::import_("bloom._state").attr("bind")(host);
    const std::lock_guard lock(state_->mutex);
    state_->host = std::move(host);
}

void EmbeddedPython::bindLive(std::shared_ptr<Session> session,
                              std::function<PythonContext()> context,
                              std::function<bool()> cancelled) {
    const nb::gil_scoped_acquire acquire;
    auto host = std::make_shared<NativeHost>(std::move(session));
    host->contextProvider = std::move(context);
    host->headless = false;
    host->cancelProvider = std::move(cancelled);
    bind(std::move(host));
}

void EmbeddedPython::requestCancellation() {
    const std::lock_guard lock(state_->mutex);
    if (state_->host)
        state_->host->requestCancellation();
}

std::vector<runtime::TaskSnapshot> EmbeddedPython::tasks() const {
    const std::lock_guard lock(state_->mutex);
    return state_->host ? state_->host->taskSnapshots() : std::vector<runtime::TaskSnapshot>{};
}

ExecutionResult EmbeddedPython::execute(const std::string_view source, const bool interactive) {
    const nb::gil_scoped_acquire acquire;
    {
        const std::lock_guard lock(state_->mutex);
        if (state_->host)
            state_->host->prepareExecution();
    }
    try {
        const auto result = nb::borrow<nb::object>(state_->console)
                                .attr("execute")(std::string(source), interactive);
        return {.output = nb::cast<std::string>(result[0]),
                .succeeded = nb::cast<bool>(result[1]),
                .incomplete = nb::cast<bool>(result[2])};
    } catch (const nb::python_error& error) {
        return {.output = error.what(), .succeeded = false, .incomplete = false};
    }
}

int EmbeddedPython::runFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input || input.tellg() > std::streamoff{1024} * 1024) {
        std::cerr << "Python script is unreadable or exceeds 1 MiB\n";
        return 2;
    }
    input.seekg(0);
    const std::string source(std::istreambuf_iterator<char>(input), {});
    const auto result = execute(source);
    std::cout << result.output;
    return result.succeeded ? 0 : 4;
}

int EmbeddedPython::interactive() {
    std::cout << "Bloom Python — bloom is pre-imported. Ctrl-D exits.\n";
    std::string source;
    std::string line;
    while (std::cout << (source.empty() ? ">>> " : "... "), std::getline(std::cin, line)) {
        source += line + '\n';
        const auto result = execute(source, true);
        std::cout << result.output;
        if (!result.incomplete) {
            source.clear();
        }
    }
    return 0;
}

} // namespace bloom::scripting::python
