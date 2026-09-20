#include "gpu_affine_private.hpp"

#include <atomic>
#include <cstdint>

// Bounded process-global quarantine accounting for AffineBilinearV1. Kept private to gpu_affine so
// an unproved affine teardown is reported through GpuAffine::teardownDrainIncomplete() without
// changing the composite fuse. The pipeline/buffer construction itself reuses the generic
// CompositeBuffer/CompositePipeline helpers from gpu_composite_resources.cpp unchanged.

namespace bloom::render {
namespace {

std::atomic<std::int32_t> g_affineQuarantineCount{0};
std::atomic<bool> g_affineTeardownIncomplete{false};

constexpr std::int32_t kMaxAffineQuarantines = 4;

} // namespace

bool affineQuarantineAllowed() noexcept {
    return g_affineQuarantineCount.load() < kMaxAffineQuarantines;
}

void noteAffineQuarantine() noexcept {
    g_affineQuarantineCount.fetch_add(1);
    g_affineTeardownIncomplete.store(true);
}

bool affineTeardownIncomplete() noexcept { return g_affineTeardownIncomplete.load(); }

} // namespace bloom::render
