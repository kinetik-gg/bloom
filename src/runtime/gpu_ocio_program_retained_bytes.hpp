#pragma once

// The single seam for a native OCIO program's retained allocation charge. Isolating the getter here
// means the program-cache accounting has exactly one place to adapt if the native accessor changes,
// and no executor ever substitutes a descriptor-declared sample-byte estimate for the actual VMA
// retained bytes.

#include <bloom/render/gpu_ocio_program.hpp>

#include <cstdint>

namespace bloom::runtime::gpu_ocio_detail {

// Actual VMA bytes of the persistent LUT textures + uniform buffer + status buffer owned by the
// program (allocator rounding included). Owner-thread only; zero for a null/moved-from program.
[[nodiscard]] inline std::uint64_t
nativeRetainedAllocationBytes(const render::GpuOcioProgram& program) noexcept {
    return program.retainedAllocationBytes();
}

} // namespace bloom::runtime::gpu_ocio_detail
