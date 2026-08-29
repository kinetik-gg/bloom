#include <bloom/ui/main_window.hpp>

#include <bloom/core/rational_time.hpp>
#include <bloom/core/sha256.hpp>
#include <bloom/document/color_settings.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/host/project_session.hpp>
#include <bloom/project/canonical_document.hpp>
#include <bloom/project/canonical_manifest.hpp>
#include <bloom/project/project_io_memory.hpp>
#include <bloom/project/save_archive.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/editor_registry.hpp>
#include <bloom/ui/project_host.hpp>
#include <bloom/ui/workspace_host.hpp>

#include <zlib.h>

#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QLabel>
#include <QString>
#include <QWidget>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <numeric>
#include <optional>
#include <source_location>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// MainWindow's read-only placeholder surface (task R1, issue #74): drives a REAL ProjectHost
// through a real preserved-read-only Open (a hand-assembled {1,1}-container archive -- the fixture
// technique is a trimmed duplicate of src/host/tests/session_open_tests.cpp's
// buildManifestSidePreservedReadOnlyArchiveOrAbort()/ArchiveWriter, since src modules may not reach
// across a sibling module's tests/ directory, exactly as that file's own comment documents for its
// own duplicate of src/project/tests/zip_container_test_support.hpp) inside a full, offscreen
// MainWindow, and asserts the placeholder mechanism switches presentation state without any
// CompositionSession/ProjectHost surgery.

namespace {

using bloom::host::ProjectSessionContentKind;
using bloom::ui::EditorRegistry;
using bloom::ui::MainWindow;
using bloom::ui::ProjectHost;
using bloom::ui::WorkspaceHost;

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

    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

class TempDirectory final {
  public:
    TempDirectory() {
        std::array<char, 64> pattern{};
        constexpr std::string_view prefix = "/tmp/bloom-mainwindow-placeholder-XXXXXX";
        std::ranges::copy(prefix, pattern.begin());
        const auto* result = ::mkdtemp(pattern.data());
        if (result != nullptr) {
            path_ = result;
        }
    }

    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;
    ~TempDirectory() {
        if (path_.empty()) {
            return;
        }
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] bool isValid() const noexcept { return !path_.empty(); }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

  private:
    std::filesystem::path path_;
};

template <typename Predicate> [[nodiscard]] bool waitUntil(Predicate predicate) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 4'000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (std::invoke(predicate)) {
            return true;
        }
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return std::invoke(predicate);
}

[[nodiscard]] bool waitUntilIdle(const ProjectHost& host) {
    return waitUntil([&host] { return !host.isBusy(); });
}

// ---------------------------------------------------------------------------------------------
// Archive fixture plumbing (trimmed duplicate of src/host/tests/session_open_tests.cpp -- see the
// file comment above).
// ---------------------------------------------------------------------------------------------

constexpr std::uint64_t kGenerousOperationBudget = 32ULL << 20U;

[[nodiscard]] bloom::project::ProjectIoOperationMemory makeOperation() {
    auto coordinator = bloom::project::ProjectIoMemoryCoordinator::create(kGenerousOperationBudget);
    if (!coordinator.has_value()) {
        std::abort();
    }
    auto operation =
        coordinator->createOperation(kGenerousOperationBudget, kGenerousOperationBudget);
    if (!operation.has_value()) {
        std::abort();
    }
    return std::move(*operation);
}

[[nodiscard]] std::array<std::uint8_t, 32> ascendingDigestBytes() noexcept {
    std::array<std::uint8_t, 32> bytes{};
    std::iota(bytes.begin(), bytes.end(), std::uint8_t{0});
    return bytes;
}

[[nodiscard]] bloom::document::ColorSettings neutralColorSettings() {
    return bloom::document::makeBloomNeutralColorSettingsV1(
        bloom::core::Sha256Digest::fromBytes(ascendingDigestBytes()));
}

[[nodiscard]] std::uint32_t crc32Of(const std::span<const std::byte> payload) noexcept {
    const auto value = crc32_z(0L, reinterpret_cast<const Bytef*>(payload.data()), payload.size());
    return static_cast<std::uint32_t>(value);
}

