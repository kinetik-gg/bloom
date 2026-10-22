#include <bloom/render/final_pixel_storage.hpp>
#include <bloom/render/image.hpp>
#include <packed_host_retirement_private.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace bloom::render::detail {
struct PackedHostRetirementTestAccess final {
    static void holdFree(PackedHostRetirementOwner& owner, void (*callback)(void*) noexcept,
                         void* context) {
        owner.impl_->state->beforeFree = callback;
        owner.impl_->state->testContext = context;
    }
};
} // namespace bloom::render::detail
namespace {
using namespace bloom::render;
std::atomic_size_t watchedSize{0}, actualAttempts{0};
std::atomic<void*> watchedBlock{nullptr};
GpuProcessingPhaseBudget* observedBudget = nullptr;
std::atomic_bool freedWhileCharged{false}, failWatched{false};
void require(bool ok, const char* message) {
    if (!ok)
        throw std::runtime_error(message);
}
template <class Predicate> bool wait(Predicate check) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!check() && std::chrono::steady_clock::now() < until)
        std::this_thread::yield();
    return check();
}
struct Owner final {
    std::shared_ptr<PackedHostRetirementOwner> owner = PackedHostRetirementOwner::create();
    ~Owner() {
        owner->beginShutdown();
        if (!wait([&] { return owner->exitAcknowledged(); }) || !owner->collect())
            std::terminate();
    }
};
struct Gate final {
    std::atomic_bool entered{false}, release{false};
    ~Gate() { release = true; }
    static void hold(void* context) noexcept {
        auto& gate = *static_cast<Gate*>(context);
        gate.entered = true;
        while (!gate.release.load())
            std::this_thread::yield();
    }
};
std::shared_ptr<GpuProcessingPhaseLease> admit(GpuProcessingPhaseBudget& budget,
                                               std::uint64_t bytes) {
    auto grant = budget.reserve({.fixedHostBytes = bytes, .sourceFree = true, .native = false});
    require(bool(grant.lease), "real final host grant is required");
    return std::move(grant.lease);
}
void boundsAndOverlap() {
    Owner owner;
    require(owner.owner->prepare(), "existing host worker prepares off the dropping thread");
    GpuProcessingPhaseBudget budget(1, owner.owner);
    budget.setHostCapacity(4096, true);
    const auto header = detail::packedHostAllocationMetadataBytes();
    auto phase = admit(budget, 128 + header - 1);
    watchedSize = 128 + header;
    actualAttempts = 0;
    auto refused = FinalRgba32fStorage::create(8, phase, 128, HostPixelStorageDomain::FinalProcess);
    require(!refused && actualAttempts == 0 && budget.snapshot().ownedFinalHostAllocations == 0,
            "one byte short including exact header refuses before backing allocation");
    watchedSize = 0;
    phase.reset();
    phase = admit(budget, 4096);
    watchedSize = 128 + header;
    actualAttempts = 0;
    failWatched = true;
    const auto failed =
        FinalRgba32fStorage::create(8, phase, 128, HostPixelStorageDomain::FinalProcess);
    require(!failed && failed.error()->code == ImageErrorCode::AllocationFailure &&
                actualAttempts == 1 && budget.snapshot().hostReservedBytes == 4096 &&
                budget.snapshot().ownedFinalHostAllocations == 0,
            "actual typed backing failure folds payload and header before returning");
    watchedSize = 0;
    failWatched = false;
    const auto ownRevision = phase->revisionAfterOwnRelease();
    auto process = FinalRgba32fStorage::create(8, phase, 128, HostPixelStorageDomain::FinalProcess);
    auto builder = FinalRgba32fStorage::create(8, phase, 128, HostPixelStorageDomain::FinalProcess);
    auto encoded = FinalRgba8Storage::create(8, phase, 32, HostPixelStorageDomain::FinalEncoded);
    require(process && builder && encoded, "three distinct final overlap allocations exist");
    require(budget.rootedHostBackingSnapshot().materializedBytes == 128 + 128 + 32 + 3 * header,
            "all completed final process and encoded vectors are eligible exact backing");
    const auto overlap = budget.snapshot();
    require(overlap.ownedFinalProcessHostBytes ==
                    process.value()->byteCost() + builder.value()->byteCost() &&
                overlap.ownedFinalEncodedHostBytes == encoded.value()->byteCost() &&
                overlap.ownedFinalHostMetadataBytes == 3 * header &&
                overlap.ownedFinalHostAllocations == 3 &&
                overlap.hostReservedBytes + overlap.ownedFinalProcessHostBytes +
                        overlap.ownedFinalEncodedHostBytes + overlap.ownedFinalHostMetadataBytes ==
                    4096 &&
                overlap.ownedPackedHostBytes == 0,
            "process copy plus builder and encoded remain separate under one unchanged total");
    auto* backing = builder.value()->data();
    FinalRgba32fStorage moved = std::move(*builder.value());
    require(moved.data() == backing && builder.value()->empty() &&
                budget.snapshot().ownedFinalHostAllocations == 3,
            "typed move keeps exact backing/domain/root without adding claims");
    phase.reset();
    process.value()->reset();
    encoded.value()->reset();
    moved.reset();
    require(budget.snapshot().totalHostClaimBytes == 0 &&
                budget.rootedHostBackingSnapshot().materializedBytes == 0 &&
                budget.snapshot().admissionRevision == ownRevision,
            "provisional overlap cleanup folds all debits before sole phase release");
}
void rootedImageFreezeOverlap() {
    Owner owner;
    require(owner.owner->prepare(), "image host owner prepares");
    GpuProcessingPhaseBudget budget(1, owner.owner);
    budget.setHostCapacity(4096, true);
    auto phase = admit(budget, 4096);
    const auto window = ImageWindow::create(-2, 3, 4, 2);
    require(bool(window), "image ROI is valid");
    const auto descriptor = Rgba32fImageDescriptor::create(*window.value(), *window.value(),
                                                           bloom::core::PixelAspectRatio::square());
    require(bool(descriptor), "image descriptor is valid");
    auto readback =
        FinalRgba32fStorage::create(8, phase, 128, HostPixelStorageDomain::FinalProcess);
    auto builder = Rgba32fImageBuilder::create(*descriptor.value(), 128, phase);
    auto encoded = FinalRgba8Storage::create(8, phase, 32, HostPixelStorageDomain::FinalEncoded);
    require(readback && builder && encoded, "real image copy and encoded overlap is admitted");
    const auto clear = Rgba32f::transparent();
    for (std::int64_t y = 3; y < 5; ++y) {
        auto row = builder.value()->row(y);
        require(bool(row), "ROI builder row exists");
        std::ranges::copy(
            readback.value()->pixels().subspan(static_cast<std::size_t>(y - 3) * 4, 4),
            row.value()->begin());
    }
    const auto before = budget.snapshot();
    require(before.ownedFinalProcessHostBytes == 256 && before.ownedFinalEncodedHostBytes == 32 &&
                before.ownedFinalHostAllocations == 3,
            "builder and process are distinct concurrent backings");
    const auto* backing = builder.value()->pixels().data();
    auto image = std::move(*builder.value()).freeze();
    require(bool(image), "image freezes successfully");
    const auto pixel = image.value()->read(-2, 3);
    require(image.value()->pixels().data() == backing && !builder.value()->isValid() &&
                budget.snapshot().ownedFinalHostAllocations == 3 && pixel &&
                *pixel.value() == clear,
            "freeze retains same backing and unchanged ROI/pixels while copy remains live");
    readback.value()->reset();
    require(PackedHostStorageAuthority::promoteFinalProduct(image.value()->storageAuthority(),
                                                            encoded.value()->authority()),
            "complete frozen image plus encoded publishes atomically");
    const std::weak_ptr<GpuProcessingPhaseLease> old = phase;
    phase.reset();
    require(old.expired() && budget.snapshot().admittedPhases == 0,
            "published image storage does not pin originating phase");
}

