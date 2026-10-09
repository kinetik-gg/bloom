#include <bloom/runtime/process_memory_sample.hpp>

#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
using namespace bloom::runtime;
constexpr std::size_t gib = std::size_t{1024} * 1024 * 1024;
int failures = 0;
void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        ++failures;
        std::cerr << message << '\n';
    }
}

#if defined(__linux__)
struct Files final {
    std::map<std::string, std::string> values;
    void domain(const std::string& path, std::string limit, std::string current) {
        values[path + "/memory.max"] = std::move(limit);
        values[path + "/memory.current"] = std::move(current);
    }
    ProcessMemoryFileReader reader() const {
        return [this](const std::filesystem::path& path) -> ProcessMemoryFileRead {
            const auto found = values.find(path.generic_string());
            return found == values.end()
                       ? ProcessMemoryFileRead{ProcessMemoryReadStatus::Missing, {}}
                       : ProcessMemoryFileRead{ProcessMemoryReadStatus::Value, found->second};
        };
    }
};
constexpr auto mount = "36 25 0:32 / /sys/fs/cgroup rw - cgroup2 cgroup rw\n";

void pairedHierarchy() {
    Files files;
    files.domain("/sys/fs/cgroup/leaf", std::to_string(8 * gib), std::to_string(6 * gib));
    files.domain("/sys/fs/cgroup", std::to_string(12 * gib), std::to_string(23 * gib / 2));
    const auto sample = parseLinuxProcessMemorySample("0::/leaf\n", mount, files.reader());
    expect(sample.view && sample.view->mountId == 36 && sample.view->filesystemDevice == "0:32" &&
               sample.view->mountRoot == "/" && sample.view->mountPoint == "/sys/fs/cgroup",
           "selected mount provenance retained without inferring namespace identity");
    expect(sample.status == ProcessMemoryStatus::Finite && sample.domains.size() == 2,
           "both own and ancestor domains are paired and finite");
    expect(minimumProcessCacheAllowance(sample, gib) == gib / 2,
           "tighter ancestor uses its own usage and credit once, never leaf usage");
    expect(processCacheAllowanceForDomain(8 * gib, 6 * gib, gib, gib) == 2 * gib &&
               processCacheAllowanceForDomain(8 * gib, 6 * gib, 0, gib) == gib,
           "paired-domain cache formula credits exclusive host bytes once");
    files.domain("/sys/fs/cgroup/leaf", "max", "0");
    const auto unlimitedLeaf = parseLinuxProcessMemorySample("0::/leaf\n", mount, files.reader());
    expect(unlimitedLeaf.status == ProcessMemoryStatus::Finite &&
               minimumProcessCacheAllowance(unlimitedLeaf, gib) == gib / 2,
           "unlimited leaf still honors finite visible ancestor");
    files.domain("/sys/fs/cgroup", "max", "0");
    const auto unlimited = parseLinuxProcessMemorySample("0::/leaf\n", mount, files.reader());
    expect(unlimited.status == ProcessMemoryStatus::NoFiniteVisibleLimit &&
               !minimumProcessCacheAllowance(unlimited, 0),
           "successfully measured no-finite-visible differs from unknown");
}

void pathsAndProvenance() {
    Files files;
    files.domain("/sys/cgroup memory/leaf", "256", "0");
    files.domain("/sys/cgroup memory", "max", "0");
    constexpr auto bind = "36 25 0:32 /parent /sys/cgroup\\040memory rw - cgroup2 cgroup rw\n";
    const auto mapped = parseLinuxProcessMemorySample("0::/parent/leaf\n", bind, files.reader());
    expect(mapped.status == ProcessMemoryStatus::Finite && mapped.domains.size() == 2 &&
               mapped.domains.front().path == "/sys/cgroup memory/leaf",
           "bind root and octal escaped mountpoint resolve correct paths");
    const auto boundary = parseLinuxProcessMemorySample("0::/parentX/leaf\n", bind, files.reader());
    expect(boundary.status == ProcessMemoryStatus::Unavailable && boundary.domains.empty(),
           "mount root prefix must match directory boundary");
    const auto namespaceRoot = parseLinuxProcessMemorySample("0::/\n", bind, files.reader());
    expect(namespaceRoot.status == ProcessMemoryStatus::Unavailable &&
               namespaceRoot.domains.empty(),
           "ambiguous namespace root is not mapped to arbitrary constrained bind root");
    const auto v1 = parseLinuxProcessMemorySample(
        "5:memory,cpu:/leaf\n", "36 25 0:32 / /sys/cgroup/memory rw - cgroup cgroup rw,memory\n",
        files.reader());
    expect(v1.source == ProcessMemorySource::LinuxCgroupV1Unsupported &&
               v1.status == ProcessMemoryStatus::Unsupported && v1.domains.empty(),
           "v1 fallback is explicit unsupported, never a measured unlimited v2 domain");
    const auto migrated = parseLinuxProcessMemorySample("0::/parent\n", bind, files.reader());
    expect(migrated.membershipPath != mapped.membershipPath && migrated.domains.size() == 1,
           "each sample resolves current membership instead of stale cached topology");
}