struct EntrySpec final {
    std::string localName;
    std::string centralName;
    std::uint16_t localFlags = 0x0800;
    std::uint16_t centralFlags = 0x0800;
    std::uint16_t localMethod = 0;
    std::uint16_t centralMethod = 0;
    std::vector<std::byte> data;
    std::uint32_t localCompressedSize = 0;
    std::uint32_t centralCompressedSize = 0;
    std::uint32_t localUncompressedSize = 0;
    std::uint32_t centralUncompressedSize = 0;
    std::uint32_t localCrc = 0;
    std::uint32_t centralCrc = 0;
    std::vector<std::byte> localExtra;
    std::vector<std::byte> centralExtra;
    std::string centralComment;
    std::uint16_t versionMadeBy = 0x0314;
    std::uint32_t externalAttrs = 0100644U << 16U;
    std::uint16_t diskNumberStart = 0;
};

[[nodiscard]] EntrySpec makeStoredEntry(const std::string& name,
                                        const std::span<const std::byte> payload) {
    EntrySpec spec;
    spec.localName = name;
    spec.centralName = name;
    spec.data.assign(payload.begin(), payload.end());
    spec.localCompressedSize = spec.centralCompressedSize =
        static_cast<std::uint32_t>(payload.size());
    spec.localUncompressedSize = spec.centralUncompressedSize =
        static_cast<std::uint32_t>(payload.size());
    spec.localCrc = spec.centralCrc = crc32Of(payload);
    return spec;
}

class ArchiveWriter final {
  public:
    std::uint64_t appendLocal(const EntrySpec& entry) {
        const auto offset = bytes_.size();
        appendU32(0x04034b50U);
        appendU16(20);
        appendU16(entry.localFlags);
        appendU16(entry.localMethod);
        appendU16(0);
        appendU16(0);
        appendU32(entry.localCrc);
        appendU32(entry.localCompressedSize);
        appendU32(entry.localUncompressedSize);
        appendU16(static_cast<std::uint16_t>(entry.localName.size()));
        appendU16(static_cast<std::uint16_t>(entry.localExtra.size()));
        appendText(entry.localName);
        appendRaw(entry.localExtra);
        appendRaw(entry.data);
        return offset;
    }

    void appendCentral(const EntrySpec& entry, const std::uint32_t localHeaderOffset) {
        appendU32(0x02014b50U);
        appendU16(entry.versionMadeBy);
        appendU16(20);
        appendU16(entry.centralFlags);
        appendU16(entry.centralMethod);
        appendU16(0);
        appendU16(0);
        appendU32(entry.centralCrc);
        appendU32(entry.centralCompressedSize);
        appendU32(entry.centralUncompressedSize);
        appendU16(static_cast<std::uint16_t>(entry.centralName.size()));
        appendU16(static_cast<std::uint16_t>(entry.centralExtra.size()));
        appendU16(static_cast<std::uint16_t>(entry.centralComment.size()));
        appendU16(entry.diskNumberStart);
        appendU16(0);
        appendU32(entry.externalAttrs);
        appendU32(localHeaderOffset);
        appendText(entry.centralName);
        appendRaw(entry.centralExtra);
        appendText(entry.centralComment);
    }

    void appendEocd(const std::uint16_t diskNumber, const std::uint16_t diskWithCd,
                    const std::uint16_t entriesThisDisk, const std::uint16_t entriesTotal,
                    const std::uint32_t cdSize, const std::uint32_t cdOffset) {
        appendU32(0x06054b50U);
        appendU16(diskNumber);
        appendU16(diskWithCd);
        appendU16(entriesThisDisk);
        appendU16(entriesTotal);
        appendU32(cdSize);
        appendU32(cdOffset);
        appendU16(0);
    }

    [[nodiscard]] std::uint64_t size() const { return bytes_.size(); }
    [[nodiscard]] const std::vector<std::byte>& bytes() const { return bytes_; }

  private:
    void appendU16(const std::uint16_t value) {
        bytes_.push_back(static_cast<std::byte>(value & 0xFFU));
        bytes_.push_back(static_cast<std::byte>((value >> 8U) & 0xFFU));
    }
    void appendU32(const std::uint32_t value) {
        for (unsigned shift = 0; shift < 32U; shift += 8U) {
            bytes_.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
        }
    }
    void appendText(const std::string_view text) {
        for (const char character : text) {
            bytes_.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
        }
    }
    void appendRaw(const std::span<const std::byte> raw) {
        bytes_.insert(bytes_.end(), raw.begin(), raw.end());
    }

    std::vector<std::byte> bytes_;
};

