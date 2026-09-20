#pragma once

#include <bloom/core/rational_time.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/document/project.hpp>

#include <string>

namespace bloom::document {

struct NewProject {
    Project project;
    // Invalid when the project has no composition at all: a new project starts blank (File > New
    // and application startup), and a composition is only present when a caller explicitly seeds
    // one. See makeNewProject()'s two overloads.
    CompositionId initialCompositionId;
};

// The natural default: a blank project carrying only its identity and name, with no composition.
// File > New and application startup use this overload; the empty project is a valid new document
// (docs/architecture/project-session.md's "Session Publication" already exposes no active
// composition for content with none).
[[nodiscard]] NewProject makeNewProject(std::string projectName);

// Explicit seed: the project additionally owns one composition with the requested name, duration,
// and format. Fixtures, headless scripting, and templates opt in by calling this overload; nothing
// in production silently seeds a composition the artist did not ask for.
[[nodiscard]] NewProject makeNewProject(std::string projectName, std::string compositionName,
                                        core::RationalTime duration, CompositionFormat format = {});

} // namespace bloom::document
