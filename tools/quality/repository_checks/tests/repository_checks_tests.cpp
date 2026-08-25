#include "repository_checks.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using bloom::quality::RepositoryFinding;
using Path = std::filesystem::path;

class TemporaryRepository final {
  public:
    TemporaryRepository() {
        static std::atomic_uint64_t sequence{0};
        const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (auto attempt = std::uint64_t{0}; attempt < 100; ++attempt) {
            root_ = std::filesystem::temp_directory_path() /
                    ("bloom-repository-checks-" + std::to_string(timestamp) + '-' +
                     std::to_string(sequence.fetch_add(1)) + '-' + std::to_string(attempt));
            std::error_code error;
            if (std::filesystem::create_directory(root_, error)) {
                return;
            }
        }
        throw std::runtime_error("could not create a temporary repository");
    }

    TemporaryRepository(const TemporaryRepository&) = delete;
    auto operator=(const TemporaryRepository&) -> TemporaryRepository& = delete;
    TemporaryRepository(TemporaryRepository&&) = delete;
    auto operator=(TemporaryRepository&&) -> TemporaryRepository& = delete;

    ~TemporaryRepository() {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    [[nodiscard]] auto root() const -> const Path& { return root_; }

    void write(const Path& relative, const std::string_view content = {}) const {
        const auto path = root_ / relative;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream stream{path, std::ios::binary};
        stream.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!stream) {
            throw std::runtime_error("could not write fixture " + path.string());
        }
    }

    void addPolicyFiles() const {
        write("LICENSE", "Apache License\nVersion 2.0\n");
        write("NOTICE", "Bloom\n");
    }

  private:
    Path root_;
};

[[nodiscard]] auto containsCategory(const std::span<const RepositoryFinding> findings,
                                    const std::string_view category) -> bool {
    return std::ranges::any_of(
        findings, [category](const auto& finding) { return finding.category == category; });
}

[[nodiscard]] auto containsMessage(const std::span<const RepositoryFinding> findings,
                                   const std::string_view fragment) -> bool {
    return std::ranges::any_of(findings, [fragment](const auto& finding) {
        return finding.message.find(fragment) != std::string::npos;
    });
}

[[nodiscard]] auto hygieneFixture(const Path& relative, const std::string_view content = {})
    -> std::vector<RepositoryFinding> {
    TemporaryRepository repository;
    repository.addPolicyFiles();
    repository.write(relative, content);
    const auto files = std::vector<Path>{"LICENSE", "NOTICE", relative};
    return bloom::quality::scanRepositoryHygiene(repository.root(), files);
}

[[nodiscard]] auto architectureFixture(const Path& relative, const std::string_view content)
    -> std::vector<RepositoryFinding> {
    TemporaryRepository repository;
    repository.write(relative, content);
    const auto files = std::vector<Path>{relative};
    return bloom::quality::scanArchitectureBoundaries(repository.root(), files);
}