void atomicPublicationAndWorkerLifetime() {
    Owner owner;
    Gate gate;
    detail::PackedHostRetirementTestAccess::holdFree(*owner.owner, &Gate::hold, &gate);
    GpuProcessingPhaseBudget budget(2, owner.owner);
    struct ClearObservation final {
        PackedHostRetirementOwner& owner;
        Gate& gate;
        ~ClearObservation() {
            // Later-declared payload aliases have already died. Drain while the observed budget
            // still exists, including failures after the free gate was released.
            gate.release = true;
            owner.beginShutdown();
            if (!wait([&] { return owner.exitAcknowledged(); }))
                std::terminate();
            observedBudget = nullptr;
            watchedBlock = nullptr;
            watchedSize = 0;
        }
    } clearObservation{*owner.owner, gate};
    budget.setHostCapacity(4096, true);
    auto phase = admit(budget, 2048);
    watchedSize = 128 + detail::packedHostAllocationMetadataBytes();
    watchedBlock = nullptr;
    freedWhileCharged = false;
    observedBudget = &budget;
    auto process = FinalRgba32fStorage::create(8, phase, 128, HostPixelStorageDomain::FinalProcess);
    auto encoded = FinalRgba8Storage::create(8, phase, 32, HostPixelStorageDomain::FinalEncoded);
    require(process && encoded, "final product allocations exist");
    auto foreignPhase = admit(budget, 2048);
    // A same-root different attempt cannot be mixed into a final product either.
    auto foreignStorage =
        FinalRgba8Storage::create(8, foreignPhase, 32, HostPixelStorageDomain::FinalEncoded);
    require(bool(foreignStorage), "different attempt owns actual encoded backing");
    auto foreignAuthority = foreignStorage.value()->authority();
    require(!PackedHostStorageAuthority::promoteFinalProduct(process.value()->authority(),
                                                             foreignAuthority) &&
                !process.value()->authority()->snapshot().published,
            "failed complete promotion changes neither final product authority");
    foreignStorage.value()->reset();
    foreignPhase.reset();
    require(PackedHostStorageAuthority::promoteFinalProduct(process.value()->authority(),
                                                            encoded.value()->authority()),
            "complete process and encoded promotion is one atomic handoff");
    phase.reset();
    require(PackedHostStorageAuthority::promoteFinalProduct(process.value()->authority(),
                                                            encoded.value()->authority()),
            "same exact original product promotion remains idempotent after phase detach");
    const auto published = budget.snapshot();
    require(published.admittedPhases == 0 && published.hostReservedBytes == 0 &&
                published.ownedFinalHostAllocations == 2 &&
                published.totalHostClaimBytes ==
                    160 + 2 * detail::packedHostAllocationMetadataBytes(),
            "published host products retain exact payload/header without strong phase");
    const auto credited = published.totalHostClaimBytes;
    budget.setSampledHostCapacity(4096, true, std::chrono::steady_clock::time_point::max(),
                                  credited,
                                  budget.rootedHostBackingSnapshot().retiredMaterializedBytes);
    auto output = std::make_shared<const FinalRgba32fStorage>(std::move(*process.value()));
    std::vector<std::shared_ptr<const FinalRgba32fStorage>> aliases(256, output);
    output.reset();
    owner.owner->beginShutdown();
    require(!owner.owner->exitAcknowledged() && budget.snapshot().ownedFinalHostAllocations == 2,
            "root close cannot exit while external output aliases remain");
    watchedSize = 0;
    std::thread dropping([owned = std::move(aliases)]() mutable { owned.clear(); });
    dropping.join();
    require(wait([&] { return gate.entered.load(); }) &&
                budget.snapshot().ownedFinalProcessHostBytes == 128 &&
                budget.snapshot().hostCapacityBytes == 4096 &&
                owner.owner->snapshot().allocations == 2 && !owner.owner->exitAcknowledged(),
            "queued final backing retains its credit and charge until actual free begins");
    encoded.value()->reset();
    gate.release = true;
    require(wait([&] { return owner.owner->exitAcknowledged(); }) &&
                budget.snapshot().ownedFinalHostAllocations == 0 &&
                budget.snapshot().totalHostClaimBytes == 0 &&
                budget.snapshot().hostCapacityBytes == 4096 - credited &&
                freedWhileCharged.load(),
            "same host worker revokes both credits and frees backing before claim release");
    observedBudget = nullptr;
    watchedBlock = nullptr;
}
void publicationIdentityAndUnproven() {
    Owner owner;
    GpuProcessingPhaseBudget budget(2, owner.owner);
    budget.setHostCapacity(4096, true);
    auto phase = admit(budget, 2048);
    auto process = FinalRgba32fStorage::create(8, phase, 128, HostPixelStorageDomain::FinalProcess);
    require(process && process.value()->authority()->promote(), "first exact origin publishes");
    phase.reset();
    phase = admit(budget, 2048);
    auto encoded = FinalRgba8Storage::create(8, phase, 32, HostPixelStorageDomain::FinalEncoded);
    require(encoded && encoded.value()->authority()->promote(), "second exact origin publishes");
    phase.reset();
    require(budget.snapshot().admittedPhases == 0 &&
                !PackedHostStorageAuthority::promoteFinalProduct(process.value()->authority(),
                                                                 encoded.value()->authority()),
            "two published same-root different-phase allocations cannot become one product");
    process.value()->reset();
    encoded.value()->reset();
    require(wait([&] { return budget.snapshot().ownedFinalHostAllocations == 0; }),
            "independent published allocations actually retire");
    phase = admit(budget, 2048);
    process = FinalRgba32fStorage::create(8, phase, 128, HostPixelStorageDomain::FinalProcess);
    encoded = FinalRgba8Storage::create(8, phase, 32, HostPixelStorageDomain::FinalEncoded);
    require(process && encoded, "unproven control has both real allocations");
    const auto revision = phase->revisionAfterOwnRelease();
    phase->markUnproven();
    require(!PackedHostStorageAuthority::promoteFinalProduct(process.value()->authority(),
                                                             encoded.value()->authority()) &&
                !process.value()->authority()->snapshot().published &&
                !encoded.value()->authority()->snapshot().published,
            "atomic final publication refuses an unproven origin without publishing a prefix");
    phase.reset();
    process.value()->reset();
    encoded.value()->reset();
    require(budget.snapshot().admittedPhases == 0 && budget.snapshot().totalHostClaimBytes == 0 &&
                budget.snapshot().admissionRevision == revision,
            "host-only provisional disposal preserves exact own release and no phase pin");
}

} // namespace
void* operator new(std::size_t bytes) {
    if (bytes && bytes == watchedSize.load()) {
        ++actualAttempts;
        if (failWatched.exchange(false))
            throw std::bad_alloc{};
    }
    auto* block = std::malloc(bytes ? bytes : 1);
    if (!block)
        throw std::bad_alloc{};
    if (bytes && bytes == watchedSize.load())
        watchedBlock = block;
    return block;
}
void operator delete(void* block) noexcept {
    const bool watched = block && block == watchedBlock.load();
    std::free(block);
    if (watched && observedBudget)
        freedWhileCharged = observedBudget->snapshot().ownedFinalProcessHostBytes != 0;
}
void operator delete(void* block, std::size_t) noexcept { ::operator delete(block); }
int main() {
    try {
        boundsAndOverlap();
        rootedImageFreezeOverlap();
        atomicPublicationAndWorkerLifetime();
        publicationIdentityAndUnproven();
        std::cout << "PASS: neutral final pixel storage\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
