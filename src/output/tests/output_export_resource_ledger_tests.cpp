#include <bloom/output/output_analysis_attempt.hpp>
#include <bloom/output/output_export_resource_ledger.hpp>
#include <bloom/render/packed_host_retirement.hpp>
#include <bloom/render/processing_host_storage.hpp>
#include <bloom/runtime/cancellation.hpp>
#include <bloom/runtime/gpu_processing_phase_policy.hpp>

#include "flat_exr_test_support.hpp"

#include <algorithm>
#include <array>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <source_location>
#include <string_view>
#include <thread>

namespace {

namespace output = bloom::output;
namespace render = bloom::render;
namespace runtime = bloom::runtime;
namespace support = bloom_output_flat_exr_test_support;
constexpr std::uint64_t gib = 1024ULL * 1024 * 1024;
runtime::GpuProcessingPhasePolicy::Clock::time_point scriptedExportSampleAt{};
runtime::MemoryBudgetSample scriptedExportSample(const runtime::CancellationToken&) noexcept {
    runtime::MemoryBudgetSample sample;
    sample.process.sampledAt = scriptedExportSampleAt;
    return sample;
}

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message,
                const std::source_location location = std::source_location::current()) {
        if (condition)
            return;
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }
    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

[[nodiscard]] std::shared_ptr<runtime::GpuProcessingPhasePolicy>
sampledRoot(const std::uint64_t availableBytes,
            const std::shared_ptr<render::PackedHostRetirementOwner>& owner = {}) {
    auto root = std::make_shared<runtime::GpuProcessingPhasePolicy>(32, owner);
    runtime::MemoryBudgetState delivered;
    delivered.machine.availableBytes = availableBytes;
    delivered.process.source = runtime::ProcessMemorySource::MacOsHostOnly;
    delivered.process.status = runtime::ProcessMemoryStatus::Unsupported;
    delivered.process.sampledAt = runtime::GpuProcessingPhasePolicy::Clock::now();
    delivered.sampleSerial = 1;
    delivered.processSampleFresh = true;
    root->applyHostSample(delivered, 16 * gib, delivered.process.sampledAt);
    return root;
}

void legacyExplicitQuotasAndLifetime(Expectations& checks) {
    output::ExportResourceLedgerV1 ledger(1024);
    output::ExportResourceAdmissionStatusV1 status{};
    auto first = ledger.reserve(100, status, 512);
    checks.expect(first && status == output::ExportResourceAdmissionStatusV1::Reserved &&
                      first->chargedBytes() == 100 && ledger.chargedBytes() == 100,
                  "explicit unrooted reservation charges once");
    if (!first)
        return;
    checks.expect(first->expand(250) == output::ExportResourceAdmissionStatusV1::Reserved &&
                      ledger.chargedBytes() == 250,
                  "unrooted expansion charges only the increment");
    checks.expect(first->expand(200) == output::ExportResourceAdmissionStatusV1::Reserved &&
                      first->chargedBytes() == 250,
                  "a smaller expansion never releases a live claim");
    checks.expect(first->expand(513) ==
                      output::ExportResourceAdmissionStatusV1::JobAllowanceExceeded,
                  "explicit job cap binds expansion");
    auto refused = ledger.reserve(800, status, 1000);
    checks.expect(!refused &&
                      status == output::ExportResourceAdmissionStatusV1::ServiceAllowanceExceeded &&
                      ledger.chargedBytes() == 250,
                  "explicit service cap refuses without disturbing held work");
    first.reset();
    checks.expect(ledger.chargedBytes() == 0, "last release returns the complete charge");
    auto admitted = ledger.reserve(800, status, 1000);
    checks.expect(admitted && status == output::ExportResourceAdmissionStatusV1::Reserved,
                  "release reopens explicit service admission");
}

