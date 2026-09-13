#pragma once

// Umbrella header (task F1, item F0). The timeline, properties, and assets editors that all lived
// in one pair of files now own a header and a translation unit each, and the shared authoring truth
// the Nodes canvas calls lives beside them; this header keeps including all four so every existing
// `#include <bloom/ui/composition_editors.hpp>` compiles unmodified.

#include <bloom/ui/assets_editor.hpp>
#include <bloom/ui/composition_authoring.hpp>
#include <bloom/ui/properties_editor.hpp>
#include <bloom/ui/timeline_editor.hpp>