void expect(const bool condition, const std::string_view message, int& failures) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void testHygiene(int& failures) {
    expect(hygieneFixture("docs/example.md",
                          "password = \"<secret>\"\npath = \"/home/<user>/project\"\n")
               .empty(),
           "placeholder credentials and account paths are accepted", failures);

    const auto posixPath = hygieneFixture("docs/leak.md", "path: /home/alice/private/project\n");
    expect(containsCategory(posixPath, "machine-specific path"), "POSIX account paths are rejected",
           failures);

    const auto windowsPath =
        hygieneFixture("docs/leak.md", R"(path: C:\Users\alice\private\project)"
                                       "\n");
    expect(containsCategory(windowsPath, "machine-specific path"),
           "Windows account paths are rejected", failures);

    const auto secret = hygieneFixture("config/example.toml", "api_key = \"real-value-123\"\n");
    expect(containsCategory(secret, "credential"), "secret assignments are rejected", failures);

    const auto yamlSecret = hygieneFixture("config/example.yaml", "auth-token: live-token-value\n");
    expect(containsCategory(yamlSecret, "credential"),
           "unquoted YAML secret assignments are rejected", failures);

    const auto generated = hygieneFixture("build/output.o");
    expect(containsCategory(generated, "generated artifact"), "generated artifacts are rejected",
           failures);

    const auto sensitive = hygieneFixture("config/private.pem");
    expect(containsCategory(sensitive, "sensitive file"), "sensitive file types are rejected",
           failures);

    expect(hygieneFixture("src/scripting/python/bloom/__init__.py", "API_VERSION = 1\n").empty(),
           "bundled Bloom scripting sources are accepted", failures);
    expect(hygieneFixture("tests/fixtures/scripting/addon/register.py", "def register(): pass\n")
               .empty(),
           "scripting fixtures are accepted", failures);
    expect(hygieneFixture("examples/scripting/hello_bloom.py", "import bloom\n").empty(),
           "artist scripting examples are accepted", failures);

    const auto rootPython = hygieneFixture("scripts/quality_check.py", "print('check')\n");
    expect(containsCategory(rootPython, "python placement"),
           "Python cannot implement repository infrastructure", failures);
    const auto actionPython = hygieneFixture(".github/scripts/build.py", "print('build')\n");
    expect(containsCategory(actionPython, "python placement"),
           "Python cannot implement CI infrastructure", failures);
    const auto misplacedStub = hygieneFixture("tools/generated_api.pyi", "class Project: ...\n");
    expect(containsCategory(misplacedStub, "python placement"),
           "Python type stubs remain scoped to the scripting module", failures);

    {
        TemporaryRepository repository;
        const auto findings =
            bloom::quality::scanRepositoryHygiene(repository.root(), std::span<const Path>{});
        const auto count = std::ranges::count_if(
            findings, [](const auto& finding) { return finding.category == "license policy"; });
        expect(count == 2, "root LICENSE and NOTICE are both required", failures);
    }

    {
        TemporaryRepository repository;
        repository.addPolicyFiles();
        repository.write("third_party/example/source.cpp", "int example;\n");
        const auto files = std::vector<Path>{"LICENSE", "NOTICE", "third_party/example/source.cpp"};
        const auto findings = bloom::quality::scanRepositoryHygiene(repository.root(), files);
        expect(containsMessage(findings, "attribution inventory"),
               "vendored content requires a repository inventory", failures);
        expect(containsMessage(findings, "upstream license"),
               "vendored content requires its upstream license", failures);
    }

    {
        TemporaryRepository repository;
        repository.addPolicyFiles();
        repository.write("present.txt", "present\n");
        repository.write("build/ignored.txt", "ignored\n");
        const auto files = bloom::quality::repositoryFiles(repository.root());
        expect(std::ranges::find(files, Path{"present.txt"}) != files.end(),
               "native traversal includes repository files", failures);
        expect(std::ranges::find(files, Path{"deleted.txt"}) == files.end(),
               "native traversal cannot report deleted paths", failures);
        expect(std::ranges::find(files, Path{"build/ignored.txt"}) == files.end(),
               "native traversal excludes build trees", failures);
    }

    {
        TemporaryRepository repository;
        repository.write("present.txt", "present\n");
        const auto manifestBytes = std::string{"present.txt\0deleted.txt\0", 24};
        repository.write("files.manifest", manifestBytes);
        const auto files = bloom::quality::repositoryFilesFromNullManifest(
            repository.root(), repository.root() / "files.manifest");
        expect(files == std::vector<Path>{"present.txt"},
               "Git manifests retain existing paths and skip deleted tracked paths", failures);

        repository.write("invalid.manifest", "present.txt");
        auto rejectedInvalidManifest = false;
        try {
            static_cast<void>(bloom::quality::repositoryFilesFromNullManifest(
                repository.root(), repository.root() / "invalid.manifest"));
        } catch (const std::invalid_argument&) {
            rejectedInvalidManifest = true;
        }
        expect(rejectedInvalidManifest, "repository manifests must be NUL terminated", failures);
    }

    {
        const RepositoryFinding finding{"src/core/value.cpp", "architecture", "example", 7};
        expect(finding.render() == "src/core/value.cpp:7: architecture: example",
               "finding diagnostics include stable path, line, category, and message", failures);
    }
}

void testArchitecture(int& failures) {
    expect(architectureFixture("src/ui/panel.cpp", "#include <QString>\n").empty(),
           "Qt is allowed in UI sources", failures);

    const auto qtDocument = architectureFixture("src/document/model.cpp", "#include <QString>\n");
    expect(containsMessage(qtDocument, "Qt types"), "Qt is rejected outside UI and apps", failures);

    const auto crossBoundary =
        architectureFixture("src/document/model.cpp", "#include \"../ui/panel.hpp\"\n");
    expect(containsMessage(crossBoundary, "public include roots"),
           "relative cross-boundary includes are rejected", failures);

    const auto misplaced =
        architectureFixture("src/core/include/bloom/wrong.hpp", "#pragma once\n");
    expect(containsMessage(misplaced, "public header"), "misplaced public headers are rejected",
           failures);

    expect(
        architectureFixture("src/runtime/compiler.cpp", "#include <bloom/document/document.hpp>\n")
            .empty(),
        "declared lower-level dependencies are accepted", failures);

    const auto upward =
        architectureFixture("src/document/model.cpp", "#include <bloom/runtime/evaluation.hpp>\n");
    expect(containsMessage(upward, "src/document may not depend"),
           "upward module dependencies are rejected", failures);

    const auto coreDependency =
        architectureFixture("src/core/value.cpp", "#include <bloom/document/project.hpp>\n");
    expect(containsMessage(coreDependency, "src/core may not depend"),
           "core dependencies on higher modules are rejected", failures);

    expect(architectureFixture("src/ui/project_panel.cpp",
                               "#include <bloom/host/publication_coordinator.hpp>\n")
               .empty(),
           "UI may adapt any module", failures);

    const auto qtLink = architectureFixture("src/document/CMakeLists.txt",
                                            "target_link_libraries(example PRIVATE Qt6::Core)\n");
    expect(containsMessage(qtLink, "links directly to Qt"),
           "non-UI modules cannot link Qt through CMake", failures);

    const auto lineFinding = architectureFixture(
        "src/core/value.cpp", "// first line\n#include <bloom/document/project.hpp>\n");
    expect(std::ranges::any_of(lineFinding, [](const auto& finding) { return finding.line == 2; }),
           "architecture diagnostics preserve source line numbers", failures);
}

} // namespace

auto main() -> int {
    auto failures = 0;
    try {
        testHygiene(failures);
        testArchitecture(failures);
    } catch (const std::exception& error) {
        std::cerr << "Unexpected repository checker test failure: " << error.what() << '\n';
        return 1;
    }
    if (failures != 0) {
        std::cerr << failures << " repository checker test(s) failed\n";
        return 1;
    }
    return 0;
}