void rootedClaimsUseTheSampledRoot(Expectations& checks) {
    auto root = sampledRoot(8 * gib);
    output::ExportResourceLedgerV1 ledger;
    output::ExportResourceAdmissionStatusV1 status{};
    auto first = ledger.reserve(3 * gib, status, std::nullopt, root);
    checks.expect(first && status == output::ExportResourceAdmissionStatusV1::Reserved &&
                      !first->jobAllowance() && first->processingPolicy() == root,
                  "a rooted export uses sampled headroom beyond the old implicit 2 GiB job cap");
    if (!first)
        return;
    auto second = ledger.reserve(2 * gib, status, std::nullopt, root);
    checks.expect(second && ledger.chargedBytes() == 5 * gib,
                  "the old implicit 4 GiB service cap does not trim rooted exports");
    const auto held = root->snapshot().reservations;
    checks.expect(held.totalHostClaimBytes == 5 * gib && held.ownedProcessingHostBytes == 5 * gib &&
                      root->snapshot().reservations.ownedFinalProcessHostBytes == 0,
                  "unmaterialized export claims occupy root headroom without fabricated backing");
    checks.expect(root->reserve({.output = runtime::GpuProcessingPhaseOutput::FloatFinal,
                                 .pixels = 0,
                                 .sourceFree = true,
                                 .native = false})
                          .status == render::GpuProcessingPhaseAdmission::Accepted,
                  "a zero-byte output phase is a real admitted root phase");
    second.reset();
    checks.expect(root->snapshot().reservations.totalHostClaimBytes == 3 * gib,
                  "one completed export retires only its own host claim");
    first.reset();
    checks.expect(root->snapshot().reservations.totalHostClaimBytes == 0 &&
                      ledger.chargedBytes() == 0,
                  "last export releases all tokens and ledger claims");
}

void rootedRefusalAndExplicitCaps(Expectations& checks) {
    auto root = sampledRoot(gib);
    output::ExportResourceLedgerV1 implicit;
    output::ExportResourceAdmissionStatusV1 status{};
    auto refused = implicit.reserve(2 * gib, status, std::nullopt, root);
    checks.expect(!refused &&
                      status == output::ExportResourceAdmissionStatusV1::ServiceAllowanceExceeded &&
                      implicit.chargedBytes() == 0 &&
                      root->snapshot().reservations.totalHostClaimBytes == 0 &&
                      root->snapshot().reservations.admittedPhases == 0,
                  "real root headroom refuses and leaves no phase or ledger claim");

    auto roomyRoot = sampledRoot(8 * gib);
    output::ExportResourceLedgerV1 explicitLedger(4 * gib);
    auto first = explicitLedger.reserve(3 * gib, status, std::nullopt, roomyRoot);
    auto second = explicitLedger.reserve(2 * gib, status, std::nullopt, roomyRoot);
    checks.expect(first && !second &&
                      status == output::ExportResourceAdmissionStatusV1::ServiceAllowanceExceeded &&
                      explicitLedger.chargedBytes() == 3 * gib,
                  "an explicit constructor service cap still binds rooted exports");
    auto jobRefused = implicit.reserve(2 * gib, status, gib, roomyRoot);
    checks.expect(!jobRefused &&
                      status == output::ExportResourceAdmissionStatusV1::JobAllowanceExceeded,
                  "an explicit per-job cap still binds rooted exports");
}

void rootedExpansionAndFacadeLifetime(Expectations& checks) {
    auto root = sampledRoot(gib);
    std::shared_ptr<output::ExportResourceReservationV1> reservation;
    {
        output::ExportResourceLedgerV1 ledger;
        output::ExportResourceAdmissionStatusV1 status{};
        reservation = ledger.reserve(100, status, std::nullopt, root);
        checks.expect(reservation && root->snapshot().reservations.totalHostClaimBytes == 100,
                      "the initial unowned metadata claim is held");
    }
    if (!reservation)
        return;
    auto alias = reservation;
    reservation.reset();
    checks.expect(alias->expand(250) == output::ExportResourceAdmissionStatusV1::Reserved &&
                      alias->chargedBytes() == 250 &&
                      root->snapshot().reservations.totalHostClaimBytes == 250,
                  "reservation remains expandable after facade destruction");
    checks.expect(alias->expand(200) == output::ExportResourceAdmissionStatusV1::Reserved &&
                      root->snapshot().reservations.totalHostClaimBytes == 250,
                  "a lower peak cannot release a still-live claim");
    alias.reset();
    checks.expect(root->snapshot().reservations.totalHostClaimBytes == 0 &&
                      root->snapshot().reservations.admittedPhases == 0,
                  "last alias retires incremental tokens and phase together");
}

