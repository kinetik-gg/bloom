#include "gpu_shader_compiler_process.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <system_error>
#include <utility>
#include <variant>

namespace bloom::color::gpu_shader_detail {

BoundedHash hashFileBounded(const std::string& path, std::size_t ceiling,
                            GpuShaderDeadline deadline,
                            const platform::ProcessCancellation& cancel) {
    using Clock = std::chrono::steady_clock;
    BoundedHash result;
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > ceiling) {
        result.error = GpuShaderCompileError::InvalidTool;
        return result;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        result.error = GpuShaderCompileError::InvalidTool;
        return result;
    }
    core::Sha256Hasher hasher;
    std::array<std::byte, 1u << 20> chunk{};
    std::uintmax_t remaining = size;
    while (remaining > 0) {
        if (cancel && cancel()) {
            result.error = GpuShaderCompileError::Cancelled;
            return result;
        }
        if (Clock::now() >= deadline) {
            result.error = GpuShaderCompileError::Timeout;
            return result;
        }
        const auto want = static_cast<std::streamsize>(
            std::min<std::uintmax_t>(remaining, static_cast<std::uintmax_t>(chunk.size())));
        input.read(reinterpret_cast<char*>(chunk.data()), want);
        const auto got = input.gcount();
        if (got <= 0) {
            result.error = GpuShaderCompileError::InvalidTool;
            return result;
        }
        if (!hasher.update(
                std::span<const std::byte>(chunk.data(), static_cast<std::size_t>(got)))) {
            result.error = GpuShaderCompileError::IoFailure;
            return result;
        }
        remaining -= static_cast<std::uintmax_t>(got);
    }
    result.digest = hasher.finalize();
    return result;
}

ToolRun runProcess(const std::string& executable, const std::vector<std::string>& arguments,
                   std::uint64_t addressSpaceBytes, std::uint32_t openFiles,
                   platform::ProcessDeadline deadline, const platform::ProcessCancellation& cancel,
                   std::size_t maxOutput) {
    using platform::ProcessError;
    using platform::ProcessFailure;
    using platform::ProcessOptions;
    using platform::ProcessSupervisor;

    ToolRun run;
    ProcessOptions options;
    options.executable = executable;
    options.arguments = arguments;
    options.addressSpaceBytes = addressSpaceBytes;
    options.openFiles = openFiles;
    auto launched = ProcessSupervisor::launch(options);
    if (auto* failure = std::get_if<ProcessFailure>(&launched)) {
        run.failure = *failure;
        return run;
    }
    auto child = std::move(std::get<std::unique_ptr<ProcessSupervisor>>(launched));
    // ProcessSupervisor::read is a fixed-length protocol read: it fills the whole span or fails,
    // and an early end-of-stream discards any partial bytes. A byte at a time is the only correct
    // way to stream unknown-length tool output without losing it. Diagnostics are small and SPIR-V
    // goes to a file, so the extra syscalls are bounded.
    std::array<std::byte, 1> buffer{};
    while (true) {
        auto read = child->read(buffer, deadline, cancel);
        if (auto* failure = std::get_if<ProcessFailure>(&read)) {
            if (failure->code == ProcessError::Crashed && failure->nativeCode == 0)
                break; // End of stream: the child closed stdout.
            run.failure = *failure;
            return run;
        }
        if (run.output.size() < maxOutput)
            run.output.push_back(static_cast<std::uint8_t>(buffer[0]));
        else
            run.outputOverflow = true;
    }
    auto finished = child->finish(deadline);
    if (auto* failure = std::get_if<ProcessFailure>(&finished)) {
        if (failure->code == ProcessError::Crashed) {
            run.exitStatus = failure->nativeCode;
            return run;
        }
        run.failure = *failure;
        return run;
    }
    return run;
}

} // namespace bloom::color::gpu_shader_detail
