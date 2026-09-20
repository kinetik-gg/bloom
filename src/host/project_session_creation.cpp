#include <bloom/host/project_session.hpp>

#include <bloom/document/new_project.hpp>
#include <bloom/host/bloom_neutral_profile.hpp>

#include <exception>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>

// New-project construction for ProjectSession. Split out of project_session.cpp so both stay a
// readable size; this is the same class, only the factory tail moved. See project_session.hpp for
// the contract (a blank new project is a valid document with no composition; the seeded overload is
// the explicit opt-in).

namespace bloom::host {

ProjectSessionCreateResult
ProjectSession::installNewProject(ProjectSessionIdentitySource& identitySource,
                                  document::Project project) {
    auto document = std::make_unique<document::Document>(std::move(project));
    auto commandStack = std::make_unique<commands::CommandStack>(*document);
    const auto cleanRevision = document->snapshot().revision();
    const auto projectSessionId = identitySource.issue();
    if (!projectSessionId.has_value()) {
        return ProjectSessionCreateResult(ProjectSessionCreateStatus::RuntimeIdentityExhausted);
    }
    // A brand-new project installs the immutable Bloom Neutral v1 built-in color settings
    // (docs/architecture/color-management.md: "New version 1 projects use the immutable built-in
    // configuration Bloom Neutral v1"), keyed by the reviewed, checked-in
    // kBloomNeutralV1ConfigDigest constant (bloom_neutral_profile.hpp) -- no roundTrip, schemaMinor
    // stays 0, and retainedRequirements stays empty, exactly as for any other freshly authored (not
    // reopened) document.
    return ProjectSessionCreateResult(
        ProjectSession(*projectSessionId, std::move(document), std::move(commandStack),
                       DecodedProjectEditability::Editable, cleanRevision, std::nullopt,
                       document::makeBloomNeutralColorSettingsV1(kBloomNeutralV1ConfigDigest),
                       std::nullopt, 0, {}));
}

ProjectSessionCreateResult ProjectSession::createNew(ProjectSessionIdentitySource& identitySource,
                                                     NewProjectSessionRequest request) {
    try {
        auto created = document::makeNewProject(std::move(request.projectName),
                                                std::move(request.compositionName),
                                                request.duration, request.format);
        return installNewProject(identitySource, std::move(created.project));
    } catch (const std::bad_alloc&) {
        return ProjectSessionCreateResult(ProjectSessionCreateStatus::ResourceUnavailable);
    } catch (const std::logic_error&) {
        return ProjectSessionCreateResult(ProjectSessionCreateStatus::InvalidNewProject);
    }
}

ProjectSessionCreateResult
ProjectSession::createNewBlank(ProjectSessionIdentitySource& identitySource,
                               std::string projectName) {
    try {
        auto created = document::makeNewProject(std::move(projectName));
        return installNewProject(identitySource, std::move(created.project));
    } catch (const std::bad_alloc&) {
        return ProjectSessionCreateResult(ProjectSessionCreateStatus::ResourceUnavailable);
    } catch (const std::logic_error&) {
        return ProjectSessionCreateResult(ProjectSessionCreateStatus::InvalidNewProject);
    }
}

} // namespace bloom::host