[[nodiscard]] std::vector<std::byte> buildConformingArchive(const EntrySpec& manifest,
                                                            const EntrySpec& document) {
    ArchiveWriter writer;
    const auto manifestOffset = writer.appendLocal(manifest);
    const auto documentOffset = writer.appendLocal(document);
    const auto centralStart = writer.size();
    writer.appendCentral(manifest, static_cast<std::uint32_t>(manifestOffset));
    writer.appendCentral(document, static_cast<std::uint32_t>(documentOffset));
    const auto centralSize = writer.size() - centralStart;
    writer.appendEocd(0, 0, 2, 2, static_cast<std::uint32_t>(centralSize),
                      static_cast<std::uint32_t>(centralStart));
    return writer.bytes();
}

[[nodiscard]] std::vector<std::byte> minimalCanonicalDocumentBytesOrAbort() {
    const auto duration = bloom::core::RationalTime::create(240, 24);
    if (!duration.has_value()) {
        std::abort();
    }
    auto newProject =
        bloom::document::makeNewProject("Untitled Project", "Main Composition", *duration);
    bloom::document::Document document{std::move(newProject.project)};
    auto snapshot = document.snapshot();
    const auto colorSettings = neutralColorSettings();

    std::vector<char> payloadScratch(64, '\0');
    std::vector<std::size_t> sortScratch(64, 0);
    const bloom::project::CanonicalDocumentV1 request{.snapshot = &snapshot,
                                                      .colorSettings = &colorSettings,
                                                      .payloadScratch = payloadScratch,
                                                      .sortScratch = sortScratch};
    const auto documentSize = bloom::project::canonicalDocumentSize(request);
    if (!documentSize) {
        std::abort();
    }
    std::vector<char> documentText(*documentSize.value());
    if (!bloom::project::encodeCanonicalDocument(request, documentText)) {
        std::abort();
    }
    std::vector<std::byte> documentBytes(documentText.size());
    std::memcpy(documentBytes.data(), documentText.data(), documentText.size());
    return documentBytes;
}

// A {1,1}-container archive (mirrors session_open_tests.cpp's
// testManifestSidePreservedReadOnly/buildManifestSidePreservedReadOnlyArchiveOrAbort exactly): a
// manifest declaring containerVersion {1,1} over an otherwise valid {1,0} document classifies
// PreservedReadOnlyRequired on the manifest side.
[[nodiscard]] std::vector<std::byte> buildManifestSidePreservedReadOnlyArchiveOrAbort() {
    const std::string manifestText =
        R"({"format":"org.kinetik.bloom.project","containerVersion":{"major":1,"minor":1},)"
        R"("document":{"path":"document.json","schemaVersion":{"major":1,"minor":0}},)"
        R"("requirements":[]})";
    std::vector<std::byte> manifestBytes(manifestText.size());
    std::memcpy(manifestBytes.data(), manifestText.data(), manifestText.size());
    const auto documentBytes = minimalCanonicalDocumentBytesOrAbort();

    auto manifestEntry = makeStoredEntry("manifest.json", manifestBytes);
    auto documentEntry = makeStoredEntry("document.json", documentBytes);
    return buildConformingArchive(manifestEntry, documentEntry);
}

// A minimal, valid, unverified two-entry archive that classifies Opened -- built through
// bloom::project's own real archive writer (no hand-rolled ZIP entries needed, since this one
// declares the container version this build actually supports).
[[nodiscard]] std::vector<std::byte> buildEditableArchiveBytesOrAbort() {
    const auto duration = bloom::core::RationalTime::create(240, 24);
    if (!duration.has_value()) {
        std::abort();
    }
    auto newProject =
        bloom::document::makeNewProject("Editable Reopened Project", "Main Composition", *duration);
    bloom::document::Document document{std::move(newProject.project)};
    auto snapshot = document.snapshot();
    const auto colorSettings = neutralColorSettings();

    const bloom::project::CanonicalManifestV1 manifest{.documentSchemaVersion = {1, 0},
                                                       .requirements = {}};
    const bloom::project::CanonicalDocumentV1 documentInput{.snapshot = &snapshot,
                                                            .colorSettings = &colorSettings};
    auto built = bloom::project::buildVerifiedSaveArchive(
        manifest, documentInput, bloom::project::SaveArchiveLimits{}, makeOperation());
    if (!built) {
        std::abort();
    }
    const auto bytes = built.archive()->bytes();
    return {bytes.begin(), bytes.end()};
}

void writeFileOrAbort(const std::filesystem::path& path, const std::span<const std::byte> bytes) {
    std::ofstream stream(path, std::ios::binary);
    if (!bytes.empty()) {
        stream.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    }
    if (!stream) {
        std::abort();
    }
}