void concurrentExpansionChargesOnce(Expectations& checks) {
    auto root = sampledRoot(gib);
    output::ExportResourceLedgerV1 ledger;
    output::ExportResourceAdmissionStatusV1 status{};
    auto reservation = ledger.reserve(100, status, std::nullopt, root);
    checks.expect(reservation != nullptr, "concurrent-expansion fixture is admitted");
    if (!reservation)
        return;
    std::barrier start(3);
    output::ExportResourceAdmissionStatusV1 first{};
    output::ExportResourceAdmissionStatusV1 second{};
    std::thread a([&] {
        start.arrive_and_wait();
        first = reservation->expand(250);
    });
    std::thread b([&] {
        start.arrive_and_wait();
        second = reservation->expand(250);
    });
    start.arrive_and_wait();
    a.join();
    b.join();
    checks.expect(first == output::ExportResourceAdmissionStatusV1::Reserved &&
                      second == output::ExportResourceAdmissionStatusV1::Reserved &&
                      reservation->chargedBytes() == 250 && ledger.chargedBytes() == 250 &&
                      root->snapshot().reservations.totalHostClaimBytes == 250,
                  "concurrent expansion retains the single peak exactly once");
}

void publicationScratchClaimsOverlapAndRetire(Expectations& checks) {
    auto root = sampledRoot(gib);
    output::ExportResourceLedgerV1 ledger;
    output::ExportResourceAdmissionStatusV1 status{};
    const runtime::CancellationToken cancellation;
    auto attempt = ledger.reserve(100, status, std::nullopt, root);
    checks.expect(attempt != nullptr, "attempt fixture is admitted");
    if (!attempt)
        return;
    auto first = attempt->reserveScratch(80, status, cancellation);
    auto second = attempt->reserveScratch(80, status, cancellation);
    checks.expect(first && second && ledger.chargedBytes() == 260 &&
                      root->snapshot().reservations.totalHostClaimBytes == 260 &&
                      attempt->chargedBytes() == 100,
                  "overlapping publications reserve independent scratch without recharging pixels");
    first.reset();
    checks.expect(ledger.chargedBytes() == 180 &&
                      root->snapshot().reservations.totalHostClaimBytes == 180,
                  "first publication retires only its own scratch");
    second.reset();
    auto sequential = attempt->reserveScratch(80, status, cancellation);
    checks.expect(sequential && ledger.chargedBytes() == 180 &&
                      root->snapshot().reservations.totalHostClaimBytes == 180,
                  "sequential publication reuses headroom without accumulating old peak");
    sequential.reset();
    attempt.reset();
    checks.expect(ledger.chargedBytes() == 0 &&
                      root->snapshot().reservations.totalHostClaimBytes == 0,
                  "all publication and retained claims retire on last owner release");

    output::ExportResourceLedgerV1 capped(200);
    auto base = capped.reserve(100, status, 150, root);
    checks.expect(base != nullptr, "explicit-cap attempt fixture is admitted");
    if (!base)
        return;
    auto jobDenied = base->reserveScratch(51, status, cancellation);
    checks.expect(!jobDenied &&
                      status == output::ExportResourceAdmissionStatusV1::JobAllowanceExceeded,
                  "scratch observes the parent's explicit total per-job cap");
    auto one = base->reserveScratch(50, status, cancellation);
    auto two = base->reserveScratch(50, status, cancellation);
    checks.expect(one && !two &&
                      status == output::ExportResourceAdmissionStatusV1::JobAllowanceExceeded &&
                      capped.chargedBytes() == 150,
                  "overlapping scratch claims share the explicit job cap");
    one.reset();
    auto laterJobScratch = base->reserveScratch(50, status, cancellation);
    checks.expect(laterJobScratch && capped.chargedBytes() == 150,
                  "scratch retirement reopens the same explicit job allowance");

    output::ExportResourceLedgerV1 serviceCapped(200);
    auto serviceBase = serviceCapped.reserve(100, status, 300, root);
    checks.expect(serviceBase != nullptr, "explicit service-cap fixture is admitted");
    if (!serviceBase)
        return;
    auto serviceOne = serviceBase->reserveScratch(50, status, cancellation);
    auto serviceTwo = serviceBase->reserveScratch(50, status, cancellation);
    auto serviceDenied = serviceBase->reserveScratch(1, status, cancellation);
    checks.expect(serviceOne && serviceTwo && !serviceDenied &&
                      status == output::ExportResourceAdmissionStatusV1::ServiceAllowanceExceeded &&
                      serviceCapped.chargedBytes() == 200,
                  "the explicit service cap still binds simultaneous job claims");
}

