#include "gpu_ocio_program_reclamation_private.hpp"

#include <limits>
#include <mutex>
#include <thread>
#include <utility>

namespace bloom::render::ocio_program_detail {
namespace {
[[nodiscard]] bool exactOwner(const vulkan_detail::DeviceAllocatorState& state) noexcept {
    return state.owner == std::this_thread::get_id() &&
           state.ownerLifetimeToken == vulkan_detail::deviceOwnerLifetimeToken();
}
} // namespace

bool ProgramRegistration::attach(std::shared_ptr<vulkan_detail::DeviceAllocatorState> state,
                                 void* program,
                                 bool (*retired)(void*,
                                                 vulkan_detail::DeviceAllocatorState&) noexcept,
                                 void (*destroy)(void*) noexcept) noexcept {
    if (!state || !exactOwner(*state) || owner) {
        return false;
    }
    std::lock_guard lock(state->programRetirementMutex);
    if (state->programOwnerClosed ||
        state->registeredPrograms == std::numeric_limits<std::uint64_t>::max()) {
        return false;
    }
    node.epoch = state->presentationEpoch;
    node.program = program;
    node.retired = retired;
    node.destroy = destroy;
    node.next = state->programRetirementHead;
    if (node.next) {
        node.next->previous = &node;
    }
    state->programRetirementHead = &node;
    ++state->registeredPrograms;
    owner = std::move(state);
    return true;
}

ProgramRegistration::~ProgramRegistration() {
    if (!owner) {
        return;
    }
    // owner is destroyed after this block, never while the registry mutex is held.
    std::lock_guard lock(owner->programRetirementMutex);
    if (node.previous) {
        node.previous->next = node.next;
    } else {
        owner->programRetirementHead = node.next;
    }
    if (node.next) {
        node.next->previous = node.previous;
    }
    --owner->registeredPrograms;
    if (node.pending) {
        --owner->pendingPrograms;
    }
}

void ProgramRegistration::enqueue() noexcept {
    std::lock_guard lock(owner->programRetirementMutex);
    if (!node.pending) {
        node.pending = true;
        ++owner->pendingPrograms;
    }
}

void drainPrograms(vulkan_detail::DeviceAllocatorState& state) noexcept {
    if (!exactOwner(state)) {
        return;
    }
    {
        std::lock_guard lock(state.programRetirementMutex);
        if (state.programOwnerClosed || state.programDrainActive) {
            return;
        }
        state.programDrainActive = true;
    }
    for (;;) {
        ProgramRetirementNode* candidate = nullptr;
        {
            std::lock_guard lock(state.programRetirementMutex);
            for (auto* node = state.programRetirementHead; node; node = node->next) {
                if (node->pending && !node->draining) {
                    candidate = node;
                    node->draining = true;
                    break;
                }
            }
        }
        if (!candidate) {
            break;
        }
        // Neither the fence query nor native destruction runs under the list mutex. Pending
        // ownership remains registered until the final native member and registration are gone.
        if (candidate->retired(candidate->program, state)) {
            candidate->destroy(candidate->program);
        }
    }
    std::lock_guard lock(state.programRetirementMutex);
    for (auto* node = state.programRetirementHead; node; node = node->next) {
        node->draining = false;
    }
    state.programDrainActive = false;
}

void closeProgramOwner(vulkan_detail::DeviceAllocatorState& state) noexcept {
    if (!exactOwner(state)) {
        return;
    }
    drainPrograms(state);
    std::lock_guard lock(state.programRetirementMutex);
    state.programOwnerClosed = true;
}

GpuOcioProgramRetirementStats
programRetirementStats(vulkan_detail::DeviceAllocatorState& state) noexcept {
    std::lock_guard lock(state.programRetirementMutex);
    return {.registeredPrograms = state.registeredPrograms,
            .pendingPrograms = state.pendingPrograms,
            .closedOwnerPendingPrograms = state.programOwnerClosed ? state.pendingPrograms : 0};
}
} // namespace bloom::render::ocio_program_detail