// ---------------------------------------------------------------------------------------------
// A small stand-in EditorRegistry matching MainWindow::resetCompositingLayout()'s five fixed
// editor ids (mirrors tests/ui_smoke.cpp's testRegistry()) -- this test drives the placeholder
// switch, not the real editors, so plain QLabel stand-ins are enough.
// ---------------------------------------------------------------------------------------------

[[nodiscard]] bool registerStandInEditors(EditorRegistry& registry) {
    const auto addTestEditor = [&registry](std::string id, QString name) {
        return registry.registerEditor(
            {.id = std::move(id), .displayName = std::move(name), .create = [](QWidget* parent) {
                 return new QLabel("Workspace test editor", parent);
             }});
    };
    return addTestEditor("bloom.viewer", "Compositor") && addTestEditor("bloom.nodes", "Nodes") &&
           addTestEditor("bloom.timeline", "Timeline") && addTestEditor("bloom.media", "Media") &&
           addTestEditor("bloom.properties", "Properties");
}

// ---------------------------------------------------------------------------------------------
// The placeholder switch itself: preserved-read-only Open -> placeholder shown, workspace hidden,
// Save/Save As disabled; editable Open on top of that -> workspace returns, actions re-enable.
// ---------------------------------------------------------------------------------------------

void testPlaceholderSwitchesOnPreservedReadOnlyOpenAndBack(Expectations& expectations) {
    TempDirectory directory;
    if (!directory.isValid()) {
        expectations.expect(false, "placeholder switch: temp directory is available");
        return;
    }
    const auto preservedPath = directory.path() / "preserved.bloom";
    const auto editablePath = directory.path() / "editable.bloom";
    writeFileOrAbort(preservedPath, buildManifestSidePreservedReadOnlyArchiveOrAbort());
    writeFileOrAbort(editablePath, buildEditableArchiveBytesOrAbort());

    EditorRegistry registry;
    expectations.expect(registerStandInEditors(registry),
                        "placeholder switch: the stand-in editors register");

    bloom::runtime::TaskScheduler scheduler;
    ProjectHost projectHost(scheduler);
    auto [initialDocument, initialCommandStack] = projectHost.liveDocumentAndStack();
    expectations.expect(initialDocument != nullptr && initialCommandStack != nullptr,
                        "placeholder switch: the fresh ProjectHost exposes a live document/stack");
    if (initialDocument == nullptr || initialCommandStack == nullptr) {
        return;
    }
    bloom::ui::CompositionSession compositionSession(*initialDocument, *initialCommandStack,
                                                     projectHost.lowestCompositionId());

    MainWindow window(registry, compositionSession, projectHost);
    window.show();
    QApplication::processEvents();

    auto* newProjectAction = window.findChild<QAction*>("newProjectAction");
    auto* openProjectAction = window.findChild<QAction*>("openProjectAction");
    auto* saveProjectAction = window.findChild<QAction*>("saveProjectAction");
    auto* saveProjectAsAction = window.findChild<QAction*>("saveProjectAsAction");
    expectations.expect(newProjectAction != nullptr && openProjectAction != nullptr &&
                            saveProjectAction != nullptr && saveProjectAsAction != nullptr,
                        "placeholder switch: the file menu actions exist");
    if (newProjectAction == nullptr || openProjectAction == nullptr ||
        saveProjectAction == nullptr || saveProjectAsAction == nullptr) {
        return;
    }

    // Baseline: a fresh, editable, decoded project shows the workspace.
    expectations.expect(!window.isShowingReadOnlyPlaceholder(),
                        "placeholder switch: a fresh editable project shows the workspace, not "
                        "the placeholder");
    expectations.expect(window.workspaceHost()->isVisible(),
                        "placeholder switch: the workspace is visible at baseline");
    expectations.expect(saveProjectAction->isEnabled() && saveProjectAsAction->isEnabled(),
                        "placeholder switch: Save/Save As are enabled at baseline");

    // A real preserved-read-only Open (task R1, issue #74's target scenario).
    projectHost.beginOpen(preservedPath);
    expectations.expect(waitUntilIdle(projectHost),
                        "placeholder switch: the preserved-read-only open reaches a terminal "
                        "state");
    expectations.expect(projectHost.stateSnapshot().contentKind ==
                            ProjectSessionContentKind::PreservedReadOnly,
                        "placeholder switch: the installed content kind is PreservedReadOnly");

    expectations.expect(window.isShowingReadOnlyPlaceholder(),
                        "placeholder switch: the placeholder is shown for preserved-read-only "
                        "content");
    expectations.expect(!window.workspaceHost()->isVisible(),
                        "placeholder switch: the workspace is hidden behind the placeholder");
    expectations.expect(!saveProjectAction->isEnabled() && !saveProjectAsAction->isEnabled(),
                        "placeholder switch: Save/Save As are disabled for read-only content");
    expectations.expect(newProjectAction->isEnabled() && openProjectAction->isEnabled(),
                        "placeholder switch: New/Open remain enabled (only busy disables them)");

    auto* fileNameLabel = window.findChild<QLabel*>("readOnlyPlaceholderFileName");
    auto* bodyLabel = window.findChild<QLabel*>("readOnlyPlaceholderBody");
    auto* headingLabel = window.findChild<QLabel*>("readOnlyPlaceholderHeading");
    expectations.expect(fileNameLabel != nullptr && bodyLabel != nullptr && headingLabel != nullptr,
                        "placeholder switch: the placeholder's labels exist");
    if (fileNameLabel != nullptr) {
        expectations.expect(fileNameLabel->text() ==
                                QString::fromStdString(preservedPath.filename().string()),
                            "placeholder switch: the placeholder names the opened file");
    }
    if (bodyLabel != nullptr) {
        expectations.expect(
            bodyLabel->text().contains(QString::fromStdString(preservedPath.filename().string())),
            "placeholder switch: the placeholder body also names the file");
    }
    if (headingLabel != nullptr) {
        expectations.expect(!headingLabel->text().isEmpty(),
                            "placeholder switch: the placeholder has a heading");
    }

    // Opening an editable archive on top of the preserved-read-only content returns the workspace.
    projectHost.beginOpen(editablePath);
    expectations.expect(waitUntilIdle(projectHost),
                        "placeholder switch: the editable reopen reaches a terminal state");
    expectations.expect(projectHost.stateSnapshot().contentKind ==
                            ProjectSessionContentKind::DecodedDocument,
                        "placeholder switch: the reopened content kind is DecodedDocument");

    expectations.expect(!window.isShowingReadOnlyPlaceholder(),
                        "placeholder switch: the workspace returns for decoded content");
    expectations.expect(window.workspaceHost()->isVisible(),
                        "placeholder switch: the workspace is visible again");
    expectations.expect(saveProjectAction->isEnabled() && saveProjectAsAction->isEnabled(),
                        "placeholder switch: Save/Save As re-enable for editable content");
}