void explicitJobFamilyClaimsAreAggregate(Expectations& checks) {
    auto root = sampledRoot(gib);
    output::ExportResourceLedgerV1 ledger;
    output::ExportResourceAdmissionStatusV1 status{};
    const runtime::CancellationToken cancellation;
    auto parent = ledger.reserve(10, status, 100, root);
    checks.expect(parent != nullptr, "aggregate job fixture is admitted");
    if (!parent)
        return;

    std::barrier start(3);
    std::shared_ptr<output::ExportResourceReservationV1> owned, scratch;
    output::ExportResourceAdmissionStatusV1 ownedStatus{}, scratchStatus{};
    std::thread a([&] {
        start.arrive_and_wait();
        owned = parent->reserveOwnedHostBacking(80, ownedStatus, cancellation);
    });
    std::thread b([&] {
        start.arrive_and_wait();
        scratch = parent->reserveScratch(80, scratchStatus, cancellation);
    });
    start.arrive_and_wait();
    a.join();
    b.join();
    checks.expect(static_cast<bool>(owned) != static_cast<bool>(scratch) &&
                      (owned ? scratchStatus : ownedStatus) ==
                          output::ExportResourceAdmissionStatusV1::JobAllowanceExceeded &&
                      ledger.chargedBytes() == 90 &&
                      root->snapshot().reservations.totalHostClaimBytes == 90,
                  "concurrent sibling claims cannot each spend the same remaining job allowance");
    owned.reset();
    scratch.reset();
    checks.expect(ledger.chargedBytes() == 10,
                  "retired sibling returns its exact charge to the shared job counter");

    owned = parent->reserveOwnedHostBacking(80, status, cancellation);
    checks.expect(owned && ledger.chargedBytes() == 90,
                  "owned host child reuses the released job allowance");
    if (!owned)
        return;
    auto nested = owned->reserveScratch(5, status, cancellation);
    auto nestedDenied = owned->reserveOwnedHostBacking(6, status, cancellation);
    checks.expect(nested && !nestedDenied &&
                      status == output::ExportResourceAdmissionStatusV1::JobAllowanceExceeded &&
                      ledger.chargedBytes() == 95 &&
                      root->snapshot().reservations.totalHostClaimBytes == 95,
                  "nested refusal rolls back its root phase and leaves the family charge intact");
    checks.expect(owned->expand(85) == output::ExportResourceAdmissionStatusV1::Reserved &&
                      parent->expand(11) ==
                          output::ExportResourceAdmissionStatusV1::JobAllowanceExceeded &&
                      ledger.chargedBytes() == 100 &&
                      root->snapshot().reservations.totalHostClaimBytes == 100,
                  "child expansion consumes the shared limit and blocks parent expansion");
    nested.reset();
    checks.expect(parent->expand(15) == output::ExportResourceAdmissionStatusV1::Reserved &&
                      ledger.chargedBytes() == 100,
                  "nested retirement releases exactly enough allowance for parent growth");
    owned.reset();
    auto recovered = parent->reserveScratch(85, status, cancellation);
    checks.expect(recovered && ledger.chargedBytes() == 100,
                  "child retirement reopens allowance for a later sibling");
    if (!recovered)
        return;
    parent.reset();
    auto afterParent = recovered->reserveScratch(15, status, cancellation);
    auto afterParentDenied = recovered->reserveScratch(1, status, cancellation);
    checks.expect(afterParent && !afterParentDenied &&
                      status == output::ExportResourceAdmissionStatusV1::JobAllowanceExceeded &&
                      ledger.chargedBytes() == 100,
                  "descendants retain the same job cap after the original parent retires");
}

