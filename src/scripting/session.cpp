#include <bloom/scripting/session.hpp>

#include <fstream>
#include <iterator>
#include <stdexcept>
#include <utility>
#include <vector>

namespace bloom::scripting {
namespace {

[[nodiscard]] SessionCreateResult failure(std::string code, std::string message) {
    return SessionCreateResult(
        nullptr, SessionDiagnostic{.code = std::move(code), .message = std::move(message)});
}

[[nodiscard]] std::optional<std::pair<std::unique_ptr<host::PublicationCoordinator>,
                                      std::unique_ptr<platform::StagedArtifactCoordinator>>>
makePublicationServices() {
    auto publication = host::PublicationCoordinator::create();
    if (!publication.has_value()) {
        return std::nullopt;
    }
    auto artifacts = platform::StagedArtifactCoordinator::create({});
    if (!artifacts) {
        return std::nullopt;
    }
    return std::make_pair(std::make_unique<host::PublicationCoordinator>(std::move(*publication)),
                          std::make_unique<platform::StagedArtifactCoordinator>(
                              std::move(artifacts).takeCoordinator()));
}

[[nodiscard]] std::optional<std::vector<std::byte>> readArchive(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return std::nullopt;
    }
    stream.seekg(0, std::ios::end);
    const auto size = stream.tellg();
    if (size < 0) {
        return std::nullopt;
    }
    stream.seekg(0, std::ios::beg);
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    if (!bytes.empty()) {
        stream.read(reinterpret_cast<char*>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size()));
        if (!stream) {
            return std::nullopt;
        }
    }
    return bytes;
}

} // namespace

SessionCreateResult Session::createNew(std::string projectName, std::string compositionName,
                                       const core::RationalTime duration,
                                       const document::CompositionFormat format) {
    auto base = std::unique_ptr<Session>(new Session());
    host::NewProjectSessionRequest request{.projectName = std::move(projectName),
                                           .compositionName = std::move(compositionName),
                                           .duration = duration,
                                           .format = format};
    auto hostResult = host::ProjectSession::createNew(base->identitySource_, std::move(request));
    return fromHostSession(std::move(base), std::move(hostResult), {});
}

SessionCreateResult Session::open(const std::filesystem::path& path) {
    if (path.empty()) {
        return failure("bloom.scripting.open-path-empty", "Open requires a project path");
    }
    const auto archive = readArchive(path);
    if (!archive.has_value()) {
        return failure("bloom.scripting.open-read-failed",
                       "The project archive could not be read: " + path.string());
    }
    const auto displayPath = host::ProjectDisplayPath::create(path);
    if (!displayPath.has_value()) {
        return failure("bloom.scripting.open-invalid-path", "The project path is invalid");
    }

    auto base = std::unique_ptr<Session>(new Session());
    auto hostResult = host::ProjectSession::createPreservedReadOnly(base->identitySource_, path);
    auto created = fromHostSession(std::move(base), std::move(hostResult), path);
    if (!created) {
        return created;
    }
    auto session = std::move(created).takeSession();
    auto memory = project::ProjectIoMemoryCoordinator::create();
    if (!memory.has_value()) {
        return failure("bloom.scripting.open-memory-unavailable",
                       "Project I/O memory could not be initialized");
    }
    auto operation = memory->createOperation();
    if (!operation.has_value()) {
        return failure("bloom.scripting.open-memory-unavailable",
                       "Project I/O operation memory could not be initialized");
    }
    const auto opened =
        host::openSessionArchive(*session->session_, *archive, displayPath, {}, *operation);
    if (!opened) {
        return failure("bloom.scripting.open-failed",
                       "The project archive was rejected by the host open pipeline");
    }
    session->displayPath_ = path;
    return SessionCreateResult(std::move(session), {});
}

SessionCreateResult Session::openReadOnly(const std::filesystem::path& path) {
    if (path.empty()) {
        return failure("bloom.scripting.open-path-empty", "Open requires a project path");
    }
    auto base = std::unique_ptr<Session>(new Session());
    auto hostResult = host::ProjectSession::createPreservedReadOnly(base->identitySource_, path);
    return fromHostSession(std::move(base), std::move(hostResult), path);
}

SessionCreateResult Session::fromHostSession(std::unique_ptr<Session> base,
                                             host::ProjectSessionCreateResult hostResult,
                                             std::filesystem::path displayPath) {
    if (!hostResult) {
        return failure("bloom.scripting.session-create-failed",
                       "The host could not create the project session");
    }
    auto services = makePublicationServices();
    if (!services.has_value()) {
        return failure("bloom.scripting.services-unavailable",
                       "Publication services could not be initialized");
    }
    base->session_ = std::make_unique<host::ProjectSession>(std::move(hostResult).takeSession());
    base->publicationCoordinator_ = std::move(services->first);
    base->artifactCoordinator_ = std::move(services->second);
    base->displayPath_ = std::move(displayPath);
    return SessionCreateResult(std::move(base), {});
}

bool Session::isValid() const noexcept { return session_ != nullptr && session_->isValid(); }

host::ProjectSessionStateSnapshot Session::state() const {
    return session_ == nullptr ? host::ProjectSessionStateSnapshot{} : session_->stateSnapshot();
}

