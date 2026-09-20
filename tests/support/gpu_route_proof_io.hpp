#pragma once

// Qt-free, header-only TEST helper for the GPU route-proof handoff. It serializes the existing
// typed GpuRouteExecutionProof together with a run nonce and the schema identity, with bounded
// sizes, exact parsing, and atomic publication. It is reachable only through an explicit PRIVATE
// test include directory; product code never performs test file I/O.
//
// This is a handoff codec, not a security framework: there is no signature or trust chain. A
// consumer accepts a proof only when its nonce and schema match the current run and the typed
// validation passes. A missing, stale, malformed, conflicting, or policy-violating proof is
// reported by name and never becomes a pass.

#include <bloom/runtime/gpu_coverage_route_proof_contract.hpp>

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace bloom::gpu_route_proof_io {

inline constexpr std::string_view kSchemaId = "bloom.gpu.route-proof";
inline constexpr std::uint32_t kSchemaVersion = 2;
inline constexpr std::size_t kMaxFileBytes = 8192;
inline constexpr std::size_t kMaxNonceBytes = 64;
inline constexpr std::size_t kMaxTokenBytes = 128;

enum class RouteProofIoStatus : std::uint8_t {
    Ok,
    Missing,
    Malformed,
    StaleNonce,
    SchemaMismatch,
    DuplicateOrConflicting,
    TooLarge,
    IoError,
};

[[nodiscard]] inline std::string_view
routeProofIoStatusName(const RouteProofIoStatus status) noexcept {
    switch (status) {
    case RouteProofIoStatus::Ok:
        return "Ok";
    case RouteProofIoStatus::Missing:
        return "Missing";
    case RouteProofIoStatus::Malformed:
        return "Malformed";
    case RouteProofIoStatus::StaleNonce:
        return "StaleNonce";
    case RouteProofIoStatus::SchemaMismatch:
        return "SchemaMismatch";
    case RouteProofIoStatus::DuplicateOrConflicting:
        return "DuplicateOrConflicting";
    case RouteProofIoStatus::TooLarge:
        return "TooLarge";
    case RouteProofIoStatus::IoError:
        return "IoError";
    }
    return "Unknown";
}

// The six required route ids come from the contract itself, so "remove only the known proof files"
// can never drift from the registry.
[[nodiscard]] inline std::vector<std::string> knownRouteIds() {
    std::vector<std::string> ids;
    for (const auto& route : bloom::runtime::gpuRenderRouteCoverage()) {
        ids.emplace_back(route.id);
    }
    return ids;
}

[[nodiscard]] inline std::filesystem::path
routeProofNoncePath(const std::filesystem::path& directory) {
    return directory / "nonce";
}

[[nodiscard]] inline std::filesystem::path routeProofPath(const std::filesystem::path& directory,
                                                          const std::string_view routeId) {
    return directory / ("route." + std::string{routeId} + ".proof");
}

// A single-line token must be nonempty, within the bound, and free of the record separators.
[[nodiscard]] inline bool validToken(const std::string_view value,
                                     const std::size_t limit) noexcept {
    if (value.empty() || value.size() > limit) {
        return false;
    }
    return value.find('\n') == std::string_view::npos && value.find('\r') == std::string_view::npos;
}

[[nodiscard]] inline std::optional<std::string> readBoundedFile(const std::filesystem::path& path,
                                                                const std::size_t limit,
                                                                RouteProofIoStatus& status) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        status = RouteProofIoStatus::Missing;
        return std::nullopt;
    }
    file.seekg(0, std::ios::end);
    const std::streampos end = file.tellg();
    if (end < std::streampos(0)) {
        status = RouteProofIoStatus::IoError;
        return std::nullopt;
    }
    const auto size = static_cast<std::size_t>(end);
    if (size > limit) {
        status = RouteProofIoStatus::TooLarge;
        return std::nullopt;
    }
    file.seekg(0, std::ios::beg);
    std::string content(size, '\0');
    if (size != 0) {
        file.read(content.data(), static_cast<std::streamsize>(size));
    }
    if (!file) {
        status = RouteProofIoStatus::IoError;
        return std::nullopt;
    }
    status = RouteProofIoStatus::Ok;
    return content;
}