void publicationRefreshFailureRetainsNoScratch(Expectations& checks) {
    auto root = sampledRoot(gib);
    output::ExportResourceLedgerV1 ledger;
    output::ExportResourceAdmissionStatusV1 status{};
    const runtime::CancellationToken cancellation;
    unsigned refreshes = 0;
    auto attempt = ledger.reserve(100, status, std::nullopt, root, 0,
                                  [&refreshes](const runtime::CancellationToken&) { ++refreshes; });
    checks.expect(attempt != nullptr, "refresh fixture is admitted");
    if (!attempt)
        return;
    auto scratch = attempt->reserveScratch(20, status, cancellation);
    checks.expect(scratch && refreshes == 1 && ledger.chargedBytes() == 120,
                  "publication refresh runs before its independent scratch admission");
    scratch.reset();
    const auto beforeExpansion = attempt->refreshCapacity(cancellation);
    checks.expect(beforeExpansion == output::ExportResourceAdmissionStatusV1::Reserved &&
                      refreshes == 2 &&
                      attempt->expand(130) == output::ExportResourceAdmissionStatusV1::Reserved,
                  "retained worker refresh also precedes a queue-style expansion");
    attempt.reset();

    auto failing = ledger.reserve(100, status, std::nullopt, root, 0,
                                  [](const runtime::CancellationToken&) { throw 1; });
    checks.expect(failing != nullptr, "throwing-refresh fixture is admitted");
    if (!failing)
        return;
    auto denied = failing->reserveScratch(20, status, cancellation);
    checks.expect(!denied &&
                      status == output::ExportResourceAdmissionStatusV1::CapacityRefreshFailed &&
                      ledger.chargedBytes() == 100 &&
                      root->snapshot().reservations.totalHostClaimBytes == 100,
                  "refresh failure refuses publication without a partial scratch claim");
}

