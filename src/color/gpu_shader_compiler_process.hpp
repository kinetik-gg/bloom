#pragma once

// Private to bloom_color_gpu_shader. Not installed and not part of the public header set.

#include <bloom/color/gpu_shader_compiler.hpp>
#include <bloom/core/sha256.hpp>
#include <bloom/platform/process_supervisor.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace bloom::color::gpu_shader_detail {

struct ToolRun {
    std::optional<platform::ProcessFailure> failure;
    int exitStatus = 0;
    std::vector<std::uint8_t> output;
    bool outputOverflow = false;
};

// Incremental, cancellable content verification of a bounded executable. The hash is computed in
// chunked reads with cancellation and deadline checks between chunks so a large binary cannot block
// a cancel request behind one monolithic read+hash.
struct BoundedHash {
    std::optional<core::Sha256Digest> digest;
    std::optional<GpuShaderCompileError> error;
};

BoundedHash hashFileBounded(const std::string& path, std::size_t ceiling,
                            GpuShaderDeadline deadline,
                            const platform::ProcessCancellation& cancel);

// Bounded, cancellable run of one absolute-path child. No shell is ever involved. stdout is drained
// to EOF while the child runs so a chatty tool cannot block on a full pipe, and the caller's
// cancellation and deadline are honored during the read. stderr is discarded by the platform
// supervisor; glslangValidator reports diagnostics on stdout and those are captured.
ToolRun runProcess(const std::string& executable, const std::vector<std::string>& arguments,
                   std::uint64_t addressSpaceBytes, std::uint32_t openFiles,
                   platform::ProcessDeadline deadline, const platform::ProcessCancellation& cancel,
                   std::size_t maxOutput);

} // namespace bloom::color::gpu_shader_detail
