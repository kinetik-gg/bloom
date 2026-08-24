#pragma once

#include <bloom/core/rational_time.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/document/project.hpp>

#include <string>

namespace bloom::document {

struct NewProject {
    Project project;
    CompositionId initialCompositionId;
};

[[nodiscard]] NewProject makeNewProject(std::string projectName, std::string compositionName,
                                        core::RationalTime duration);

} // namespace bloom::document