void rootAndAlternateMounts() {
    Files files;
    const auto bareRoot =
        parseLinuxProcessMemorySample("0::/\n", mount, files.reader(), {},
                                      {.cgroup = ProcessMemoryNamespaceIdentity{1, 2},
                                       .mount = ProcessMemoryNamespaceIdentity{1, 3}});
    expect(bareRoot.domains.empty() && bareRoot.rootInterfacesAbsent &&
               isCompleteProcessMemorySample(bareRoot),
           "trusted true-root interface absence is complete for initial host-only admission");
    auto inventedRoot = bareRoot;
    inventedRoot.rootInterfacesAbsent = false;
    expect(!isCompleteProcessMemorySample(inventedRoot),
           "empty no-finite label without parser root evidence stays incomplete");
    files.domain("/sys/fs/cgroup/parent/child", "max", "0");
    files.domain("/sys/fs/cgroup/parent", std::to_string(gib), "0");
    // Real hierarchy root intentionally has no memory.max/current controller interfaces.
    const auto full = parseLinuxProcessMemorySample("0::/parent/child\n", mount, files.reader());
    expect(full.status == ProcessMemoryStatus::Finite && full.domains.size() == 2 &&
               minimumProcessCacheAllowance(full, 0) == 7 * gib / 8,
           "expected true hierarchy-root absence does not prevent valid constrained sampling");
    files.domain("/leaf", "max", "0");
    const auto alternate = parseLinuxProcessMemorySample(
        "0::/parent/child\n",
        std::string(mount) + "38 25 0:32 /parent/child /leaf rw - cgroup2 cgroup rw\n",
        files.reader());
    expect(alternate.status == ProcessMemoryStatus::Finite &&
               minimumProcessCacheAllowance(alternate, 0) == 7 * gib / 8 &&
               alternate.domains.front().path == "/sys/fs/cgroup/parent/child",
           "less restricted visible mount exposes finite parent hidden by specific bind mount");
    const auto inaccessible = parseLinuxProcessMemorySample(
        "0::/parent/child\n", mount, [&files](const std::filesystem::path& path) {
            return path.parent_path() == "/sys/fs/cgroup" ? ProcessMemoryFileRead{}
                                                          : files.reader()(path);
        });
    expect(inaccessible.status == ProcessMemoryStatus::Partial,
           "inaccessible root cannot masquerade as expected missing interfaces");
    const auto constrainedMissing = parseLinuxProcessMemorySample(
        "0::/parent\n", "36 25 0:32 /parent /constrained rw - cgroup2 cgroup rw\n", files.reader());
    expect(constrainedMissing.status == ProcessMemoryStatus::Partial &&
               constrainedMissing.domains.size() == 1,
           "missing constrained bind root remains incomplete");
    const auto hybrid = parseLinuxProcessMemorySample(
        "0::/scope\n5:memory:/legacy/scope\n",
        std::string(mount) + "38 25 0:33 / /legacy-memory rw - cgroup cgroup rw,memory\n",
        files.reader());
    expect(hybrid.status == ProcessMemoryStatus::Unsupported &&
               hybrid.source == ProcessMemorySource::LinuxCgroupV1Unsupported,
           "actual v1 memory membership wins over generic hybrid unified membership");
    files.domain("/sys/fs/cgroup", "1024", "0");
    const auto namespaceRoot = parseLinuxProcessMemorySample("0::/\n", mount, files.reader());
    expect(namespaceRoot.status == ProcessMemoryStatus::Finite && namespaceRoot.domains.size() == 1,
           "proven visible namespace root with real controller files samples exactly once");
}