// Atomic publication: write a sibling temp file, flush/close, then rename over the target. Only the
// temp and the exact target path are ever touched; there is no tree delete.
[[nodiscard]] inline RouteProofIoStatus writeBoundedFileAtomic(const std::filesystem::path& path,
                                                               const std::string& content) {
    if (content.size() > kMaxFileBytes) {
        return RouteProofIoStatus::TooLarge;
    }
    std::error_code error;
    const auto temp = std::filesystem::path{path.string() + ".tmp"};
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        if (!file) {
            return RouteProofIoStatus::IoError;
        }
        file.write(content.data(), static_cast<std::streamsize>(content.size()));
        file.close();
        if (!file) {
            std::filesystem::remove(temp, error);
            return RouteProofIoStatus::IoError;
        }
    }
    std::filesystem::rename(temp, path, error);
    if (error) {
        // A pre-existing target (for example on Windows) is replaced once; the temp is removed on
        // any remaining failure.
        std::filesystem::remove(path, error);
        error.clear();
        std::filesystem::rename(temp, path, error);
    }
    if (error) {
        std::filesystem::remove(temp, error);
        return RouteProofIoStatus::IoError;
    }
    return RouteProofIoStatus::Ok;
}

// Removes only the six known proof files. The nonce file, the directory, and every other file are
// left alone.
[[nodiscard]] inline RouteProofIoStatus
clearKnownProofFiles(const std::filesystem::path& directory) {
    std::error_code error;
    for (const auto& routeId : knownRouteIds()) {
        std::filesystem::remove(routeProofPath(directory, routeId), error);
        error.clear();
    }
    return RouteProofIoStatus::Ok;
}

[[nodiscard]] inline std::string serializeNonce(const std::string_view nonce) {
    std::string content;
    content += "schema=";
    content += kSchemaId;
    content += "\nversion=";
    content += std::to_string(kSchemaVersion);
    content += "\nnonce=";
    content += nonce;
    content += "\n";
    return content;
}

[[nodiscard]] inline RouteProofIoStatus writeRunNonce(const std::filesystem::path& directory,
                                                      const std::string_view nonce) {
    if (!validToken(nonce, kMaxNonceBytes)) {
        return RouteProofIoStatus::Malformed;
    }
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    return writeBoundedFileAtomic(routeProofNoncePath(directory), serializeNonce(nonce));
}

namespace detail {

[[nodiscard]] inline std::optional<std::pair<std::string, std::string>>
splitRecord(const std::string_view line) {
    const auto equals = line.find('=');
    if (equals == std::string_view::npos || equals == 0 || equals + 1 > line.size()) {
        return std::nullopt;
    }
    return std::make_pair(std::string{line.substr(0, equals)},
                          std::string{line.substr(equals + 1)});
}

[[nodiscard]] inline std::optional<std::uint64_t> parseUnsigned(const std::string_view value) {
    if (value.empty()) {
        return std::nullopt;
    }
    std::uint64_t parsed = 0;
    const auto* const begin = value.data();
    const auto* const end = value.data() + value.size();
    const auto result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc{} || result.ptr != end) {
        return std::nullopt;
    }
    return parsed;
}

struct ParsedRecords final {
    std::vector<std::pair<std::string, std::string>> records;
    bool duplicate = false;
    bool malformed = false;
};

[[nodiscard]] inline ParsedRecords parseRecords(const std::string_view content) {
    ParsedRecords parsed;
    std::size_t start = 0;
    while (start < content.size()) {
        auto end = content.find('\n', start);
        if (end == std::string_view::npos) {
            end = content.size();
        }
        auto line = content.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (!line.empty()) {
            const auto record = splitRecord(line);
            if (!record.has_value()) {
                parsed.malformed = true;
                return parsed;
            }
            for (const auto& existing : parsed.records) {
                if (existing.first == record->first) {
                    parsed.duplicate = true;
                    return parsed;
                }
            }
            parsed.records.push_back(*record);
        }
        start = end + 1;
    }
    return parsed;
}

[[nodiscard]] inline const std::string*
findRecord(const std::vector<std::pair<std::string, std::string>>& records,
           const std::string_view key) {
    for (const auto& record : records) {
        if (record.first == key) {
            return &record.second;
        }
    }
    return nullptr;
}

} // namespace detail