void ownedAudioBackingReconcilesWithoutScratchCredit(Expectations& checks) {
    const auto firstAt = runtime::GpuProcessingPhasePolicy::Clock::now() - std::chrono::seconds(2);
    runtime::MemoryBudgetState delivered;
    delivered.machine.availableBytes = 200;
    delivered.process.source = runtime::ProcessMemorySource::OtherHostOnly;
    delivered.process.status = runtime::ProcessMemoryStatus::Unsupported;
    delivered.process.sampledAt = firstAt;
    delivered.processSampleFresh = true;
    delivered.sampleSerial = 1;
    auto root = std::make_shared<runtime::GpuProcessingPhasePolicy>();
    root->applyHostSample(delivered, 200, runtime::GpuProcessingPhasePolicy::Clock::now());
    output::ExportResourceLedgerV1 ledger;
    output::ExportResourceAdmissionStatusV1 status{};
    const runtime::CancellationToken cancellation;
    auto queue = ledger.reserve(10, status, std::nullopt, root);
    checks.expect(queue && !queue->ownedHostPhase(),
                  "queue metadata has no materialized-host phase authority");
    if (!queue)
        return;
    auto retained = queue->reserveOwnedHostBacking(80, status, cancellation);
    auto scratch = queue->reserveScratch(20, status, cancellation);
    checks.expect(retained && retained->ownedHostPhase() && scratch && !scratch->ownedHostPhase() &&
                      ledger.chargedBytes() == 110,
                  "retained backing and temporary decode scratch have distinct root claims");
    if (!retained || !scratch)
        return;
    auto backing = render::adoptProcessingHostBacking(std::make_unique<std::array<std::byte, 80>>(),
                                                      retained->ownedHostPhase(), 80, 80);
    checks.expect(backing && root->snapshot().reservations.totalHostClaimBytes == 110 &&
                      root->snapshot().reservations.ownedProcessingHostBytes == 110,
                  "materializing retained backing transfers rather than adds to its fixed claim");
    scratch.reset();
    checks.expect(ledger.chargedBytes() == 90 &&
                      root->snapshot().reservations.totalHostClaimBytes == 90,
                  "decode scratch releases while the real audio buffer remains held");

    scriptedExportSampleAt = firstAt + std::chrono::seconds(1);
    static_cast<void>(root->sampleHostMemory(cancellation, scriptedExportSample));
    delivered.machine.availableBytes = 60;
    delivered.process.sampledAt = scriptedExportSampleAt;
    ++delivered.sampleSerial;
    root->applyHostSample(delivered, 200, runtime::GpuProcessingPhasePolicy::Clock::now());
    auto second = queue->reserveOwnedHostBacking(50, status, cancellation);
    checks.expect(second && root->snapshot().reservations.hostCapacityBytes == 140 &&
                      root->snapshot().reservations.totalHostClaimBytes == 140,
                  "a second audio buffer fits after exact live backing credit and scratch release");
    auto overcommit = queue->reserveOwnedHostBacking(1, status, cancellation);
    checks.expect(!overcommit &&
                      status == output::ExportResourceAdmissionStatusV1::ServiceAllowanceExceeded,
                  "simultaneous held audio claims still refuse overcommit");
    backing.reset();
    checks.expect(root->snapshot().reservations.hostCapacityBytes == 60,
                  "audio backing retirement revokes its live-sample credit");
    second.reset();
    retained.reset();
    queue.reset();
    checks.expect(ledger.chargedBytes() == 0 &&
                      root->snapshot().reservations.totalHostClaimBytes == 0,
                  "all audio and queue claims retire after backing ownership ends");
}

void ownedAudioBackingHonorsExplicitServiceCap(Expectations& checks) {
    auto root = sampledRoot(gib);
    output::ExportResourceLedgerV1 ledger(110);
    output::ExportResourceAdmissionStatusV1 status{};
    const runtime::CancellationToken cancellation;
    auto queue = ledger.reserve(10, status, std::nullopt, root);
    if (!queue) {
        checks.expect(false, "explicit-cap queue fixture is admitted");
        return;
    }
    auto retained = queue->reserveOwnedHostBacking(80, status, cancellation);
    auto scratch = queue->reserveScratch(20, status, cancellation);
    auto denied = queue->reserveOwnedHostBacking(1, status, cancellation);
    checks.expect(retained && scratch && !denied &&
                      status == output::ExportResourceAdmissionStatusV1::ServiceAllowanceExceeded &&
                      ledger.chargedBytes() == 110,
                  "explicit service cap includes retained audio and simultaneous decode scratch");
    scratch.reset();
    auto next = queue->reserveOwnedHostBacking(20, status, cancellation);
    checks.expect(next && ledger.chargedBytes() == 110,
                  "retired scratch frees explicit service allowance for later audio backing");
}

