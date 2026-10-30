#include <bloom/runtime/process_memory_sample.hpp>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <limits>
#include <utility>

#if defined(__linux__)
#include <sys/stat.h>
#endif

namespace bloom::runtime {
namespace {
constexpr std::size_t kMaximumMountInfoBytes = std::size_t{1024} * 1024U;
constexpr std::size_t kMaximumMembershipBytes = std::size_t{64} * 1024U;
constexpr std::size_t kMaximumPathBytes = 4096;

std::string_view trim(std::string_view value) noexcept {
    while (!value.empty() &&
           (value.front() == ' ' || value.front() == '\t' || value.front() == '\n'))
        value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\n'))
        value.remove_suffix(1);
    return value;
}

std::optional<std::size_t> bytes(const std::string_view value) noexcept {
    const auto text = trim(value);
    std::uint64_t parsed = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (text.empty() || result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
        parsed > std::numeric_limits<std::size_t>::max())
        return std::nullopt;
    return static_cast<std::size_t>(parsed);
}

std::optional<std::filesystem::path> decodePath(const std::string_view encoded) {
    if (encoded.empty() || encoded.size() > kMaximumPathBytes)
        return std::nullopt;
    std::string decoded;
    decoded.reserve(encoded.size());
    for (std::size_t index = 0; index < encoded.size(); ++index) {
        if (encoded[index] != '\\') {
            decoded.push_back(encoded[index]);
            continue;
        }
        if (encoded.size() - index < 4)
            return std::nullopt;
        unsigned octal = 0;
        for (std::size_t offset = 1; offset <= 3; ++offset) {
            const auto digit = encoded[index + offset];
            if (digit < '0' || digit > '7')
                return std::nullopt;
            octal = octal * 8U + static_cast<unsigned>(digit - '0');
        }
        if (octal == 0 || octal > 255)
            return std::nullopt;
        decoded.push_back(static_cast<char>(octal));
        index += 3;
    }
    std::filesystem::path path(decoded);
    if (!path.is_absolute() || path.lexically_normal() != path)
        return std::nullopt;
    return path;
}

bool containsController(std::string_view list, const std::string_view wanted) noexcept {
    while (!list.empty()) {
        const auto end = list.find(',');
        if (list.substr(0, end) == wanted)
            return true;
        if (end == std::string_view::npos)
            return false;
        list.remove_prefix(end + 1);
    }
    return false;
}

std::vector<std::string_view> fields(std::string_view line) {
    std::vector<std::string_view> result;
    while (!(line = trim(line)).empty()) {
        const auto end = line.find_first_of(" \t");
        result.push_back(line.substr(0, end));
        if (result.size() > 64)
            return {};
        if (end == std::string_view::npos)
            break;
        line.remove_prefix(end + 1);
    }
    return result;
}

bool below(const std::filesystem::path& path, const std::filesystem::path& root) {
    auto part = path.begin();
    for (const auto& rootPart : root) {
        if (part == path.end() || *part != rootPart)
            return false;
        ++part;
    }
    return true;
}

#if defined(__linux__)
std::optional<ProcessMemoryNamespaceIdentity> namespaceIdentity(const char* path) noexcept {
    struct stat identity{};
    if (::stat(path, &identity) != 0 || identity.st_ino == 0)
        return {};
    return ProcessMemoryNamespaceIdentity{.device = static_cast<std::uint64_t>(identity.st_dev),
                                          .inode = static_cast<std::uint64_t>(identity.st_ino)};
}
ProcessMemoryNamespaces namespaceIdentities() noexcept {
    return {.cgroup = namespaceIdentity("/proc/self/ns/cgroup"),
            .mount = namespaceIdentity("/proc/self/ns/mnt")};
}
ProcessMemoryFileRead readBounded(const std::filesystem::path& path, const std::size_t limit) {
    errno = 0;
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {errno == ENOENT ? ProcessMemoryReadStatus::Missing
                                : ProcessMemoryReadStatus::Unavailable,
                {}};
    std::string value(limit + 1, '\0');
    input.read(value.data(), static_cast<std::streamsize>(value.size()));
    const auto read = input.gcount();
    if (input.bad() || read < 0)
        return {};
    if (static_cast<std::size_t>(read) > limit)
        return {ProcessMemoryReadStatus::TooLarge, {}};
    value.resize(static_cast<std::size_t>(read));
    return {ProcessMemoryReadStatus::Value, std::move(value)};
}
#endif
} // namespace

ProcessMemorySample
parseLinuxProcessMemorySample(const std::string_view cgroupText,
                              const std::string_view mountInfoText,
                              const ProcessMemoryFileReader& readScalar,
                              const std::chrono::steady_clock::time_point sampledAt,
                              ProcessMemoryNamespaces verifiedNamespaces) noexcept {
    ProcessMemorySample sample;
    sample.sampledAt = sampledAt;
    try {
        if (!readScalar || cgroupText.size() > kMaximumMembershipBytes ||
            mountInfoText.size() > kMaximumMountInfoBytes)
            return sample;
        std::optional<std::filesystem::path> membership;
        std::optional<std::filesystem::path> v1Membership;
        auto remaining = cgroupText;
        while (!remaining.empty()) {
            const auto end = remaining.find('\n');
            const auto line = remaining.substr(0, end);
            const auto first = line.find(':');
            const auto second = first == std::string_view::npos ? first : line.find(':', first + 1);
            if (second != std::string_view::npos) {
                const auto controller = line.substr(first + 1, second - first - 1);
                if (line.substr(0, first) == "0" && controller.empty())
                    membership = decodePath(line.substr(second + 1));
                else if (containsController(controller, "memory"))
                    v1Membership = decodePath(line.substr(second + 1));
            }
            if (end == std::string_view::npos)
                break;
            remaining.remove_prefix(end + 1);
        }
        std::optional<std::filesystem::path> mount;
        std::optional<std::filesystem::path> mountRoot;
        std::optional<std::filesystem::path> leaf;
        bool v1Mount = false;
        std::string selectedDevice;
        std::uint64_t selectedMountId = 0;
        remaining = mountInfoText;
        while (!remaining.empty()) {
            const auto end = remaining.find('\n');
            const auto parts = fields(remaining.substr(0, end));
            const auto separator = std::ranges::find(parts, std::string_view{"-"});
            if (parts.size() >= 10 && separator != parts.end() && parts.end() - separator >= 4) {
                const auto root = decodePath(parts[3]);
                const auto point = decodePath(parts[4]);
                if (root && point && separator[1] == "cgroup" && v1Membership &&
                    below(*v1Membership, *root) && containsController(separator[3], "memory"))
                    v1Mount = true;
                if (root && point && separator[1] == "cgroup2" && membership) {
                    if (below(*membership, *root)) {
                        const auto relative = membership->lexically_relative(*root);
                        const auto candidate =
                            relative == "." ? *point : (*point / relative).lexically_normal();
                        if (below(candidate, *point) &&
                            (!mountRoot || root->native().size() < mountRoot->native().size())) {
                            mount = *point;
                            mountRoot = *root;
                            leaf = candidate;
                            selectedDevice = std::string(parts[2]);
                            const auto id = bytes(parts[0]);
                            selectedMountId = id.value_or(0);
                        }
                    }
                }
            }
            if (end == std::string_view::npos)
                break;
            remaining.remove_prefix(end + 1);
        }
        // A generic unified membership does not prove v2 owns memory on hybrid hosts.
        if (v1Membership && v1Mount) {
            sample.source = ProcessMemorySource::LinuxCgroupV1Unsupported;
            sample.status = ProcessMemoryStatus::Unsupported;
            sample.membershipPath = v1Membership->generic_string();
            return sample;
        }
        if (!mount || !leaf || !membership)
            return sample;
        sample.source = ProcessMemorySource::LinuxCgroupV2;
        sample.membershipPath = membership->generic_string();
        sample.view = ProcessMemoryView{.mountRoot = mountRoot->generic_string(),
                                        .mountPoint = mount->generic_string(),
                                        .filesystemDevice = std::move(selectedDevice),
                                        .mountId = selectedMountId,
                                        .namespaces = verifiedNamespaces};
        bool complete = true;
        bool finite = false;
        auto path = *leaf;
        for (;;) {
            if (sample.domains.size() == kMaximumProcessMemoryDomains) {
                complete = false;
                break;
            }
            ProcessMemoryDomain domain;
            domain.path = path.generic_string();
            const auto maximum = readScalar(path / "memory.max");
            const auto current = readScalar(path / "memory.current");
            // Only a proven true hierarchy root lacks these non-root controller interfaces.
            // Constrained/bind roots and inaccessible files stay incomplete.
            if (path == *mount && *mountRoot == "/" &&
                maximum.status == ProcessMemoryReadStatus::Missing &&
                current.status == ProcessMemoryReadStatus::Missing) {
                sample.rootInterfacesAbsent = true;
                break;
            }
            if (maximum.status == ProcessMemoryReadStatus::Value &&
                maximum.text.size() <= kMaximumProcessMemoryScalarBytes) {
                if (trim(maximum.text) == "max")
                    domain.kind = ProcessMemoryLimitKind::Unlimited;
                else if (const auto value = bytes(maximum.text); value) {
                    domain.kind = ProcessMemoryLimitKind::Finite;
                    domain.limitBytes = *value;
                    finite = true;
                }
            }
            if (current.status == ProcessMemoryReadStatus::Value &&
                current.text.size() <= kMaximumProcessMemoryScalarBytes)
                domain.currentBytes = bytes(current.text);
            std::optional<std::size_t> reclaimable;
            if (domain.currentBytes) {
                const auto stat = readScalar(path / "memory.stat");
                if (stat.status == ProcessMemoryReadStatus::Value &&
                    stat.text.size() <= kMaximumProcessMemoryStatBytes) {
                    if (const auto parsed = parseCgroupReclaimableBytes(stat.text))
                        reclaimable = std::min(*parsed, *domain.currentBytes);
                }
            }
            sample.reclaimableBytes.push_back(reclaimable);
            complete = complete && domain.kind != ProcessMemoryLimitKind::Unknown &&
                       domain.currentBytes.has_value();
            sample.domains.push_back(std::move(domain));
            if (path == *mount)
                break;
            const auto parent = path.parent_path();
            if (parent == path || !below(parent, *mount)) {
                complete = false;
                break;
            }
            path = parent;
        }
        sample.status = !complete ? ProcessMemoryStatus::Partial
                        : finite  ? ProcessMemoryStatus::Finite
                                  : ProcessMemoryStatus::NoFiniteVisibleLimit;
    } catch (...) {
        sample.status = sample.domains.empty() ? ProcessMemoryStatus::Unavailable
                                               : ProcessMemoryStatus::Partial;
    }
    return sample;
}

ProcessMemorySample sampleProcessMemory(CancellationToken cancellation) noexcept {
    ProcessMemorySample sample;
    sample.sampledAt = std::chrono::steady_clock::now();
#if defined(__linux__)
    try {
        if (cancellation.isCancellationRequested())
            return sample;
        const auto before = namespaceIdentities();
        const auto membership = readBounded("/proc/self/cgroup", kMaximumMembershipBytes);
        const auto mounts = readBounded("/proc/self/mountinfo", kMaximumMountInfoBytes);
        if (membership.status != ProcessMemoryReadStatus::Value ||
            mounts.status != ProcessMemoryReadStatus::Value)
            return sample;
        auto parsed = parseLinuxProcessMemorySample(
            membership.text, mounts.text,
            [&cancellation](const std::filesystem::path& path) {
                return cancellation.isCancellationRequested()
                           ? ProcessMemoryFileRead{}
                           : readBounded(path, path.filename() == "memory.stat"
                                                   ? kMaximumProcessMemoryStatBytes
                                                   : kMaximumProcessMemoryScalarBytes);
            },
            sample.sampledAt, before);
        const auto membershipAfter = readBounded("/proc/self/cgroup", kMaximumMembershipBytes);
        const auto mountsAfter = readBounded("/proc/self/mountinfo", kMaximumMountInfoBytes);
        const auto after = namespaceIdentities();
        const bool changed =
            before != after || membershipAfter.status != ProcessMemoryReadStatus::Value ||
            mountsAfter.status != ProcessMemoryReadStatus::Value ||
            membershipAfter.text != membership.text || mountsAfter.text != mounts.text;
        if (changed)
            parsed.status = ProcessMemoryStatus::Partial;
        if (changed || !before.cgroup || !before.mount) {
            if (parsed.view)
                parsed.view->namespaces = {};
        }
        return parsed;
    } catch (...) {
        return sample;
    }
#elif defined(_WIN32)
    (void)cancellation;
    sample.source = ProcessMemorySource::WindowsHostOnly;
    sample.status = ProcessMemoryStatus::Unsupported;
#elif defined(__APPLE__)
    (void)cancellation;
    sample.source = ProcessMemorySource::MacOsHostOnly;
    sample.status = ProcessMemoryStatus::Unsupported;
#else
    (void)cancellation;
    sample.source = ProcessMemorySource::OtherHostOnly;
    sample.status = ProcessMemoryStatus::Unsupported;
#endif
    return sample;
}

namespace {
bool boundedPosixPath(const std::string_view path) noexcept {
    if (path.empty() || path.front() != '/' || path.size() > kMaximumPathBytes)
        return false;
    if (path == "/")
        return true;
    auto remaining = path.substr(1);
    while (!remaining.empty()) {
        const auto end = remaining.find('/');
        const auto part = remaining.substr(0, end);
        if (part.empty() || part == "." || part == ".." ||
            part.find('\0') != std::string_view::npos)
            return false;
        if (end == std::string_view::npos)
            return true;
        remaining.remove_prefix(end + 1);
    }
    return false;
}
bool trustedView(const ProcessMemorySample& sample) noexcept {
    if (!sample.view || !boundedPosixPath(sample.membershipPath))
        return false;
    const auto& view = *sample.view;
    const auto separator = view.filesystemDevice.find(':');
    return boundedPosixPath(view.mountRoot) && boundedPosixPath(view.mountPoint) &&
           view.mountId != 0 && view.filesystemDevice.size() <= 64 &&
           separator != std::string::npos &&
           bytes(std::string_view(view.filesystemDevice).substr(0, separator)).has_value() &&
           bytes(std::string_view(view.filesystemDevice).substr(separator + 1)).has_value() &&
           view.namespaces.cgroup && view.namespaces.cgroup->inode != 0 && view.namespaces.mount &&
           view.namespaces.mount->inode != 0;
}
bool domainInView(const std::string_view path, const ProcessMemoryView& view) noexcept {
    return path == view.mountPoint ||
           (path.starts_with(view.mountPoint) &&
            (view.mountPoint == "/" ||
             (path.size() > view.mountPoint.size() && path[view.mountPoint.size()] == '/')));
}
} // namespace

bool isCompleteProcessMemorySample(const ProcessMemorySample& sample) noexcept {
    if (sample.source != ProcessMemorySource::LinuxCgroupV2 ||
        sample.domains.size() > kMaximumProcessMemoryDomains ||
        (sample.status != ProcessMemoryStatus::Finite &&
         sample.status != ProcessMemoryStatus::NoFiniteVisibleLimit))
        return false;
    if (sample.domains.empty())
        return sample.status == ProcessMemoryStatus::NoFiniteVisibleLimit &&
               sample.rootInterfacesAbsent && sample.view && trustedView(sample) &&
               sample.membershipPath == "/" && sample.view->mountRoot == "/";
    bool finite = false;
    for (std::size_t index = 0; index < sample.domains.size(); ++index) {
        const auto& domain = sample.domains[index];
        if (!boundedPosixPath(domain.path) || !domain.currentBytes ||
            (domain.kind != ProcessMemoryLimitKind::Finite &&
             domain.kind != ProcessMemoryLimitKind::Unlimited))
            return false;
        for (std::size_t previous = 0; previous < index; ++previous)
            if (sample.domains[previous].path == domain.path)
                return false;
        finite = finite || domain.kind == ProcessMemoryLimitKind::Finite;
    }
    return finite == (sample.status == ProcessMemoryStatus::Finite);
}

void ProcessMemoryConstraintHistory::recordFinite(const ProcessMemorySample& sample,
                                                  const bool replaceComplete) noexcept {
    const bool hasFinite = std::ranges::any_of(sample.domains, [](const auto& domain) {
        return domain.kind == ProcessMemoryLimitKind::Finite;
    });
    if (!hasFinite)
        return;
    try {
        if (!sample.view || !trustedView(sample) ||
            sample.domains.size() > kMaximumProcessMemoryDomains) {
            valid_ = false;
            return;
        }
        if (replaceComplete && isCompleteProcessMemorySample(sample)) {
            // Never forget a previously finite domain within the same view merely because a
            // later finite snapshot omits it. A removal proof must still read that old path.
            std::vector<std::string> paths =
                view_ == sample.view && membership_ == sample.membershipPath
                    ? finitePaths_
                    : std::vector<std::string>{};
            for (const auto& domain : sample.domains) {
                if (!domainInView(domain.path, *sample.view)) {
                    valid_ = false;
                    return;
                }
                if (domain.kind == ProcessMemoryLimitKind::Finite &&
                    std::ranges::find(paths, domain.path) == paths.end()) {
                    if (paths.size() == kMaximumProcessMemoryDomains) {
                        valid_ = false;
                        return;
                    }
                    paths.push_back(domain.path);
                }
            }
            auto view = sample.view;
            auto membership = sample.membershipPath;
            finitePaths_.swap(paths);
            view_.swap(view);
            membership_.swap(membership);
            valid_ = true;
            return;
        }
        if (!valid_ || view_ != sample.view || membership_ != sample.membershipPath) {
            valid_ = false;
            return;
        }
        for (const auto& domain : sample.domains) {
            if (domain.kind != ProcessMemoryLimitKind::Finite)
                continue;
            if (!boundedPosixPath(domain.path) || !domainInView(domain.path, *sample.view)) {
                valid_ = false;
                return;
            }
            if (std::ranges::find(finitePaths_, domain.path) == finitePaths_.end()) {
                if (finitePaths_.size() == kMaximumProcessMemoryDomains) {
                    valid_ = false;
                    return;
                }
                finitePaths_.push_back(domain.path);
            }
        }
    } catch (...) {
        valid_ = false;
    }
}

bool ProcessMemoryConstraintHistory::verifiesRemoval(
    const ProcessMemorySample& sample) const noexcept {
    if (!valid_ || finitePaths_.empty() || !sample.view || !trustedView(sample) ||
        sample.status != ProcessMemoryStatus::NoFiniteVisibleLimit ||
        !isCompleteProcessMemorySample(sample) || view_ != sample.view ||
        membership_ != sample.membershipPath)
        return false;
    for (const auto& domain : sample.domains)
        if (!domainInView(domain.path, *sample.view))
            return false;
    for (const auto& path : finitePaths_) {
        const auto domain = std::ranges::find(sample.domains, path, &ProcessMemoryDomain::path);
        if (domain == sample.domains.end() || domain->kind != ProcessMemoryLimitKind::Unlimited ||
            !domain->currentBytes)
            return false;
    }
    return true;
}

std::optional<std::size_t> parseCgroupReclaimableBytes(const std::string_view text) noexcept {
    std::optional<std::size_t> activeFile, inactiveFile, slabReclaimable, dirty, writeback;
    auto remaining = text;
    while (!remaining.empty()) {
        const auto end = remaining.find('\n');
        const auto line = remaining.substr(0, end);
        const auto space = line.find(' ');
        if (space != std::string_view::npos) {
            const auto key = line.substr(0, space);
            const auto value = bytes(line.substr(space + 1));
            if (key == "active_file")
                activeFile = value;
            else if (key == "inactive_file")
                inactiveFile = value;
            else if (key == "slab_reclaimable")
                slabReclaimable = value;
            else if (key == "file_dirty")
                dirty = value;
            else if (key == "file_writeback")
                writeback = value;
        }
        if (end == std::string_view::npos)
            break;
        remaining.remove_prefix(end + 1);
    }
    if (!activeFile || !inactiveFile)
        return std::nullopt;
    const auto saturatingAdd = [](const std::size_t left, const std::size_t right) {
        return right > std::numeric_limits<std::size_t>::max() - left
                   ? std::numeric_limits<std::size_t>::max()
                   : left + right;
    };
    // Dirty and writeback pages must reach the disk before they can be freed: no credit.
    const auto cache =
        saturatingAdd(saturatingAdd(*activeFile, *inactiveFile), slabReclaimable.value_or(0));
    const auto pinned = saturatingAdd(dirty.value_or(0), writeback.value_or(0));
    return cache - std::min(cache, pinned);
}

std::size_t processMemoryDomainUnreclaimableBytes(const ProcessMemorySample& sample,
                                                  const std::size_t index) noexcept {
    if (index >= sample.domains.size())
        return 0;
    const auto current = sample.domains[index].currentBytes.value_or(0);
    const auto reclaimable =
        index < sample.reclaimableBytes.size() ? sample.reclaimableBytes[index].value_or(0) : 0;
    return current - std::min(current, reclaimable);
}

std::size_t reserveForProcessMemoryLimit(const std::size_t limit) noexcept {
    constexpr std::size_t mib = std::size_t{1024} * 1024U;
    return std::min(1024U * mib, std::max(16U * mib, limit / 8U));
}

std::size_t processCacheAllowanceForDomain(const std::size_t limit, const std::size_t current,
                                           const std::size_t reusableHostBytes,
                                           const std::size_t reserve) noexcept {
    const auto otherUsage = current - std::min(current, reusableHostBytes);
    const auto remaining = limit - std::min(limit, otherUsage);
    return remaining - std::min(remaining, reserve);
}

std::optional<std::size_t>
minimumProcessCacheAllowance(const ProcessMemorySample& sample,
                             const std::size_t reusableHostBytes) noexcept {
    std::optional<std::size_t> cap;
    for (std::size_t index = 0; index < sample.domains.size(); ++index) {
        const auto& domain = sample.domains[index];
        if (domain.kind != ProcessMemoryLimitKind::Finite)
            continue;
        // An unread usage cannot authorize growth, but a known limit remains an absolute ceiling.
        const auto allowance = processCacheAllowanceForDomain(
            domain.limitBytes, processMemoryDomainUnreclaimableBytes(sample, index),
            reusableHostBytes, reserveForProcessMemoryLimit(domain.limitBytes));
        cap = cap ? std::min(*cap, allowance) : allowance;
    }
    return cap;
}
} // namespace bloom::runtime