[[nodiscard]] inline RouteProofIoStatus readRunNonce(const std::filesystem::path& directory,
                                                     std::string& nonce) {
    RouteProofIoStatus status = RouteProofIoStatus::Ok;
    const auto content = readBoundedFile(routeProofNoncePath(directory), kMaxFileBytes, status);
    if (!content.has_value()) {
        return status;
    }
    const auto parsed = detail::parseRecords(*content);
    if (parsed.malformed) {
        return RouteProofIoStatus::Malformed;
    }
    if (parsed.duplicate) {
        return RouteProofIoStatus::DuplicateOrConflicting;
    }
    const auto* const schema = detail::findRecord(parsed.records, "schema");
    const auto* const version = detail::findRecord(parsed.records, "version");
    const auto* const value = detail::findRecord(parsed.records, "nonce");
    if (schema == nullptr || version == nullptr || value == nullptr || parsed.records.size() != 3) {
        return RouteProofIoStatus::Malformed;
    }
    if (*schema != kSchemaId) {
        return RouteProofIoStatus::SchemaMismatch;
    }
    const auto parsedVersion = detail::parseUnsigned(*version);
    if (!parsedVersion.has_value() || *parsedVersion != kSchemaVersion) {
        return RouteProofIoStatus::SchemaMismatch;
    }
    if (!validToken(*value, kMaxNonceBytes)) {
        return RouteProofIoStatus::Malformed;
    }
    nonce = *value;
    return RouteProofIoStatus::Ok;
}

[[nodiscard]] inline std::string
serializeRouteProof(const std::string_view nonce,
                    const bloom::runtime::GpuRouteExecutionProof& proof) {
    std::string content;
    const auto append = [&content](const std::string_view key, const std::string& value) {
        content += key;
        content += '=';
        content += value;
        content += '\n';
    };
    append("schema", std::string{kSchemaId});
    append("version", std::to_string(kSchemaVersion));
    append("nonce", std::string{nonce});
    append("route", proof.routeId);
    append("harness", std::to_string(static_cast<std::uint64_t>(proof.harness)));
    append("process_identity", proof.processIdentityDigest);
    append("device_epoch", proof.deviceOwnershipEpoch);
    append("native_dispatches", std::to_string(proof.nativeDispatches));
    append("verified_frames", std::to_string(proof.verifiedFrames));
    append("readback_submissions", std::to_string(proof.readbackSubmissions));
    append("payloads", std::to_string(proof.payloads));
    append("transferred_bytes", std::to_string(proof.transferredBytes));
    append("evidence", proof.evidenceDigest);
    return content;
}

// Writes one proof atomically. The typed validation runs first, so a harness cannot publish a
// zero-native, no-frame, or over-policy proof even by accident.
[[nodiscard]] inline RouteProofIoStatus
writeRouteProof(const std::filesystem::path& directory, const std::string_view nonce,
                const bloom::runtime::GpuRouteExecutionProof& proof) {
    if (!validToken(nonce, kMaxNonceBytes) || !validToken(proof.routeId, kMaxTokenBytes) ||
        !validToken(proof.processIdentityDigest, kMaxTokenBytes) ||
        !validToken(proof.deviceOwnershipEpoch, kMaxTokenBytes) ||
        !validToken(proof.evidenceDigest, kMaxTokenBytes)) {
        return RouteProofIoStatus::Malformed;
    }
    if (!bloom::runtime::validateGpuRouteExecutionProof(proof).empty()) {
        return RouteProofIoStatus::Malformed;
    }
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    return writeBoundedFileAtomic(routeProofPath(directory, proof.routeId),
                                  serializeRouteProof(nonce, proof));
}

