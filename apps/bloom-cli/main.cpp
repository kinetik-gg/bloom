#include <bloom/document/node_definition_registry.hpp>
#include <bloom/output/output_analysis.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/scripting/json_script.hpp>
#include <bloom/scripting/render.hpp>
#include <bloom/scripting/session.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

namespace document = bloom::document;
namespace output = bloom::output;
namespace runtime = bloom::runtime;
namespace scripting = bloom::scripting;

void printUsage() {
    std::cerr << "usage: bloom-cli new <file> | open <file> | run <script.json> | "
                 "render [--project <file>] (--frame N | --range A-B) --preset <id> --out <dir>\n";
}

[[nodiscard]] std::optional<std::string> readText(const std::filesystem::path& path) {
    std::ifstream stream(path);
    if (!stream) {
        return std::nullopt;
    }
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

[[nodiscard]] std::optional<std::uint64_t> unsignedInteger(std::string_view value) {
    try {
        std::size_t consumed = 0;
        const auto parsed = std::stoull(std::string(value), &consumed);
        return consumed == value.size() ? std::optional(parsed) : std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
}

[[nodiscard]] std::optional<output::OutputPresetV1> preset(std::string_view id) {
    if (id == "PngRgba8SrgbV1") {
        return output::OutputPresetV1::PngRgba8SrgbV1;
    }
    if (id == "FlatExrRgba32fLinRec709SceneV1") {
        return output::OutputPresetV1::FlatExrRgba32fLinRec709SceneV1;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::pair<std::uint64_t, std::uint64_t>>
frameRange(std::string_view value) {
    const auto separator = value.find('-');
    if (separator == std::string_view::npos) {
        return std::nullopt;
    }
    const auto first = unsignedInteger(value.substr(0, separator));
    const auto last = unsignedInteger(value.substr(separator + 1));
    if (!first.has_value() || !last.has_value()) {
        return std::nullopt;
    }
    return std::pair{*first, *last};
}

[[nodiscard]] int commandNew(const std::filesystem::path& path) {
    auto created = scripting::Session::createNew();
    if (!created) {
        std::cerr << "error[" << created.diagnostic().code << "]: " << created.diagnostic().message
                  << '\n';
        return 2;
    }
    auto session = std::move(created).takeSession();
    const auto saved = session->saveAs(path);
    if (!saved.succeeded()) {
        std::cerr << "error[" << saved.code << "]: " << saved.message << '\n';
        return 3;
    }
    std::cout << "created " << path << '\n';
    return 0;
}

[[nodiscard]] int commandOpen(const std::filesystem::path& path) {
    auto opened = scripting::Session::open(path);
    if (!opened) {
        std::cerr << "error[" << opened.diagnostic().code << "]: " << opened.diagnostic().message
                  << '\n';
        return 2;
    }
    const auto session = std::move(opened).takeSession();
    const auto state = session->state();
    std::cout << "opened " << path << " revision="
              << (state.currentRevision.has_value() ? state.currentRevision->value() : 0) << '\n';
    return 0;
}

[[nodiscard]] int commandRun(const std::filesystem::path& path) {
    const auto text = readText(path);
    if (!text.has_value()) {
        std::cerr << "error[bloom-cli.script-read]: could not read " << path << '\n';
        return 2;
    }
    auto parsed = scripting::parseScriptJson(*text);
    if (!parsed) {
        const auto* diagnostic = parsed.diagnostic();
        std::cerr << "error[" << diagnostic->code << "] at " << diagnostic->byteOffset << ": "
                  << diagnostic->message << '\n';
        return 2;
    }
    auto created = scripting::Session::createNew();
    if (!created) {
        std::cerr << "error[" << created.diagnostic().code << "]: " << created.diagnostic().message
                  << '\n';
        return 3;
    }
    auto session = std::move(created).takeSession();
    for (const auto& transaction : parsed.transactions()) {
        const auto result =
            session->executeJsonOperation(transaction.operation, transaction.arguments);
        if (result.diagnostic.has_value()) {
            std::cerr << "error[" << result.diagnostic->code << "] "
                      << result.diagnostic->operationId << " " << result.diagnostic->argument
                      << ": " << result.diagnostic->message << '\n';
            return 4;
        }
        if (!result.succeeded()) {
            std::cerr << "error[bloom-cli.command-rejected] " << transaction.operation << '\n';
            return 5;
        }
    }
    std::cout << "executed " << parsed.transactions().size()
              << " transaction(s), revision=" << session->snapshot().revision().value() << '\n';
    return 0;
}

[[nodiscard]] int commandRender(int argc, char** argv) {
    std::optional<std::filesystem::path> project;
    std::optional<std::uint64_t> frame;
    std::optional<std::pair<std::uint64_t, std::uint64_t>> range;
    std::optional<output::OutputPresetV1> selectedPreset;
    std::filesystem::path outputDirectory;
    for (int index = 2; index < argc; ++index) {
        const std::string_view option = argv[index];
        if ((option == "--project" || option == "--out" || option == "--preset" ||
             option == "--frame" || option == "--range") &&
            index + 1 >= argc) {
            std::cerr << "error[bloom-cli.option]: " << option << " needs a value\n";
            return 2;
        }
        if (option == "--project") {
            project = argv[++index];
        } else if (option == "--frame") {
            frame = unsignedInteger(argv[++index]);
            if (!frame.has_value()) {
                std::cerr << "error[bloom-cli.frame]: invalid frame\n";
                return 2;
            }
        } else if (option == "--range") {
            range = frameRange(argv[++index]);
            if (!range.has_value()) {
                std::cerr << "error[bloom-cli.range]: invalid range\n";
                return 2;
            }
        } else if (option == "--preset") {
            selectedPreset = preset(argv[++index]);
            if (!selectedPreset.has_value()) {
                std::cerr << "error[bloom-cli.preset]: unknown preset\n";
                return 2;
            }
        } else if (option == "--out") {
            outputDirectory = argv[++index];
        } else {
            std::cerr << "error[bloom-cli.option]: unknown option " << option << '\n';
            return 2;
        }
    }
    if ((frame.has_value() == range.has_value()) || !selectedPreset.has_value() ||
        outputDirectory.empty()) {
        printUsage();
        return 2;
    }
    std::unique_ptr<scripting::Session> session;
    auto opened =
        project.has_value() ? scripting::Session::open(*project) : scripting::Session::createNew();
    if (!opened) {
        std::cerr << "error[" << opened.diagnostic().code << "]: " << opened.diagnostic().message
                  << '\n';
        return 3;
    }
    session = std::move(opened).takeSession();
    const auto snapshot = session->snapshot();
    if (snapshot.project().compositions().empty()) {
        std::cerr << "error[bloom-cli.composition]: no composition exists\n";
        return 4;
    }
    const auto composition = snapshot.project().compositions().front().id();
    const auto* const extension =
        *selectedPreset == output::OutputPresetV1::PngRgba8SrgbV1 ? ".png" : ".exr";
    std::error_code outputError;
    std::filesystem::create_directories(outputDirectory, outputError);
    if (outputError) {
        std::cerr << "error[bloom-cli.out]: could not create output directory\n";
        return 4;
    }
    const auto destination = outputDirectory / ("frame" + std::string(extension));
    runtime::TaskSchedulerConfig config = runtime::TaskSchedulerConfig::defaults();
    config.rowBandWorkerCount = runtime::kSerialRowBandWorkers;
    runtime::TaskScheduler scheduler(config);
    runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    const auto result = scripting::Render::run(*session, scheduler, compiler,
                                               {.composition = composition,
                                                .frame = frame,
                                                .range = range,
                                                .preset = *selectedPreset,
                                                .destination = destination});
    std::cout << result.preservationReport << '\n';
    if (!result.succeeded) {
        std::cerr << "error[bloom-cli.render]: " << result.diagnostic << '\n';
        return 5;
    }
    std::cout << "rendered " << result.publishedFrames << " frame(s) to " << outputDirectory
              << '\n';
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        printUsage();
        return 2;
    }
    const std::string_view command = argv[1];
    if (command == "new" && argc == 3) {
        return commandNew(argv[2]);
    }
    if (command == "open" && argc == 3) {
        return commandOpen(argv[2]);
    }
    if (command == "run" && argc == 3) {
        return commandRun(argv[2]);
    }
    if (command == "render") {
        return commandRender(argc, argv);
    }
    printUsage();
    return 2;
}