void failuresAndBounds() {
    Files files;
    files.domain("/sys/fs/cgroup/leaf", "0", "0");
    files.domain("/sys/fs/cgroup", "max", "0");
    const auto zero = parseLinuxProcessMemorySample("0::/leaf\n", mount, files.reader());
    expect(zero.status == ProcessMemoryStatus::Finite && minimumProcessCacheAllowance(zero, 0) == 0,
           "measured finite zero and zero usage are preserved");
    for (const auto* const invalid : {"-1", "18446744073709551616", "128 garbage", ""}) {
        files.values["/sys/fs/cgroup/leaf/memory.max"] = invalid;
        expect(parseLinuxProcessMemorySample("0::/leaf\n", mount, files.reader()).status ==
                   ProcessMemoryStatus::Partial,
               "negative/overflow/malformed/empty limit stays incomplete");
    }
    files.values["/sys/fs/cgroup/leaf/memory.max"] = "1024";
    files.values.erase("/sys/fs/cgroup/leaf/memory.current");
    expect(parseLinuxProcessMemorySample("0::/leaf\n", mount, files.reader()).status ==
               ProcessMemoryStatus::Partial,
           "finite limit without paired usage cannot authorize a complete sample");
    files.values["/sys/fs/cgroup/leaf/memory.max"] = "0";
    const auto zeroUnknownUsage =
        parseLinuxProcessMemorySample("0::/leaf\n", mount, files.reader());
    expect(zeroUnknownUsage.status == ProcessMemoryStatus::Partial &&
               minimumProcessCacheAllowance(zeroUnknownUsage, 0) == 0,
           "reliably measured finite zero clamps even with unread usage");
    files.values["/sys/fs/cgroup/leaf/memory.max"] = std::to_string(256 * 1024 * 1024);
    const auto loweredUnknownUsage =
        parseLinuxProcessMemorySample("0::/leaf\n", mount, files.reader());
    expect(loweredUnknownUsage.status == ProcessMemoryStatus::Partial &&
               minimumProcessCacheAllowance(loweredUnknownUsage, 0) == 224 * 1024 * 1024,
           "known tighter limit with missing usage still supplies absolute non-expanding ceiling");
    files.values["/sys/fs/cgroup/leaf/memory.current"] = std::string(4097, '1');
    expect(parseLinuxProcessMemorySample("0::/leaf\n", mount, files.reader()).status ==
               ProcessMemoryStatus::Partial,
           "oversized scalar is explicitly incomplete");
    std::string deep;
    for (std::size_t index = 0; index < kMaximumProcessMemoryDomains + 1; ++index)
        deep += "/a";
    const auto truncated = parseLinuxProcessMemorySample(
        "0::" + deep + "\n", mount, [](const std::filesystem::path& path) {
            return ProcessMemoryFileRead{ProcessMemoryReadStatus::Value,
                                         path.filename() == "memory.max" ? "max" : "0"};
        });
    expect(truncated.status == ProcessMemoryStatus::Partial &&
               truncated.domains.size() == kMaximumProcessMemoryDomains,
           "ancestor bound is partial rather than an invented no-limit sample");
    expect(parseLinuxProcessMemorySample("0::/leaf\n", std::string(1024 * 1024 + 1, 'x'),
                                         files.reader())
                   .status == ProcessMemoryStatus::Unavailable,
           "oversized topology input is bounded and unavailable");
}
#endif

