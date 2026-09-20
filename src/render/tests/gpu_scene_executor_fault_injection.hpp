#ifndef BLOOM_RENDER_VULKAN_GPU_SCENE_EXECUTOR_FAULT_INJECTION_HPP
#define BLOOM_RENDER_VULKAN_GPU_SCENE_EXECUTOR_FAULT_INJECTION_HPP

// TEST-ONLY fault injection for the GPU scene executor tests. This header is never installed and is
// not part of the production API; only the test-only fault-instrumented native render closure (see
// src/render/tests/CMakeLists.txt) compiles it. It lets a test force the next
// GpuSolid/GpuComposite::poll() to observe a real submitted job as Pending (stalled), as a
// device-lost failure, or as an unproven unknown-fence failure, so the executor's retention and
// drain contracts can be exercised against a genuinely submitted native job (never a fake handle).
//
// The native poll hook is compiled only when BLOOM_GPU_SCENE_EXECUTOR_TEST_FAULT_INJECTION is
// defined. Nothing here allocates or calls Vulkan.

#include <atomic>
#include <cstdint>

namespace bloom::render::gpu_scene_executor_fault {

enum class PollFault : std::uint8_t { None, StallPending, DeviceLost, UnknownFence };

inline std::atomic<std::uint8_t>& cell() noexcept {
    static std::atomic<std::uint8_t> value{0};
    return value;
}

inline void set(const PollFault fault) noexcept { cell().store(static_cast<std::uint8_t>(fault)); }

[[nodiscard]] inline PollFault peek() noexcept { return static_cast<PollFault>(cell().load()); }

// One-shot for DeviceLost/UnknownFence; StallPending persists until clear().
[[nodiscard]] inline PollFault take() noexcept {
    const auto value = static_cast<PollFault>(cell().load());
    if (value != PollFault::StallPending) {
        cell().store(0);
    }
    return value;
}

inline void clear() noexcept { cell().store(0); }

} // namespace bloom::render::gpu_scene_executor_fault

#endif // BLOOM_RENDER_VULKAN_GPU_SCENE_EXECUTOR_FAULT_INJECTION_HPP
