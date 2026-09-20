#pragma once

// Private (non-installed) trusted extraction helper shared by the in-process colour GPU producer
// (`ocio_gpu_program.cpp`) and the isolated `bloom-color-worker` process. It contains the only
// OCIO-dependent GPU shader/resource reflection code, so neither the host nor the helper duplicates
// it. The helper returns the portable `render::OcioGpuProgramDesc` with every field except the
// device-independent digests and the producer-owned content identity; the host owns those.
//
// OCIO types stay private to this private header and the translation units that include it; no
// public bloom/color header names an OCIO type.

#include "ocio_gpu_program_transport.hpp"

#include <bloom/render/ocio_gpu_program.hpp>

#include <OpenColorIO/OpenColorIO.h>

#include <string_view>

namespace OCIO = OCIO_NAMESPACE;

namespace bloom::color::detail {

// Reflects one OCIO processor's default GPU program into a portable descriptor. `stage` and
// `semanticsId` are caller-owned and written through unchanged. Every checked limit is enforced
// against the real OCIO reflection; an out-of-limit extraction is a typed refusal, never a
// truncated program. Never throws.
[[nodiscard]] OcioGpuProgramTransport
extractOcioGpuProgram(const OCIO::ConstProcessorRcPtr& processor, render::OcioGpuProgramStage stage,
                      std::string_view semanticsId,
                      const render::OcioGpuProgramLimits& limits) noexcept;

} // namespace bloom::color::detail