ProcessMemorySample proofSample() {
    ProcessMemorySample sample;
    sample.source = ProcessMemorySource::LinuxCgroupV2;
    sample.status = ProcessMemoryStatus::Finite;
    sample.membershipPath = "/leaf";
    sample.view = ProcessMemoryView{.mountRoot = "/",
                                    .mountPoint = "/cgroup",
                                    .filesystemDevice = "0:32",
                                    .mountId = 36,
                                    .namespaces = {.cgroup = ProcessMemoryNamespaceIdentity{1, 2},
                                                   .mount = ProcessMemoryNamespaceIdentity{1, 3}}};
    sample.domains = {{"/cgroup/leaf", ProcessMemoryLimitKind::Finite, gib, 0},
                      {"/cgroup", ProcessMemoryLimitKind::Unlimited, 0, 0}};
    return sample;
}
ProcessMemoryView& checkedView(ProcessMemorySample& sample) {
    if (!sample.view)
        throw std::runtime_error("fixture view missing");
    return *sample.view;
}
void constraintRemovalProof() {
    const auto finite = proofSample();
    ProcessMemoryConstraintHistory history;
    history.recordFinite(finite, true);
    auto unlimited = finite;
    unlimited.status = ProcessMemoryStatus::NoFiniteVisibleLimit;
    unlimited.domains[0].kind = ProcessMemoryLimitKind::Unlimited;
    unlimited.domains[0].limitBytes = 0;
    expect(history.verifiesRemoval(unlimited),
           "same verified view explicitly removes every observed finite constraint");
    auto outside = finite;
    outside.domains.push_back({"/elsewhere", ProcessMemoryLimitKind::Finite, 0, 0});
    ProcessMemoryConstraintHistory outsideHistory;
    outsideHistory.recordFinite(finite, true);
    outsideHistory.recordFinite(outside, true);
    expect(!outsideHistory.verifiesRemoval(unlimited),
           "out-of-view finite constraint invalidates otherwise-valid old removal proof");
    ProcessMemoryConstraintHistory empty;
    expect(!empty.verifiesRemoval(unlimited), "empty history cannot prove removal");
    const auto rejects = [&](const ProcessMemorySample& candidate, const std::string_view reason) {
        expect(!history.verifiesRemoval(candidate), reason);
    };
    auto changed = unlimited;
    checkedView(changed).namespaces.cgroup = ProcessMemoryNamespaceIdentity{1, 4};
    rejects(changed, "same paths in a different cgroup namespace do not prove removal");
    changed = unlimited;
    checkedView(changed).namespaces.mount = ProcessMemoryNamespaceIdentity{1, 4};
    rejects(changed, "same paths in a different mount namespace do not prove removal");
    changed = unlimited;
    checkedView(changed).namespaces.mount.reset();
    rejects(changed, "unverified namespace identity cannot prove removal");
    changed = unlimited;
    checkedView(changed).mountId += 1;
    rejects(changed, "replaced mount cannot prove removal");
    changed = unlimited;
    checkedView(changed).filesystemDevice = "0:33";
    rejects(changed, "changed filesystem identity cannot prove removal");
    changed = unlimited;
    changed.membershipPath = "/other";
    rejects(changed, "membership migration cannot prove removal");
    changed = unlimited;
    changed.domains.erase(changed.domains.begin());
    rejects(changed, "missing old finite domain cannot prove removal");
    changed = unlimited;
    changed.domains[0].currentBytes.reset();
    rejects(changed, "missing current read cannot prove removal despite unlimited label");
    changed = unlimited;
    changed.domains[0].kind = ProcessMemoryLimitKind::Unknown;
    rejects(changed, "unknown domain cannot impersonate complete no-finite data");
    changed = unlimited;
    changed.domains[0].kind = ProcessMemoryLimitKind::Finite;
    rejects(changed, "finite domain under no-finite label cannot prove removal");
    changed = unlimited;
    changed.domains.push_back(changed.domains[0]);
    rejects(changed, "duplicate domain is malformed, not a complete proof");
    changed = unlimited;
    changed.domains.clear();
    rejects(changed, "empty domain list cannot prove removal");
    changed = unlimited;
    changed.status = ProcessMemoryStatus::Partial;
    rejects(changed, "partial probe cannot prove removal");
    changed = unlimited;
    changed.domains[0].path = "/cgroup/../leaf";
    rejects(changed, "malformed domain path cannot prove removal");
    auto ancestor = finite;
    ancestor.domains[1].kind = ProcessMemoryLimitKind::Finite;
    ancestor.domains[1].limitBytes = gib;
    history.recordFinite(ancestor, true);
    history.recordFinite(finite, true);
    changed = unlimited;
    changed.domains.pop_back();
    rejects(changed, "later finite snapshot cannot forget observed finite ancestor");
    history.recordFinite(ancestor, true);
    changed = unlimited;
    changed.domains[1].currentBytes = 0;
    expect(history.verifiesRemoval(changed), "all previously finite ancestors explicitly max");
    auto malformed = finite;
    malformed.domains[0].currentBytes.reset();
    history.recordFinite(malformed, true);
    expect(history.verifiesRemoval(changed),
           "malformed finite replacement does not erase previous matched history");
    changed.domains.pop_back();
    rejects(changed, "malformed replacement cannot forget previously finite ancestor");
}

void arithmeticAndPlatformFallback() {
    expect(processCacheAllowanceForDomain(8 * gib, 6 * gib, gib, gib) == 2 * gib &&
               processCacheAllowanceForDomain(8 * gib, 6 * gib, 0, gib) == gib,
           "paired-domain formula credits exclusive host bytes once");
    expect(processCacheAllowanceForDomain(100, 100, 0, 1) == 0 &&
               processCacheAllowanceForDomain(0, 0, 0, 1) == 0 &&
               processCacheAllowanceForDomain(100, 0, 0, 101) == 0 &&
               reserveForProcessMemoryLimit(0) == std::size_t{16} * 1024 * 1024 &&
               reserveForProcessMemoryLimit(std::numeric_limits<std::size_t>::max()) == gib,
           "tiny/zero/over-limit reserves saturate without floor or integer wrap");
#if !defined(__linux__)
    const auto sample = sampleProcessMemory();
    expect(sample.status == ProcessMemoryStatus::Unsupported && sample.domains.empty(),
           "native fallback explicitly reports unavailable process-limit protection");
#if defined(_WIN32)
    expect(sample.source == ProcessMemorySource::WindowsHostOnly, "Windows host-only provenance");
#elif defined(__APPLE__)
    expect(sample.source == ProcessMemorySource::MacOsHostOnly, "macOS host-only provenance");
#else
    expect(sample.source == ProcessMemorySource::OtherHostOnly, "other host-only provenance");
#endif
#endif
}

} // namespace

int main() try {
#if defined(__linux__)
    pairedHierarchy();
    pathsAndProvenance();
    rootAndAlternateMounts();
    failuresAndBounds();
#endif
    constraintRemovalProof();
    arithmeticAndPlatformFallback();
    return failures == 0 ? 0 : 1;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