host::DecodedProjectSnapshotResult Session::snapshotResult() const {
    if (session_ == nullptr) {
        throw std::logic_error("Bloom scripting session is not available");
    }
    return session_->decodedSnapshot();
}

document::Snapshot Session::snapshot() const {
    const auto result = snapshotResult();
    if (!result) {
        throw std::logic_error("Bloom scripting session has no decoded document");
    }
    return result.snapshot();
}

commands::CommandStack* Session::commandStack() noexcept {
    if (session_ == nullptr) {
        return nullptr;
    }
    return session_->liveDocumentAndStack().second;
}

document::Document* Session::document() noexcept {
    if (session_ == nullptr) {
        return nullptr;
    }
    return session_->liveDocumentAndStack().first;
}

SessionCommandResult Session::makeCommandResult(host::ProjectSessionCommandResult result) const {
    return {
        .status = result.status, .command = std::move(result.command), .diagnostic = std::nullopt};
}

SessionCommandResult Session::execute(commands::Transaction transaction) {
    if (session_ == nullptr) {
        return {};
    }
    return makeCommandResult(session_->execute(std::move(transaction)));
}

SessionCommandResult Session::undo() {
    if (session_ == nullptr) {
        return {};
    }
    return makeCommandResult(session_->undo());
}

SessionCommandResult Session::redo() {
    if (session_ == nullptr) {
        return {};
    }
    return makeCommandResult(session_->redo());
}

SessionCommandResult Session::executeJsonOperation(const std::string_view operation,
                                                   const Arguments& arguments,
                                                   const std::optional<std::string>& label) {
    if (session_ == nullptr) {
        return {};
    }
    auto created = registry_.create(operation, arguments);
    if (!created) {
        SessionCommandResult result;
        result.diagnostic = *created.diagnostic();
        return result;
    }
    const auto currentRevision = snapshot().revision();
    commands::Transaction transaction(label.value_or(std::string(operation)), currentRevision);
    if (!transaction.add(std::move(created).takeOperation())) {
        SessionCommandResult result;
        result.diagnostic =
            OperationDiagnostic{.code = "bloom.scripting.operation-empty",
                                .operationId = std::string(operation),
                                .argument = {},
                                .message = "The operation factory returned no operation"};
        return result;
    }
    return execute(std::move(transaction));
}

SessionSaveResult Session::save() {
    if (displayPath_.empty()) {
        return {.status = SessionSaveStatus::Failed,
                .code = "bloom.scripting.save-path-required",
                .message = "The session has no path; use saveAs"};
    }
    return saveTo(displayPath_, false);
}

SessionSaveResult Session::saveAs(const std::filesystem::path& path) {
    if (path.empty()) {
        return {.status = SessionSaveStatus::Failed,
                .code = "bloom.scripting.save-path-empty",
                .message = "Save As requires a project path"};
    }
    return saveTo(path, true);
}

SessionSaveResult Session::saveTo(const std::filesystem::path& path, const bool saveAsOperation) {
    if (!isValid()) {
        return {.status = SessionSaveStatus::InvalidSession,
                .code = "bloom.scripting.invalid-session",
                .message = "The project session is not available"};
    }
    if (session_->stateSnapshot().contentKind ==
        host::ProjectSessionContentKind::PreservedReadOnly) {
        return {.status = SessionSaveStatus::ReadOnly,
                .code = "bloom.scripting.read-only",
                .message = "A preserved read-only session cannot be edited or saved in place"};
    }
    host::SessionPathIntentCapture pathIntent;
    if (saveAsOperation) {
        const auto advanced = session_->advancePathIntentForSaveAs();
        if (!advanced) {
            return {.status = SessionSaveStatus::Failed,
                    .code = "bloom.scripting.save-intent-failed",
                    .message = "The Save As path intent could not be admitted"};
        }
        pathIntent = advanced.capture();
    } else {
        pathIntent = session_->capturePlainSavePathIntent();
    }
    auto memoryCoordinator = project::ProjectIoMemoryCoordinator::create();
    if (!memoryCoordinator.has_value()) {
        return {.status = SessionSaveStatus::Failed,
                .code = "bloom.scripting.save-memory-unavailable",
                .message = "Project I/O memory could not be initialized"};
    }
    auto operation = memoryCoordinator->createOperation();
    if (!operation.has_value()) {
        return {.status = SessionSaveStatus::Failed,
                .code = "bloom.scripting.save-memory-unavailable",
                .message = "Project I/O operation memory could not be initialized"};
    }
    host::SessionSaveRequest request{.targetPath = path,
                                     .overwritePolicy =
                                         platform::ArtifactOverwritePolicy::CreateOrReplace,
                                     .expectedTarget = std::nullopt,
                                     .limits = {},
                                     .intent = pathIntent};
    auto result = host::saveProjectSession(*session_, *publicationCoordinator_,
                                           *artifactCoordinator_, request, *operation);
    if (!result) {
        if (saveAsOperation) {
            static_cast<void>(session_->abandonSaveAsIntent(pathIntent));
        }
        return {.status = SessionSaveStatus::Failed,
                .code = "bloom.scripting.save-failed",
                .message = "The host save pipeline did not publish the project"};
    }
    displayPath_ = path;
    return {.status = SessionSaveStatus::Saved, .code = {}, .message = {}};
}

} // namespace bloom::scripting