[[nodiscard]] inline RouteProofIoStatus
readRouteProof(const std::filesystem::path& directory, const std::string_view routeId,
               const std::string_view expectedNonce,
               bloom::runtime::GpuRouteExecutionProof& proof) {
    RouteProofIoStatus status = RouteProofIoStatus::Ok;
    const auto content = readBoundedFile(routeProofPath(directory, routeId), kMaxFileBytes, status);
    if (!content.has_value()) {
        return status;
    }
    const auto parsed = detail::parseRecords(*content);
    if (parsed.malformed) {
        return RouteProofIoStatus::Malformed;
    }
    if (parsed.duplicate) {
        return RouteProofIoStatus::DuplicateOrConflicting;
    }
    const auto* const schema = detail::findRecord(parsed.records, "schema");
    const auto* const version = detail::findRecord(parsed.records, "version");
    const auto* const nonce = detail::findRecord(parsed.records, "nonce");
    // A present-but-wrong schema/version is reported as such even when the record set is short, so
    // a foreign file is never silently classified as malformed.
    if (schema != nullptr && *schema != kSchemaId) {
        return RouteProofIoStatus::SchemaMismatch;
    }
    if (version != nullptr) {
        const auto parsedVersion = detail::parseUnsigned(*version);
        if (!parsedVersion.has_value() || *parsedVersion != kSchemaVersion) {
            return RouteProofIoStatus::SchemaMismatch;
        }
    }
    if (parsed.records.size() != 13 || schema == nullptr || version == nullptr ||
        nonce == nullptr) {
        return RouteProofIoStatus::Malformed;
    }
    if (!validToken(*nonce, kMaxNonceBytes) || *nonce != expectedNonce) {
        return RouteProofIoStatus::StaleNonce;
    }
    const auto* const fileRoute = detail::findRecord(parsed.records, "route");
    const auto* const harness = detail::findRecord(parsed.records, "harness");
    const auto* const processIdentity = detail::findRecord(parsed.records, "process_identity");
    const auto* const epoch = detail::findRecord(parsed.records, "device_epoch");
    const auto* const dispatches = detail::findRecord(parsed.records, "native_dispatches");
    const auto* const frames = detail::findRecord(parsed.records, "verified_frames");
    const auto* const submissions = detail::findRecord(parsed.records, "readback_submissions");
    const auto* const payloads = detail::findRecord(parsed.records, "payloads");
    const auto* const bytes = detail::findRecord(parsed.records, "transferred_bytes");
    const auto* const evidence = detail::findRecord(parsed.records, "evidence");
    if (fileRoute == nullptr || harness == nullptr || processIdentity == nullptr ||
        epoch == nullptr || dispatches == nullptr || frames == nullptr || submissions == nullptr ||
        payloads == nullptr || bytes == nullptr || evidence == nullptr) {
        return RouteProofIoStatus::Malformed;
    }
    if (*fileRoute != routeId) {
        return RouteProofIoStatus::DuplicateOrConflicting;
    }
    if (!validToken(*fileRoute, kMaxTokenBytes) || !validToken(*processIdentity, kMaxTokenBytes) ||
        !validToken(*epoch, kMaxTokenBytes) || !validToken(*evidence, kMaxTokenBytes)) {
        return RouteProofIoStatus::Malformed;
    }
    const auto parsedHarness = detail::parseUnsigned(*harness);
    const auto parsedDispatches = detail::parseUnsigned(*dispatches);
    const auto parsedFrames = detail::parseUnsigned(*frames);
    const auto parsedSubmissions = detail::parseUnsigned(*submissions);
    const auto parsedPayloads = detail::parseUnsigned(*payloads);
    const auto parsedBytes = detail::parseUnsigned(*bytes);
    if (!parsedHarness.has_value() || !parsedDispatches.has_value() || !parsedFrames.has_value() ||
        !parsedSubmissions.has_value() || !parsedPayloads.has_value() || !parsedBytes.has_value()) {
        return RouteProofIoStatus::Malformed;
    }
    proof.routeId = *fileRoute;
    proof.harness = static_cast<bloom::runtime::GpuRouteHarnessKind>(*parsedHarness);
    proof.processIdentityDigest = *processIdentity;
    proof.deviceOwnershipEpoch = *epoch;
    proof.nativeDispatches = *parsedDispatches;
    proof.verifiedFrames = *parsedFrames;
    proof.readbackSubmissions = *parsedSubmissions;
    proof.payloads = *parsedPayloads;
    proof.transferredBytes = *parsedBytes;
    proof.evidenceDigest = *evidence;
    return RouteProofIoStatus::Ok;
}

struct RouteProofLoadResult final {
    std::string routeId;
    RouteProofIoStatus status = RouteProofIoStatus::Missing;
    std::string detail;
    bloom::runtime::GpuRouteExecutionProof proof;
    bool accepted = false;
};

// Reads the six known proofs and runs the typed validation. A proof is accepted only when the codec
// status is Ok, the typed validation passes, and the proof's own route matches. Every failure is
// named by status or contract issue.
[[nodiscard]] inline std::vector<RouteProofLoadResult>
readKnownRouteProofs(const std::filesystem::path& directory, const std::string_view expectedNonce) {
    std::vector<RouteProofLoadResult> results;
    for (const auto& routeId : knownRouteIds()) {
        RouteProofLoadResult result;
        result.routeId = routeId;
        bloom::runtime::GpuRouteExecutionProof proof;
        result.status = readRouteProof(directory, routeId, expectedNonce, proof);
        if (result.status != RouteProofIoStatus::Ok) {
            result.detail = std::string{routeProofIoStatusName(result.status)};
            results.push_back(std::move(result));
            continue;
        }
        const auto issues = bloom::runtime::validateGpuRouteExecutionProof(proof);
        if (!issues.empty()) {
            result.status = RouteProofIoStatus::Malformed;
            result.detail = issues.front().detail;
            results.push_back(std::move(result));
            continue;
        }
        result.proof = std::move(proof);
        result.accepted = true;
        results.push_back(std::move(result));
    }
    return results;
}

} // namespace bloom::gpu_route_proof_io
