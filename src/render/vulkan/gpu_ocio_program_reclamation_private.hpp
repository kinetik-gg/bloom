#pragma once

#include "gpu_device_private.hpp"

#include <memory>

namespace bloom::render::ocio_program_detail {

// Embedded before the program's native members. The generation owns only intrusive links; the
// registration retains the generation until all native members have been destroyed.
struct ProgramRetirementNode final {
    ProgramRetirementNode* previous = nullptr;
    ProgramRetirementNode* next = nullptr;
    void* program = nullptr;
    bool (*retired)(void*, vulkan_detail::DeviceAllocatorState&) noexcept = nullptr;
    void (*destroy)(void*) noexcept = nullptr;
    std::uint64_t epoch = 0;
    bool pending = false;
    bool draining = false;
};

struct ProgramRegistration final {
    ProgramRegistration() = default;
    ProgramRegistration(const ProgramRegistration&) = delete;
    ProgramRegistration& operator=(const ProgramRegistration&) = delete;
    ~ProgramRegistration();

    [[nodiscard]] bool attach(std::shared_ptr<vulkan_detail::DeviceAllocatorState> state,
                              void* program,
                              bool (*retired)(void*, vulkan_detail::DeviceAllocatorState&) noexcept,
                              void (*destroy)(void*) noexcept) noexcept;
    void enqueue() noexcept;

    std::shared_ptr<vulkan_detail::DeviceAllocatorState> owner;
    ProgramRetirementNode node;
};

void drainPrograms(vulkan_detail::DeviceAllocatorState& state) noexcept;
void closeProgramOwner(vulkan_detail::DeviceAllocatorState& state) noexcept;
[[nodiscard]] GpuOcioProgramRetirementStats
programRetirementStats(vulkan_detail::DeviceAllocatorState& state) noexcept;

} // namespace bloom::render::ocio_program_detail
