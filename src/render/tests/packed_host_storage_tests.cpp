#include <bloom/render/display_buffer.hpp>
#include <bloom/render/packed_host_retirement.hpp>

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <thread>

namespace {
using namespace bloom::render;
std::atomic<std::size_t> watchedSize{0}, matchingAllocations{0};
std::atomic_bool failWatchedAllocation{false};
std::atomic<void*> watchedBlock{nullptr};
GpuProcessingPhaseBudget* deletionBudget = nullptr;
std::atomic_bool freedWhileCharged{false};
int failures = 0;
void expect(const bool value, const char* message) {
    if (!value) {
        ++failures;
        std::cerr << "FAILED: " << message << '\n';
    }
}
std::shared_ptr<GpuProcessingPhaseLease> grant(GpuProcessingPhaseBudget& budget,
                                               const std::uint64_t bytes) {
    auto result = budget.reserve({.fixedHostBytes = bytes, .sourceFree = true, .native = false});
    expect(result.status == GpuProcessingPhaseAdmission::Accepted && result.lease,
           "fixture phase is admitted");
    return std::move(result.lease);
}
template <class Predicate> bool wait(Predicate check) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!check() && std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return check();
}
struct RetirementFixture final {
    std::shared_ptr<PackedHostRetirementOwner> owner = PackedHostRetirementOwner::create();
    ~RetirementFixture() {
        owner->beginShutdown();
        if (!wait([&] { return owner->exitAcknowledged(); }) || !owner->collect()) {
            std::terminate();
        }
    }
};
ReferenceDisplayBufferDescriptor descriptor() {
    const auto window = ImageWindow::create(0, 0, 8, 4);
    const auto result = ReferenceDisplayBufferDescriptor::create(
        *window.value(), bloom::core::PixelAspectRatio::square());
    return *result.value();
}
ImageResult<PreparedReferenceDisplayBuffer> tryAdopt(PackedRgba8Storage& source,
                                                     const std::size_t byteLimit) noexcept {
    // This tentative handoff consumes the source only on success, which the assertions below
    // verify. Passing the fixture owner by reference expresses that conditional mutation.
    return PreparedReferenceDisplayBuffer::adopt(descriptor(), std::move(source), byteLimit);
}
void observeAllocation(const std::size_t payloadBytes) {
    watchedSize = detail::packedHostAllocationMetadataBytes() + payloadBytes;
    matchingAllocations = 0;
    watchedBlock = nullptr;
    freedWhileCharged = false;
}
void clearObservation() {
    watchedSize = 0;
    watchedBlock = nullptr;
    deletionBudget = nullptr;
    failWatchedAllocation = false;
}
void allocatorFailureAndBounds() {
    RetirementFixture retirement;
    GpuProcessingPhaseBudget budget(2, retirement.owner);
    budget.setHostCapacity(512, true);
    auto phase = grant(budget, 512);
    const auto revision = phase->revisionAfterOwnRelease();
    observeAllocation(128);
    failWatchedAllocation = true;
    auto failed = PackedRgba8Storage::create(32, phase, 128);
    expect(!failed && failed.error()->code == ImageErrorCode::AllocationFailure &&
               matchingAllocations == 1,
           "actual pixel allocation failure occurs after a provisional claim");
    const auto folded = budget.snapshot();
    expect(folded.hostReservedBytes == 512 && folded.ownedPackedHostBytes == 0 &&
               folded.totalHostClaimBytes == 512 && folded.admissionRevision + 1 == revision,
           "allocation failure atomically folds bytes back without advancing revision");
    clearObservation();

    auto authority = PackedHostStorageAuthority::create(phase, 128);
    detail::PackedRgba8Allocator allocator(authority);
    using ProxyAllocator = std::allocator_traits<detail::PackedRgba8Allocator>::rebind_alloc<int>;
    static_assert(std::is_same_v<ProxyAllocator::value_type, int>);
    ProxyAllocator proxy(allocator);
    auto* metadata = proxy.allocate(3);
    expect(budget.snapshot().ownedPackedHostBytes == 0 && proxy == allocator,
           "conforming non-pixel rebound metadata is explicitly outside payload accounting");
    proxy.deallocate(metadata, 3);
    auto* first = allocator.allocate(16);
    auto* second = allocator.allocate(16);
    expect(budget.snapshot().ownedPackedHostBytes == 128 &&
               budget.snapshot().ownedPackedHostAllocations == 2 &&
               budget.snapshot().totalHostClaimBytes == 512,
           "simultaneous old/new allocation requests are both charged");
    observeAllocation(4);
    bool refused = false;
    try {
        auto* unexpected = allocator.allocate(1);
        allocator.deallocate(unexpected, 1);
    } catch (const detail::PackedHostCapacityError&) {
        refused = true;
    }
    expect(refused && matchingAllocations == 0 && budget.snapshot().ownedPackedHostBytes == 128,
           "actual allocator request exceeding simultaneous ceiling refuses BEFORE allocation");
    clearObservation();
    allocator.deallocate(first, 16);
    allocator.deallocate(second, 16);
    expect(budget.snapshot().ownedPackedHostBytes == 0 &&
               budget.snapshot().hostReservedBytes == 512,
           "all provisional allocation debits return to the one phase");
    phase.reset();
    expect(budget.snapshot().admittedPhases == 0 && budget.snapshot().totalHostClaimBytes == 0 &&
               budget.snapshot().admissionRevision == revision,
           "empty allocators retain no phase and cleanup preserves exact R+1");
    bool expiredRefused = false;
    try {
        auto* unexpected = allocator.allocate(1);
        allocator.deallocate(unexpected, 1);
    } catch (const detail::PackedHostCapacityError&) {
        expiredRefused = true;
    }
    expect(expiredRefused, "expired origin cannot allocate uncharged storage");
}
void moveAdoptPublishAndCopy() {
    RetirementFixture retirement;
    GpuProcessingPhaseBudget budget(2, retirement.owner);
    budget.setHostCapacity(512, true);
    auto phase = grant(budget, 512);
    const auto revision = phase->revisionAfterOwnRelease();
    std::atomic_bool stop{false}, invalidTotal{false};
    std::thread observer([&] {
        while (!stop.load()) {
            const auto snapshot = budget.snapshot();
            if (snapshot.totalHostClaimBytes != 512 ||
                snapshot.hostReservedBytes + snapshot.ownedPackedHostBytes +
                        snapshot.ownedPackedHostMetadataBytes !=
                    512) {
                invalidTotal = true;
            }
        }
    });
    auto created = PackedRgba8Storage::create(32, phase, 128);
    expect(created.hasValue(), "rooted vector is constructed");
    if (!created) {
        stop = true;
        observer.join();
        return;
    }
    auto storage = std::move(*created.value());
    const auto& sourceAfterHandoff = storage;
    auto* original = storage.mutablePixels().data();
    storage.mutablePixels()[0] = {1, 2, 3, 4};
    expect(storage.byteCost() == storage.capacity() * sizeof(Rgba8) &&
               budget.snapshot().ownedPackedHostBytes == storage.byteCost(),
           "charge is actual vector capacity, not request geometry");
    auto rejected = tryAdopt(storage, 127);
    expect(!rejected && sourceAfterHandoff.pixels().data() == original &&
               sourceAfterHandoff.size() == 32 && budget.snapshot().ownedPackedHostBytes == 128,
           "rejected adoption preserves the entire charged source owner");
    auto adopted = tryAdopt(storage, 128);
    expect(adopted.hasValue() && adopted.value()->pixels().data() == original &&
               sourceAfterHandoff.size() == 0,
           "adoption moves backing and token without copying pixels");
    auto copy =
        PreparedReferenceDisplayBuffer::create(descriptor(), adopted.value()->pixels(), phase, 128);
    expect(copy.hasValue() && copy.value()->pixels().data() != original &&
               copy.value()->pixels()[0] == Rgba8{1, 2, 3, 4} &&
               budget.snapshot().ownedPackedHostBytes == 256 &&
               budget.snapshot().ownedPackedHostAllocations == 2,
           "a true copy owns a second actual allocation under the same phase root");
    stop = true;
    observer.join();
    expect(!invalidTotal, "snapshot observer never sees a gap or double charge through handoff");
    expect(adopted.value()->promotePixelOwnership() && copy.value()->promotePixelOwnership(),
           "complete successful storage authorities promote once");
    expect(adopted.value()->promotePixelOwnership(), "promotion is idempotent");
    phase.reset();
    expect(budget.snapshot().admittedPhases == 0 && budget.snapshot().hostReservedBytes == 0 &&
               budget.snapshot().ownedPackedHostBytes == 256 &&
               budget.snapshot().totalHostClaimBytes ==
                   256 + 2 * detail::packedHostAllocationMetadataBytes() &&
               budget.snapshot().admissionRevision == revision,
           "published host storage survives independently of phase count/device reservation");
    auto competing = budget.reserve({.fixedHostBytes = 257, .sourceFree = true, .native = false});
    expect(competing.status == GpuProcessingPhaseAdmission::BudgetRefused,
           "new phase admission includes retained owned payload in the unified total");
    auto firstAlias =
        std::make_shared<const PreparedReferenceDisplayBuffer>(std::move(*adopted.value()));
    auto externalPin = firstAlias;
    firstAlias.reset();
    expect(budget.snapshot().ownedPackedHostBytes == 256,
           "purging one mailbox/cache alias leaves external pin charged exactly once");
    externalPin.reset();
    expect(wait([&] { return budget.snapshot().ownedPackedHostBytes == 128; }) &&
               budget.snapshot().ownedPackedHostAllocations == 1,
           "only last alias destruction releases the first allocation");
}
void moveAssignmentAndEmptyStorage() {
    RetirementFixture retirement;
    GpuProcessingPhaseBudget budget(1, retirement.owner);
    budget.setHostCapacity(512, true);
    auto phase = grant(budget, 512);
    observeAllocation(64);
    auto destination = PackedRgba8Storage::create(16, phase, 64);
    watchedSize = 0;
    auto source = PackedRgba8Storage::create(16, phase, 64);
    const auto* sourcePixels = source.value()->pixels().data();
    deletionBudget = &budget;
    *destination.value() = std::move(*source.value());
    expect(freedWhileCharged && destination.value()->pixels().data() == sourcePixels &&
               source.value()->size() == 0 && budget.snapshot().ownedPackedHostBytes == 64,
           "move assignment destroys destination backing before release and steals exact source");
    clearObservation();
    destination.value()->reset();
    auto zero = PackedRgba8Storage::create(0, phase, 0);
    expect(zero.hasValue() && zero.value()->size() == 0 &&
               budget.snapshot().ownedPackedHostAllocations == 0,
           "zero storage acquires no allocation token");
    auto overflow = PackedRgba8Storage::create(std::numeric_limits<std::size_t>::max(), phase,
                                               std::numeric_limits<std::size_t>::max());
    expect(!overflow && budget.snapshot().ownedPackedHostAllocations == 0,
           "overflow is refused without allocation or owned-byte claim");
    phase.reset();
    expect(budget.snapshot().admittedPhases == 0 && budget.snapshot().totalHostClaimBytes == 0,
           "empty moved-from and zero allocators never retain phase count");
    expect(zero.value()->promote(), "empty storage promotion remains a zero-byte operation");
    auto builderPhase = grant(budget, 512);
    auto builder = ReferenceDisplayBufferBuilder::create(descriptor(), builderPhase, 128);
    expect(builder.hasValue(), "rooted builder allocates once under actual byte authority");
    auto frozen = std::move(*builder.value()).freeze();
    expect(frozen.hasValue() &&
               frozen.value()->pixelOwnership() == PackedRgba8Ownership::RootedProvisional &&
               budget.snapshot().ownedPackedHostBytes == frozen.value()->pixelStorageByteCost(),
           "builder freeze transfers the complete provisional allocation without a new charge");
    builderPhase->markUnproven();
    expect(!frozen.value()->promotePixelOwnership(),
           "host storage promotion never authorizes an unproven phase retirement");
}
void publicationLifetimeAndOrdering() {
    RetirementFixture retirement;
    auto budget = std::make_unique<GpuProcessingPhaseBudget>(1, retirement.owner);
    budget->setHostCapacity(256, true);
    auto phase = grant(*budget, 256);
    observeAllocation(128);
    auto storage = PackedRgba8Storage::create(32, phase, 128);
    expect(storage.hasValue(), "lifetime vector allocation reaches actual allocator");
    if (!storage) {
        clearObservation();
        return;
    }
    expect(storage.value()->promote(), "lifetime storage promotes");
    phase.reset();
    deletionBudget = budget.get();
    storage.value()->reset();
    expect(wait([&] { return budget->snapshot().ownedPackedHostBytes == 0; }) &&
               freedWhileCharged && budget->snapshot().totalHostClaimBytes == 0,
           "backing allocation is freed before its published token releases bytes");
    clearObservation();
    phase = grant(*budget, 256);
    auto retained = PackedRgba8Storage::create(32, phase, 128);
    expect(retained.hasValue() && retained.value()->promote(), "retained storage promotes");
    phase.reset();
    budget.reset();
    expect(retained.value()->size() == 32 &&
               retained.value()->ownership() == PackedRgba8Ownership::RootedPublished,
           "neutral allocation owner survives root shutdown with immutable pixels intact");
    retained.value()->reset();
}
void capacityFailureWatermark() {
    for (const int releaseOrder : {0, 1, 2}) {
        RetirementFixture retirement;
        GpuProcessingPhaseBudget budget(2, retirement.owner);
        budget.setHostCapacity(512, true);
        auto phase = grant(budget, 256);
        auto competitor = grant(budget, 256);
        const auto watermark = phase->revisionAfterOwnRelease();
        auto storage = PackedRgba8Storage::create(32, phase, 128);
        expect(storage.hasValue(), "capacity failure follows actual provisional pixel allocation");
        const auto refusal =
            budget.reserve({.fixedHostBytes = 1, .sourceFree = true, .native = false});
        expect(refusal.status == GpuProcessingPhaseAdmission::BudgetRefused,
               "a genuine typed capacity refusal occurs after provisional allocation");
        phase.reset();
        expect(budget.snapshot().admittedPhases == 2,
               "provisional allocation keeps its exact origin phase alive for failed cleanup");
        if (releaseOrder == 1) {
            competitor.reset();
        }
        storage.value()->reset();
        expect(budget.snapshot().admissionRevision == watermark + (releaseOrder == 1 ? 1U : 0U),
               "provisional teardown folds back before one final own phase release");
        if (releaseOrder == 2) {
            competitor.reset();
        }
        expect((budget.snapshot().admissionRevision > watermark) == (releaseOrder != 0),
               "own cleanup cannot retry while interleaved independent release remains visible");
    }
}
} // namespace

void* operator new(const std::size_t bytes) {
    const bool watched = bytes != 0 && bytes == watchedSize.load();
    if (watched) {
        ++matchingAllocations;
        if (failWatchedAllocation.exchange(false)) {
            throw std::bad_alloc{};
        }
    }
    void* const block = std::malloc(bytes == 0 ? 1 : bytes);
    if (!block) {
        throw std::bad_alloc{};
    }
    if (watched) {
        watchedBlock = block;
    }
    return block;
}
void operator delete(void* const block) noexcept {
    const bool watched = block && block == watchedBlock.load();
    std::free(block);
    if (watched && deletionBudget) {
        const auto snapshot = deletionBudget->snapshot();
        freedWhileCharged = snapshot.ownedPackedHostBytes != 0;
    }
}
void operator delete(void* const block, std::size_t) noexcept { ::operator delete(block); }

int main() {
    allocatorFailureAndBounds();
    moveAdoptPublishAndCopy();
    moveAssignmentAndEmptyStorage();
    publicationLifetimeAndOrdering();
    capacityFailureWatermark();
    std::cout << "packed-host core checks failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
