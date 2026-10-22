#include <bloom/render/native_buffer_storage.hpp>
#include <bloom/render/native_image_storage.hpp>

#include <chrono>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using namespace bloom::render;
using Clock = std::chrono::steady_clock;
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
NativeBufferStorageToken& token(NativeBufferStorageGrant& grant) {
    if (!grant.token)
        throw std::runtime_error("expected an admitted buffer token");
    return *grant.token;
}
NativeBufferStorageIdentity local() {
    return {.allocatorGeneration = 7, .memoryType = 2, .heap = 1};
}
NativeBufferStorageIdentity nonlocal() {
    return {.allocatorGeneration = 7, .memoryType = 3, .heap = 0, .deviceLocal = false};
}
GpuProcessingPhaseRequest request(std::uint64_t host = 80, std::uint64_t device = 100) {
    return {.fixedHostBytes = host, .deviceWorkingBytes = device, .sourceFree = true};
}
void empty(const GpuProcessingPhaseBudget& budget) {
    const auto s = budget.snapshot();
    require(s.hostReservedBytes == 0 && s.deviceReservedBytes == 0 && s.totalHostClaimBytes == 0 &&
                s.totalDeviceClaimBytes == 0 && s.ownedNativeBufferBytes == 0 &&
                s.ownedNonlocalNativeBufferHostBytes == 0 && s.ownedNativeBuffers == 0 &&
                s.admittedPhases == 0 && s.unprovenPhases == 0,
            "buffer and phase claims return to zero");
}
void pairedAdmissionAndReconcile() {
    GpuProcessingPhaseBudget budget;
    budget.setHostCapacity(1000, true);
    budget.setDeviceCapacity(1000, true);
    auto phase = budget.reserve(request());
    require(phase.lease != nullptr, "paired phase admitted");
    auto device = NativeBufferStorageToken::reserve(phase.lease, 40, local());
    auto host = NativeBufferStorageToken::reserve(phase.lease, 60, nonlocal());
    require(device.token && host.token, "both actual heap domains transfer existing debit");
    const auto s = budget.snapshot();
    require(s.ownedNativeBufferBytes == 100 && s.ownedNativeBuffers == 2 &&
                s.ownedNonlocalNativeBufferHostBytes == 60 && s.hostReservedBytes == 20 &&
                s.deviceReservedBytes == 0 && s.totalHostClaimBytes == 80 &&
                s.totalDeviceClaimBytes == 100 && s.ownedNativeImages == 0,
            "paired handoff preserves both totals and separate image counts");
    auto denied = NativeBufferStorageToken::reserve(phase.lease, 1, nonlocal());
    require(!denied.token && denied.status == GpuProcessingPhaseAdmission::BudgetRefused &&
                denied.domain == GpuProcessingPhaseDomain::Device &&
                budget.snapshot().hostReservedBytes == s.hostReservedBytes,
            "one-byte native refusal never admits the host half");
    for (unsigned field = 0; field != 4; ++field) {
        auto wrong = nonlocal();
        if (field == 0)
            ++wrong.allocatorGeneration;
        else if (field == 1)
            ++wrong.memoryType;
        else if (field == 2)
            ++wrong.heap;
        else
            wrong.deviceLocal = true;
        require(!token(host).reconcile(50, wrong) && token(host).bytes() == 60,
                "each wrong identity field independently rejects unchanged");
    }
    require(!token(host).reconcile(61, nonlocal()) && !token(host).reconcile(0, nonlocal()) &&
                token(host).reconcile(50, nonlocal()),
            "reconciliation only shrinks a positive exact allocation");
    const auto reconciled = budget.snapshot();
    require(reconciled.deviceReservedBytes == 10 && reconciled.hostReservedBytes == 30 &&
                reconciled.ownedNativeBufferBytes == 90 &&
                reconciled.ownedNonlocalNativeBufferHostBytes == 50 &&
                reconciled.totalDeviceClaimBytes == 100 && reconciled.totalHostClaimBytes == 80 &&
                phase.lease->hostBytes() == 80 && phase.lease->deviceBytes() == 100,
            "downward reconciliation restores both remaining debits, never immutable limits");
    auto moved = std::move(token(host));
    host.token.reset();
    require(budget.snapshot().ownedNativeBuffers == 2,
            "move and moved-from destruction preserve one token per suballocation");
    const auto watermark = denied.revisionAfterOwnRelease;
    // Neutral controls have no native backing; real backing-before-reset proof belongs to adoption.
    moved = {};
    device.token.reset();
    require(budget.snapshot().admissionRevision + 1 == watermark &&
                budget.snapshot().hostReservedBytes == 80 &&
                budget.snapshot().deviceReservedBytes == 100,
            "provisional foldback is quiet and restores the whole admitted envelope");
    phase.lease.reset();
    require(budget.snapshot().admissionRevision == watermark,
            "only actual final phase release advances own R+1");
    empty(budget);

    phase = budget.reserve(request(5, 10));
    require(phase.lease != nullptr, "small host grant admitted");
    const auto before = budget.snapshot();
    denied = NativeBufferStorageToken::reserve(phase.lease, 6, nonlocal());
    require(!denied.token && denied.domain == GpuProcessingPhaseDomain::Host &&
                budget.snapshot().deviceReservedBytes == before.deviceReservedBytes &&
                budget.snapshot().ownedNativeBufferBytes == 0,
            "one-byte host refusal never consumes native debit or adds supplemental capacity");
    phase.lease.reset();
    empty(budget);
}
void grandfatheredConversionAndLifetime() {
    GpuProcessingPhaseBudget budget;
    const auto now = Clock::now();
    budget.setHostCapacity(1000, true, now + std::chrono::seconds(10));
    budget.setDeviceCapacity(1000, true);
    auto phase = budget.reserve(request());
    require(phase.lease != nullptr, "original grant admitted from a fresh delivery");
    budget.setHostCapacity(0, false, now - std::chrono::seconds(1));
    budget.setDeviceCapacity(0, false);
    auto admitted = NativeBufferStorageToken::reserve(phase.lease, 40, nonlocal());
    require(admitted.token && budget.snapshot().totalHostClaimBytes == 80 &&
                budget.snapshot().totalDeviceClaimBytes == 100,
            "already reserved paired conversion survives staleness and quota shrink");
    auto additive = NativeImageStorageToken::reserve(
        phase.lease, 1,
        {.allocatorGeneration = 7, .memoryType = 3, .heap = 0, .deviceLocal = false},
        now + std::chrono::seconds(11));
    require(!additive.token && additive.status == GpuProcessingPhaseAdmission::AwaitingCapacity,
            "a genuinely additive image host claim still requires fresh readiness");
    struct Holder final {
        NativeBufferStorageToken token;
    };
    auto holder = std::make_shared<Holder>();
    holder->token = std::move(token(admitted));
    admitted.token.reset();
    std::vector<std::shared_ptr<Holder>> aliases(256, holder);
    auto weak = std::weak_ptr<GpuProcessingPhaseLease>(phase.lease);
    phase.lease->markUnproven();
    require(!NativeBufferStorageToken::reserve(phase.lease, 1, local()).token,
            "marked-unproven authority cannot create another buffer");
    phase.lease.reset();
    holder.reset();
    require(!weak.expired() && budget.snapshot().ownedNativeBuffers == 1 &&
                budget.snapshot().ownedNativeBufferBytes == 40 &&
                budget.snapshot().ownedNonlocalNativeBufferHostBytes == 40 &&
                budget.snapshot().unprovenPhases == 1,
            "aliases retain one transient token and exact sticky attempt phase");
    std::thread release([held = std::move(aliases)]() mutable { held.clear(); });
    release.join();
    require(weak.expired(), "last token drops the phase only after leaving the neutral mutex");
    empty(budget);
}
void maximumConservation() {
    GpuProcessingPhaseBudget budget;
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    budget.setHostCapacity(maximum, true);
    budget.setDeviceCapacity(maximum, true);
    auto phase = budget.reserve(request(maximum, maximum));
    require(phase.lease != nullptr, "maximum finite paired grant is representable");
    auto admitted = NativeBufferStorageToken::reserve(phase.lease, maximum, nonlocal());
    require(admitted.token && budget.snapshot().totalHostClaimBytes == maximum &&
                budget.snapshot().totalDeviceClaimBytes == maximum,
            "transfer additions cannot overflow or double total claims");
    require(!NativeBufferStorageToken::reserve(phase.lease, 1, nonlocal()).token,
            "exhausted remaining debit rejects before addition");
    admitted.token.reset();
    phase.lease.reset();
    empty(budget);
}
GpuProcessingPhaseRequest mediaRequest() {
    return {.fixedHostBytes = 100,
            .requestedSourceBytes = 100,
            .minimumSourceBytes = 100,
            .deviceWorkingBytes = 300};
}
void protectedUploadPartition() {
    GpuProcessingPhaseBudget budget;
    budget.setHostCapacity(1000, true);
    budget.setDeviceCapacity(1000, true);
    auto phase = budget.reserve(mediaRequest());
    require(phase.lease && phase.lease->hostBytes() == 400 &&
                phase.lease->sourceByteLimit() == 100 && phase.lease->uploadByteLimit() == 100,
            "unchanged F+3L grants protected source, scratch and upload estimates");
    auto output =
        PackedHostStorageAuthority::create(phase.lease, 400, HostPixelStorageDomain::FinalProcess);
    require(output && !output->reserve(100, 1),
            "output payload plus header cannot borrow protected S/C/U");
    auto outputDenied = NativeBufferStorageToken::reserve(phase.lease, 101, nonlocal());
    require(!outputDenied.token && outputDenied.domain == GpuProcessingPhaseDomain::Host,
            "nonlocal output buffer cannot borrow source/upload estimates");
    auto outputBuffer = NativeBufferStorageToken::reserve(phase.lease, 80, nonlocal());
    auto outputPayload = output->reserve(20);
    require(outputBuffer.token && outputPayload && !output->reserve(1),
            "ordinary output backing and payload share only F");
    auto first = NativeBufferStorageToken::reserveUpload(phase.lease, 70, local());
    require(first.token && budget.snapshot().hostReservedBytes == 300,
            "local or BAR upload occupies U while conservative host reservation remains");
    const auto held = budget.snapshot();
    auto duplicate = NativeBufferStorageToken::reserveUpload(phase.lease, 31, local());
    require(!duplicate.token && duplicate.status == GpuProcessingPhaseAdmission::BudgetRefused &&
                duplicate.domain == GpuProcessingPhaseDomain::Host &&
                budget.snapshot().ownedNativeBufferBytes == held.ownedNativeBufferBytes &&
                budget.snapshot().hostReservedBytes == held.hostReservedBytes &&
                budget.snapshot().deviceReservedBytes == held.deviceReservedBytes,
            "simultaneous local buffers cannot double-spend occupied U, refusal is atomic");
    auto second = NativeBufferStorageToken::reserveUpload(phase.lease, 30, nonlocal());
    require(second.token && budget.snapshot().ownedNonlocalNativeBufferHostBytes == 110 &&
                budget.snapshot().hostReservedBytes == 270 &&
                budget.snapshot().totalHostClaimBytes == 400 &&
                budget.snapshot().totalDeviceClaimBytes == 300,
            "nonlocal upload transfers same U estimate once beside source/scratch/output");
    require(token(first).reconcile(50, local()), "local reconciliation restores occupied U");
    auto reused = NativeBufferStorageToken::reserveUpload(phase.lease, 20, local());
    require(reused.token && !NativeBufferStorageToken::reserveUpload(phase.lease, 1, local()).token,
            "only actual downward reconciliation restores the exact U difference");
    auto wrong = nonlocal();
    ++wrong.heap;
    require(!token(second).reconcile(20, wrong) && token(second).reconcile(20, nonlocal()),
            "nonlocal downward reconcile restores both logical U and its paired host debit");
    auto final = NativeBufferStorageToken::reserveUpload(phase.lease, 10, nonlocal());
    require(final.token && budget.snapshot().totalHostClaimBytes == 400,
            "reconciled paired upload debit can be reused without a new grant");
    const auto revision = budget.snapshot().admissionRevision;
    outputPayload.reset();
    outputBuffer.token.reset();
    first.token.reset();
    second.token.reset();
    reused.token.reset();
    final.token.reset();
    require(budget.snapshot().hostReservedBytes == 400 &&
                budget.snapshot().deviceReservedBytes == 300 &&
                budget.snapshot().admissionRevision == revision,
            "proven storage disposal quietly restores exact partitions, no fake retry revision");
    auto wholeU = NativeBufferStorageToken::reserveUpload(phase.lease, 100, local());
    require(wholeU.token.has_value(), "all U is restored after every owned buffer is destroyed");
    wholeU.token.reset();
    phase.lease.reset();
    empty(budget);
}
void maximumMediaPartition() {
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    constexpr std::uint64_t fixed = 7;
    constexpr auto sourceRoom = (maximum - fixed) / 3;
    GpuProcessingPhaseBudget budget;
    budget.setHostCapacity(maximum, true);
    budget.setDeviceCapacity(100, true);
    GpuProcessingPhaseRequest media{
        .fixedHostBytes = fixed,
        .requestedSourceBytes = maximum,
        .minimumSourceBytes = sourceRoom + 1,
        .deviceWorkingBytes = 1,
    };
    auto overflow = budget.reserve(media);
    require(!overflow.lease && overflow.status == GpuProcessingPhaseAdmission::BudgetRefused &&
                overflow.domain == GpuProcessingPhaseDomain::Host,
            "unrepresentable minimum triple-source overlap refuses before any claim");
    empty(budget);
    media.minimumSourceBytes = sourceRoom;
    auto boundary = budget.reserve(media);
    require(boundary.lease && boundary.lease->sourceByteLimit() == sourceRoom &&
                boundary.lease->uploadByteLimit() == sourceRoom &&
                boundary.lease->hostBytes() == fixed + 3 * sourceRoom &&
                budget.snapshot().totalHostClaimBytes == fixed + 3 * sourceRoom,
            "exact UINT64 boundary F+3L is representable without wrapping");
    auto output = PackedHostStorageAuthority::create(boundary.lease, maximum);
    require(output && !output->reserve(fixed + 1),
            "even maximum source estimate cannot become spare fixed output credit");
    boundary.lease.reset();
    empty(budget);
}
void movedUploadPartition() {
    GpuProcessingPhaseBudget uploadRoot;
    GpuProcessingPhaseBudget outputRoot;
    for (auto* root : {&uploadRoot, &outputRoot}) {
        root->setHostCapacity(1000, true);
        root->setDeviceCapacity(1000, true);
    }
    auto uploadPhase = uploadRoot.reserve(mediaRequest());
    auto outputPhase = outputRoot.reserve(request());
    require(uploadPhase.lease && outputPhase.lease, "independent move roots admitted");
    auto incoming = NativeBufferStorageToken::reserveUpload(uploadPhase.lease, 100, local());
    auto destination = NativeBufferStorageToken::reserve(outputPhase.lease, 40, nonlocal());
    require(incoming.token && destination.token, "upload and ordinary move operands admitted");
    auto constructed = NativeBufferStorageToken(std::move(token(incoming)));
    incoming.token.reset();
    require(!NativeBufferStorageToken::reserveUpload(uploadPhase.lease, 1, local()).token,
            "move construction retains the original occupied U");
    const auto uploadRevision = uploadRoot.snapshot().admissionRevision;
    const auto outputRevision = outputRoot.snapshot().admissionRevision;
    token(destination) = std::move(constructed);
    require(outputRoot.snapshot().hostReservedBytes == 80 &&
                outputRoot.snapshot().deviceReservedBytes == 100 &&
                outputRoot.snapshot().ownedNativeBuffers == 0 &&
                outputRoot.snapshot().admissionRevision == outputRevision &&
                !NativeBufferStorageToken::reserveUpload(outputPhase.lease, 1, local()).token,
            "nonempty assignment returns only the displaced ordinary root debit, never U");
    outputPhase.lease.reset();
    empty(outputRoot);
    require(outputRoot.snapshot().admissionRevision == outputRevision + 1 &&
                uploadRoot.snapshot().ownedNativeBufferBytes == 100,
            "displaced root releases once while incoming upload retains its exact root");
    destination.token.reset();
    require(uploadRoot.snapshot().admissionRevision == uploadRevision &&
                uploadRoot.snapshot().hostReservedBytes == 400 &&
                uploadRoot.snapshot().deviceReservedBytes == 300,
            "moved upload disposal restores exact debit quietly");
    auto reusable = NativeBufferStorageToken::reserveUpload(uploadPhase.lease, 100, nonlocal());
    require(reusable.token && uploadRoot.snapshot().ownedNonlocalNativeBufferHostBytes == 100,
            "partition tag survives both moves and restores whole U only to its origin");
    reusable.token.reset();
    uploadPhase.lease.reset();
    empty(uploadRoot);
}
void grandfatheredProtectedUpload() {
    GpuProcessingPhaseBudget budget;
    budget.setHostCapacity(1000, true);
    budget.setDeviceCapacity(1000, true);
    auto phase = budget.reserve(mediaRequest());
    require(phase.lease != nullptr, "media phase admitted before readiness/shrink transition");
    budget.setHostCapacity(1, false, Clock::now() - std::chrono::seconds(1));
    budget.setDeviceCapacity(1, false);
    const auto revision = budget.snapshot().admissionRevision;
    auto upload = NativeBufferStorageToken::reserveUpload(phase.lease, 100, nonlocal());
    require(upload.token && budget.snapshot().totalHostClaimBytes == 400 &&
                budget.snapshot().totalDeviceClaimBytes == 300,
            "conversion of already admitted U is grandfathered under staleness/shrink");
    phase.lease.reset();
    require(budget.snapshot().admittedPhases == 1,
            "pending buffer retains exact phase and protected estimates until actual disposal");
    upload.token.reset();
    empty(budget);
    require(budget.snapshot().admissionRevision == revision + 1,
            "last upload disposal returns only the exact final phase revision");
    budget.setHostCapacity(1000, true);
    budget.setDeviceCapacity(1000, true);
    auto sourceFree = budget.reserve(request(100, 300));
    require(sourceFree.lease && sourceFree.lease->uploadByteLimit() == 0 &&
                !NativeBufferStorageToken::reserveUpload(sourceFree.lease, 1, local()).token,
            "source-free grant cannot fabricate an upload partition from spare output/native");
    sourceFree.lease.reset();
    empty(budget);
}
void nonemptyCrossRootMove() {
    GpuProcessingPhaseBudget first;
    GpuProcessingPhaseBudget second;
    for (auto* budget : {&first, &second}) {
        budget->setHostCapacity(1000, true);
        budget->setDeviceCapacity(1000, true);
    }
    auto oldPhase = first.reserve(request(30, 30));
    auto newPhase = second.reserve(request(40, 50));
    require(oldPhase.lease && newPhase.lease, "two distinct roots admit independent phases");
    auto old = NativeBufferStorageToken::reserve(oldPhase.lease, 20, nonlocal());
    auto incoming = NativeBufferStorageToken::reserve(newPhase.lease, 30, local());
    const auto oldRevision = first.snapshot().admissionRevision;
    const auto newRevision = second.snapshot().admissionRevision;
    // No native backing exists in this neutral control; real caller must destroy old backing first.
    token(old) = std::move(token(incoming));
    incoming.token.reset();
    require(first.snapshot().ownedNativeBuffers == 0 && first.snapshot().hostReservedBytes == 30 &&
                first.snapshot().deviceReservedBytes == 30 &&
                first.snapshot().totalHostClaimBytes == 30 &&
                first.snapshot().totalDeviceClaimBytes == 30 &&
                first.snapshot().admissionRevision == oldRevision,
            "nonempty replacement quietly folds the previous exact-root debit once");
    require(second.snapshot().ownedNativeBuffers == 1 &&
                second.snapshot().ownedNativeBufferBytes == 30 &&
                second.snapshot().totalDeviceClaimBytes == 50 &&
                second.snapshot().totalHostClaimBytes == 40 &&
                second.snapshot().admissionRevision == newRevision,
            "moved new root remains charged independently of moved-from destruction");
    oldPhase.lease.reset();
    empty(first);
    require(first.snapshot().admissionRevision == oldRevision + 1,
            "replaced origin retains only its own final phase release");
    auto weak = std::weak_ptr<GpuProcessingPhaseLease>(newPhase.lease);
    newPhase.lease.reset();
    require(!weak.expired() && second.snapshot().ownedNativeBufferBytes == 30,
            "nonempty move retains the incoming exact attempt until actual token disposal");
    old.token.reset();
    require(weak.expired() && second.snapshot().admissionRevision == newRevision + 1,
            "incoming final disposal releases its own phase once outside the mutex");
    empty(second);
}
void processingHeadroomAndPartitions() {
    GpuProcessingPhaseBudget budget;
    budget.setHostCapacity(400, true);
    budget.setDeviceCapacity(1000, true);
    auto phase = budget.reserve(mediaRequest());
    require(phase.lease != nullptr, "processing phase admitted with full protected host grant");
    auto denied = NativeBufferStorageToken::reserveProcessing(phase.lease, 1, nonlocal());
    require(!denied.token && denied.domain == GpuProcessingPhaseDomain::Host &&
                budget.snapshot().deviceReservedBytes == 300,
            "processing cannot spend output or source partitions as supplemental host credit");
    budget.setHostCapacity(550, true);
    auto processing = NativeBufferStorageToken::reserveProcessing(phase.lease, 150, nonlocal());
    require(processing.token && budget.snapshot().totalHostClaimBytes == 550 &&
                budget.snapshot().hostReservedBytes == 400 &&
                budget.snapshot().ownedNonlocalNativeBufferHostBytes == 150,
            "nonlocal processing adds exact fresh host headroom and transfers native debit");
    auto output = NativeBufferStorageToken::reserve(phase.lease, 50, nonlocal());
    auto upload = NativeBufferStorageToken::reserveUpload(phase.lease, 100, nonlocal());
    require(output.token && upload.token && budget.snapshot().totalHostClaimBytes == 550 &&
                budget.snapshot().hostReservedBytes == 250 &&
                budget.snapshot().ownedNativeBufferBytes == 300,
            "output and upload retain their independent partitions alongside processing");
    require(token(processing).reconcile(100, nonlocal()) &&
                budget.snapshot().totalHostClaimBytes == 500 &&
                budget.snapshot().hostReservedBytes == 250,
            "processing reconciliation releases only additive host bytes");
    auto moved = NativeBufferStorageToken(std::move(token(processing)));
    token(output) = std::move(moved);
    processing.token.reset();
    require(budget.snapshot().totalHostClaimBytes == 500 &&
                budget.snapshot().hostReservedBytes == 300 &&
                budget.snapshot().ownedNativeBufferBytes == 200,
            "processing tag survives construction and nonempty cross-partition assignment");
    const auto revision = budget.snapshot().admissionRevision;
    phase.lease->markUnproven();
    phase.lease.reset();
    require(budget.snapshot().unprovenPhases == 1 && budget.snapshot().totalHostClaimBytes == 500,
            "live processing tokens retain exact unproven phase and supplemental claim");
    upload.token.reset();
    output.token.reset();
    empty(budget);
    require(budget.snapshot().admissionRevision == revision + 1,
            "last processing token releases its retained phase once outside the lock");
}
void processingFreshnessAndOverflow() {
    GpuProcessingPhaseBudget budget;
    const auto now = Clock::now();
    budget.setHostCapacity(1000, true, now);
    budget.setDeviceCapacity(1000, true);
    auto phase = budget.reserve(request());
    require(phase.lease != nullptr, "freshness phase admitted");
    auto denied = NativeBufferStorageToken::reserveProcessing(phase.lease, 1, nonlocal(),
                                                              now + std::chrono::seconds(1));
    require(!denied.token && denied.status == GpuProcessingPhaseAdmission::AwaitingCapacity &&
                !budget.snapshot().hostReady && budget.snapshot().ownedNativeBuffers == 0,
            "expired supplemental headroom closes readiness before any host/native half claim");
    auto localBuffer = NativeBufferStorageToken::reserveProcessing(phase.lease, 100, local());
    require(localBuffer.token && budget.snapshot().totalHostClaimBytes == 80,
            "local processing only converts admitted native debit under host staleness");
    localBuffer.token.reset();
    phase.lease.reset();
    empty(budget);
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    budget.setHostCapacity(maximum, true);
    budget.setDeviceCapacity(maximum, true);
    phase = budget.reserve(request(maximum, maximum));
    require(phase.lease != nullptr, "maximum processing grant admitted");
    denied = NativeBufferStorageToken::reserveProcessing(phase.lease, 1, nonlocal());
    require(!denied.token && denied.domain == GpuProcessingPhaseDomain::Host &&
                budget.snapshot().totalHostClaimBytes == maximum &&
                budget.snapshot().deviceReservedBytes == maximum,
            "supplemental host addition rejects overflow before atomic native conversion");
    phase.lease.reset();
    empty(budget);
}
void retainedProcessingOwnership() {
    GpuProcessingPhaseBudget budget(1);
    budget.setHostCapacity(1000, true);
    budget.setDeviceCapacity(150, true);
    auto phase = budget.reserve(request());
    require(phase.lease != nullptr, "retained processing origin admitted");
    auto first = NativeBufferStorageToken::reserveProcessing(phase.lease, 40, local());
    auto second = NativeBufferStorageToken::reserveProcessing(phase.lease, 30, nonlocal());
    require(first.token && second.token && token(first).canPromote() && token(first).promote() &&
                token(second).promote(),
            "retained buffers publish from their exact proven origin");
    require(!token(first).reconcile(30, local()), "published backing cannot be reconciled later");
    const auto weak = std::weak_ptr<GpuProcessingPhaseLease>(phase.lease);
    phase.lease.reset();
    require(weak.expired() && budget.snapshot().admittedPhases == 0 &&
                budget.snapshot().totalDeviceClaimBytes == 70 &&
                budget.snapshot().totalHostClaimBytes == 30,
            "cached charges survive independently without pinning the whole origin phase");
    const auto denied = budget.reserve(request());
    require(!denied.lease && denied.domain == GpuProcessingPhaseDomain::Device,
            "later phase cannot double-spend retained buffer backing");
    const auto revision = budget.snapshot().admissionRevision;
    auto moved = NativeBufferStorageToken(std::move(token(first)));
    first.token.reset();
    moved = {};
    second.token.reset();
    require(budget.snapshot().admissionRevision == revision + 2,
            "actual independent retained releases emit one capacity pulse each");
    empty(budget);
    phase = budget.reserve(request());
    require(phase.lease != nullptr, "retained destruction restores capacity-one admission");
    auto prefix = NativeBufferStorageToken::reserveProcessing(phase.lease, 40, nonlocal());
    require(prefix.token && token(prefix).promote(), "partly published failed prefix exists");
    const auto before = budget.snapshot();
    GpuProcessingPhaseBudget foreign;
    foreign.setHostCapacity(1000, true);
    foreign.setDeviceCapacity(1000, true);
    auto wrong = foreign.reserve(request());
    require(wrong.lease && !token(prefix).releaseFailedAttemptAfterBackingDestroyed(wrong.lease) &&
                token(prefix).releaseFailedAttemptAfterBackingDestroyed(phase.lease),
            "only exact root/phase may quietly fold destroyed failed-prefix backing");
    require(budget.snapshot().admissionRevision == before.admissionRevision &&
                budget.snapshot().deviceReservedBytes == 100 &&
                budget.snapshot().totalHostClaimBytes == 80,
            "failed publication foldback conserves origin debit without self-retry pulse");
    prefix.token.reset();
    auto transient = NativeBufferStorageToken::reserve(phase.lease, 20, local());
    require(transient.token && !token(transient).canPromote() && !token(transient).promote(),
            "ordinary output partition cannot become retained processing storage");
    transient.token.reset();
    phase.lease.reset();
    wrong.lease.reset();
    empty(budget);
    empty(foreign);
}
} // namespace

int main() {
    try {
        pairedAdmissionAndReconcile();
        grandfatheredConversionAndLifetime();
        maximumConservation();
        nonemptyCrossRootMove();
        protectedUploadPartition();
        grandfatheredProtectedUpload();
        movedUploadPartition();
        maximumMediaPartition();
        processingHeadroomAndPartitions();
        processingFreshnessAndOverflow();
        retainedProcessingOwnership();
        std::cout << "PASS: neutral transient native buffer storage\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