void attemptDeductsOnlyPublishedSameRootBacking(Expectations& checks) {
    struct RetirementOwner final {
        std::shared_ptr<render::PackedHostRetirementOwner> owner =
            render::PackedHostRetirementOwner::create();
        ~RetirementOwner() {
            owner->beginShutdown();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (!owner->exitAcknowledged() && std::chrono::steady_clock::now() < deadline)
                std::this_thread::yield();
            if (!owner->collect())
                std::terminate();
        }
    } retirement;
    checks.expect(retirement.owner->prepare(), "rooted fixture retirement owner prepares");
    auto root = sampledRoot(8 * gib, retirement.owner);
    auto fixture = support::roundTripFixture();
    const auto descriptor = *fixture.processImage.descriptor();
    const auto oldPixels = fixture.processImage.pixels();
    auto phase = root->reserve({.output = runtime::GpuProcessingPhaseOutput::FloatFinal,
                                .pixels = descriptor.layout().pixelCount,
                                .sourceFree = true,
                                .native = false});
    checks.expect(phase.lease != nullptr, "rooted fixture phase is admitted");
    if (!phase.lease)
        return;
    auto builder = render::Rgba32fImageBuilder::create(
        descriptor, descriptor.layout().pixelStorageBytes, phase.lease);
    checks.expect(static_cast<bool>(builder), "rooted fixture image allocates");
    if (!builder)
        return;
    for (std::uint32_t row = 0; row < descriptor.dataWindow().extent().height(); ++row) {
        auto target = builder.value()->row(descriptor.dataWindow().originY() + row);
        checks.expect(static_cast<bool>(target), "rooted fixture row exists");
        if (!target)
            return;
        const auto offset =
            static_cast<std::size_t>(row) * descriptor.dataWindow().extent().width();
        std::ranges::copy(oldPixels.subspan(offset, target.value()->size()),
                          target.value()->begin());
    }
    auto image = std::move(*builder.value()).freeze();
    checks.expect(static_cast<bool>(image), "rooted fixture image publishes");
    if (!image)
        return;
    checks.expect(
        render::PackedHostStorageAuthority::promoteFinalProduct(image.value()->storageAuthority()),
        "rooted fixture backing promotes before publication");
    fixture.processImage = std::move(*image.value());
    auto source = support::prepareSource(std::move(fixture));
    const auto authority = source.frame->processImage().storageAuthority();
    checks.expect(authority && authority->snapshot().published &&
                      authority->rootIdentity() == root->rootIdentity(),
                  "attempt source carries published backing under the expected root");
    if (!authority)
        return;
    const auto rootedBytes = authority->snapshot().bytes;
    const auto before = root->snapshot().reservations.totalHostClaimBytes;
    output::ExportResourceLedgerV1 ledger;
    auto build = [&](const std::shared_ptr<runtime::GpuProcessingPhasePolicy>& policy) {
        return output::buildOutputAnalysisAttemptV1(
            {.frame = source.frame,
             .processIdentity = source.processIdentity,
             .report = source.report,
             .target = {.targetKey = bloom::core::ArtifactTargetKey::fromRaw(7),
                        .observation = bloom::platform::ArtifactTargetObservation::absent(),
                        .targetPath = "/tmp/unused-rooted-export.exr",
                        .overwritePolicy =
                            bloom::platform::ArtifactOverwritePolicy::CreateOrReplace},
             .display = {},
             .processingPolicy = policy},
            ledger);
    };
    auto admitted = build(root);
    checks.expect(static_cast<bool>(admitted), "same-root published process frame is admitted");
    if (!admitted)
        return;
    const auto reservation = admitted.attempt()->resources();
    checks.expect(reservation->alreadyRootedBytes() == rootedBytes &&
                      root->snapshot().reservations.totalHostClaimBytes - before ==
                          reservation->chargedBytes() - rootedBytes,
                  "export deducts only the already-published pixel payload, not metadata");
    const auto held = root->snapshot().reservations.totalHostClaimBytes;
    auto alien = build(sampledRoot(8 * gib));
    checks.expect(!alien &&
                      alien.error() ==
                          output::OutputAnalysisAttemptErrorCodeV1::ResourceReservationFailed &&
                      root->snapshot().reservations.totalHostClaimBytes == held,
                  "mismatched root refuses rather than crediting another root's pixels");
}

} // namespace

int main() {
    Expectations checks;
    legacyExplicitQuotasAndLifetime(checks);
    rootedClaimsUseTheSampledRoot(checks);
    rootedRefusalAndExplicitCaps(checks);
    rootedExpansionAndFacadeLifetime(checks);
    concurrentExpansionChargesOnce(checks);
    publicationScratchClaimsOverlapAndRetire(checks);
    explicitJobFamilyClaimsAreAggregate(checks);
    publicationRefreshFailureRetainsNoScratch(checks);
    ownedAudioBackingReconcilesWithoutScratchCredit(checks);
    ownedAudioBackingHonorsExplicitServiceCap(checks);
    attemptDeductsOnlyPublishedSameRootBacking(checks);
    return checks.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