// ---------------------------------------------------------------------------------------------
// Save a Copy from the read-only placeholder (task SC1, issue #77): open a preserved-read-only
// archive -> the "Save a Copy…" action enables -> drive requestSaveCopy() through the SAME Save As
// dialog seam to a temp target -> the published file byte-equals the retained original; the
// session's stateSnapshot (content kind, generations, path) is identical before and after; opening
// an editable archive afterward clears the retention and disables the action again.
// ---------------------------------------------------------------------------------------------

void testSaveCopyFromReadOnlyPlaceholder(Expectations& expectations) {
    TempDirectory directory;
    if (!directory.isValid()) {
        expectations.expect(false, "save a copy: temp directory is available");
        return;
    }
    const auto preservedPath = directory.path() / "preserved.bloom";
    const auto copyTargetPath = directory.path() / "preserved-copy.bloom";
    const auto editablePath = directory.path() / "editable.bloom";
    const auto preservedBytes = buildManifestSidePreservedReadOnlyArchiveOrAbort();
    writeFileOrAbort(preservedPath, preservedBytes);
    writeFileOrAbort(editablePath, buildEditableArchiveBytesOrAbort());

    EditorRegistry registry;
    expectations.expect(registerStandInEditors(registry),
                        "save a copy: the stand-in editors register");

    bloom::runtime::TaskScheduler scheduler;
    ProjectHost projectHost(scheduler);
    auto [initialDocument, initialCommandStack] = projectHost.liveDocumentAndStack();
    if (initialDocument == nullptr || initialCommandStack == nullptr) {
        expectations.expect(false,
                            "save a copy: the fresh ProjectHost exposes a live document/stack");
        return;
    }
    bloom::ui::CompositionSession compositionSession(*initialDocument, *initialCommandStack,
                                                     projectHost.lowestCompositionId());

    MainWindow window(registry, compositionSession, projectHost);
    window.show();
    QApplication::processEvents();

    auto* saveCopyAction = window.findChild<QAction*>("saveProjectCopyAction");
    expectations.expect(saveCopyAction != nullptr, "save a copy: the menu action exists");
    if (saveCopyAction == nullptr) {
        return;
    }
    expectations.expect(!saveCopyAction->isEnabled() && !projectHost.canSaveCopy(),
                        "save a copy: disabled at baseline for a fresh editable project");

    projectHost.beginOpen(preservedPath);
    expectations.expect(waitUntilIdle(projectHost),
                        "save a copy: the preserved-read-only open reaches a terminal state");
    expectations.expect(projectHost.stateSnapshot().contentKind ==
                            ProjectSessionContentKind::PreservedReadOnly,
                        "save a copy: the installed content kind is PreservedReadOnly");
    expectations.expect(saveCopyAction->isEnabled() && projectHost.canSaveCopy(),
                        "save a copy: enabled once preserved-read-only content with retained "
                        "bytes is installed");

    const auto beforeSnapshot = projectHost.stateSnapshot();

    bool copyDialogInvoked = false;
    projectHost.setSaveAsPathProvider([&] {
        copyDialogInvoked = true;
        return std::optional(copyTargetPath);
    });
    int copyFinishedCount = 0;
    bloom::ui::ProjectHostOperationOutcome copyOutcome =
        bloom::ui::ProjectHostOperationOutcome::Failed;
    QObject::connect(&projectHost, &ProjectHost::copyFinished,
                     [&](const bloom::ui::ProjectHostOperationOutcome outcome, const QString&) {
                         ++copyFinishedCount;
                         copyOutcome = outcome;
                     });

    projectHost.requestSaveCopy();
    expectations.expect(waitUntilIdle(projectHost),
                        "save a copy: the async copy reaches a terminal state");
    expectations.expect(copyDialogInvoked,
                        "save a copy: requestSaveCopy() drove the SAME Save As dialog seam");
    expectations.expect(copyFinishedCount == 1 &&
                            copyOutcome == bloom::ui::ProjectHostOperationOutcome::Published,
                        "save a copy: the copy publishes");
    expectations.expect(std::filesystem::exists(copyTargetPath),
                        "save a copy: the target file was written");

    // Byte-exact copy: the published file equals the ORIGINAL source archive bytes read from disk.
    std::ifstream publishedStream(copyTargetPath, std::ios::binary);
    const std::string publishedText((std::istreambuf_iterator<char>(publishedStream)),
                                    std::istreambuf_iterator<char>());
    const std::string sourceText(reinterpret_cast<const char*>(preservedBytes.data()),
                                 preservedBytes.size());
    expectations.expect(publishedText == sourceText,
                        "save a copy: the published file byte-equals the retained original");

    // Session stateSnapshot is identical before/after (kind, generations, path): Save Copy never
    // proposes ProjectSession changes.
    const auto afterSnapshot = projectHost.stateSnapshot();
    expectations.expect(
        afterSnapshot.contentKind == beforeSnapshot.contentKind &&
            afterSnapshot.resultAcceptanceGeneration == beforeSnapshot.resultAcceptanceGeneration &&
            afterSnapshot.openIntentGeneration == beforeSnapshot.openIntentGeneration &&
            afterSnapshot.pathIntentGeneration == beforeSnapshot.pathIntentGeneration &&
            afterSnapshot.pathIntentKind == beforeSnapshot.pathIntentKind &&
            afterSnapshot.displayPath == beforeSnapshot.displayPath,
        "save a copy: session stateSnapshot (kind, generations, path) is identical before and "
        "after");

    // Retention is cleared once an editable open replaces the preserved-read-only content; the
    // action disables again.
    projectHost.beginOpen(editablePath);
    expectations.expect(waitUntilIdle(projectHost),
                        "save a copy: the editable reopen reaches a terminal state");
    expectations.expect(projectHost.stateSnapshot().contentKind ==
                            ProjectSessionContentKind::DecodedDocument,
                        "save a copy: the reopened content kind is DecodedDocument");
    expectations.expect(!saveCopyAction->isEnabled() && !projectHost.canSaveCopy(),
                        "save a copy: disabled again once the preserved-read-only content is "
                        "replaced (retention cleared)");
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    Expectations expectations;
    testPlaceholderSwitchesOnPreservedReadOnlyOpenAndBack(expectations);
    testSaveCopyFromReadOnlyPlaceholder(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
