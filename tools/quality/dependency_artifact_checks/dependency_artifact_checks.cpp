#include "dependency_artifact_checks_internal.hpp"

#include <bloom/core/sha256.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

namespace bloom::quality::dependencies {
namespace detail {

[[nodiscard]] auto asBytes(std::string_view value) noexcept -> std::span<const std::byte>;

[[noreturn]] void fail(const std::string_view code, const std::string_view location,
                       const std::string_view detail) {
    throw CheckError(code, location, detail);
}

[[nodiscard]] auto asBytes(const std::string_view value) noexcept -> std::span<const std::byte> {
    return std::as_bytes(std::span(value.data(), value.size()));
}

[[nodiscard]] auto digestHex(const std::string_view value) -> std::string {
    const auto digest = core::Sha256Hasher::hash(asBytes(value));
    if (!digest.has_value()) {
        fail("sha256", "$", "input exceeds the SHA-256 implementation limit");
    }
    const auto hexadecimal = digest->toLowercaseHex();
    return std::string(hexadecimal.data(), hexadecimal.size());
}

auto object(const Value& value, const std::initializer_list<std::string_view> keys,
            const std::string_view location) -> const Value::Object& {
    if (!value.isObject()) {
        fail("type", location, "expected object");
    }
    const auto& members = value.asObject();
    if (members.size() != keys.size()) {
        fail("members", location, "object members differ from the frozen contract");
    }
    std::initializer_list<std::string_view>::const_iterator expected = keys.begin();
    for (const auto& [name, child] : members) {
        static_cast<void>(child);
        if (name != *expected) {
            fail("members", location, "object member order differs from the frozen contract");
        }
        ++expected;
    }
    return members;
}

[[nodiscard]] auto array(const Value& value, const std::string_view location,
                         const std::size_t maximum, const std::size_t minimum)
    -> const Value::Array& {
    if (!value.isArray()) {
        fail("type", location, "expected array");
    }
    const auto& result = value.asArray();
    if (result.size() < minimum || result.size() > maximum) {
        fail("count", location, "array length is outside the frozen contract");
    }
    return result;
}

[[nodiscard]] auto isAscii(const std::string_view value) noexcept -> bool {
    return std::ranges::all_of(value, [](const unsigned char byte) { return byte <= 0x7FU; });
}

[[nodiscard]] auto isIdentifier(const std::string_view value) noexcept -> bool {
    if (value.empty() || value.size() > 128) {
        return false;
    }
    const auto first = static_cast<unsigned char>(value.front());
    if (!((first >= 'a' && first <= 'z') || (first >= '0' && first <= '9'))) {
        return false;
    }
    return std::ranges::all_of(value, [](const unsigned char byte) {
        return (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') || byte == '.' ||
               byte == '_' || byte == '-';
    });
}

[[nodiscard]] auto isDottedVersion(const std::string_view value) noexcept -> bool {
    if (value.empty() || value.front() == '.' || value.back() == '.') {
        return false;
    }
    return std::ranges::all_of(value,
                               [](const unsigned char byte) {
                                   return (byte >= '0' && byte <= '9') || byte == '.';
                               }) &&
           value.find("..") == std::string_view::npos;
}

[[nodiscard]] auto isEnvironmentName(const std::string_view value) noexcept -> bool {
    if (value.empty() || value.size() > 128 ||
        !(value.front() == '_' || (value.front() >= 'A' && value.front() <= 'Z'))) {
        return false;
    }
    return std::ranges::all_of(value, [](const unsigned char byte) {
        return byte == '_' || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9');
    });
}

[[nodiscard]] auto isCmakeOption(const std::string_view value) noexcept -> bool {
    if (value.empty() || value.size() > 256) {
        return false;
    }
    const auto first = static_cast<unsigned char>(value.front());
    if (!((first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z') || first == '_')) {
        return false;
    }
    return std::ranges::all_of(value, [](const unsigned char byte) {
        return (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
               (byte >= '0' && byte <= '9') || byte == '_' || byte == '.' || byte == '-';
    });
}

auto stringValue(const Value& value, const std::string_view location, const std::size_t maximum)
    -> const std::string& {
    if (!value.isString() || value.asString().empty() || value.asString().size() > maximum) {
        fail("string", location, "expected a non-empty bounded UTF-8 string");
    }
    return value.asString();
}

auto nullableString(const Value& value, const std::string_view location, const std::size_t maximum)
    -> std::optional<std::string_view> {
    if (value.isNull()) {
        return std::nullopt;
    }
    return stringValue(value, location, maximum);
}

auto identifier(const Value& value, const std::string_view location) -> const std::string& {
    const auto& result = stringValue(value, location, 128);
    if (!isIdentifier(result)) {
        fail("lexical", location, "expected a lowercase portable identifier");
    }
    return result;
}

auto enumString(const Value& value, const std::initializer_list<std::string_view> allowed,
                const std::string_view location) -> const std::string& {
    const auto& result = stringValue(value, location);
    if (std::ranges::find(allowed, result) == allowed.end()) {
        fail("enum", location, "value is outside the frozen enumeration");
    }
    return result;
}

auto uintValue(const Value& value, const std::string_view location, const std::uint64_t maximum)
    -> std::uint64_t {
    const auto result = json::asUint64(value);
    if (!result.has_value() || *result > maximum) {
        fail("uint", location, "expected an unsigned integer in range");
    }
    return *result;
}

auto booleanValue(const Value& value, const std::string_view location) -> bool {
    if (!value.isBoolean()) {
        fail("type", location, "expected boolean");
    }
    return value.asBoolean();
}

[[nodiscard]] auto isDigest(const std::string_view value) noexcept -> bool {
    if (value.size() != 71 || !value.starts_with("sha256:")) {
        return false;
    }
    return std::ranges::all_of(value.substr(7), [](const unsigned char byte) {
        return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
    });
}

auto digestValue(const Value& value, const std::string_view location, const FixtureContext* context)
    -> const std::string& {
    const auto& result = stringValue(value, location, 71);
    if (!isDigest(result)) {
        fail("lexical", location, "expected canonical sha256 text");
    }
    if (context != nullptr && !context->payloadDigests.contains(result)) {
        fail("fixture-digest", location, "digest does not bind a checked-in synthetic payload");
    }
    return result;
}

[[nodiscard]] auto lowerAscii(std::string value) -> std::string {
    std::ranges::transform(value, value.begin(), [](const unsigned char byte) {
        return static_cast<char>(byte >= 'A' && byte <= 'Z' ? byte - 'A' + 'a' : byte);
    });
    return value;
}

auto portablePath(const Value& value, const std::string_view location) -> const std::string& {
    const auto& result = stringValue(value, location, 1024);
    static const std::set<std::string> reserved = [] {
        std::set<std::string> names{"con", "prn", "aux", "nul", "clock$", "conin$", "conout$"};
        for (const auto* prefix : {"com", "lpt"}) {
            for (int number = 1; number <= 9; ++number) {
                names.insert(std::string(prefix) + std::to_string(number));
            }
        }
        return names;
    }();
    std::size_t begin = 0;
    while (begin <= result.size()) {
        const auto end = result.find('/', begin);
        const auto segment = std::string_view(result).substr(
            begin, end == std::string::npos ? result.size() - begin : end - begin);
        if (segment.empty() || segment == "." || segment == ".." || segment.size() > 255 ||
            segment.back() == ' ' || segment.back() == '.') {
            fail("portable-path", location, "path contains a non-portable segment");
        }
        for (const auto byte : segment) {
            const auto unsignedByte = static_cast<unsigned char>(byte);
            if (unsignedByte < 32 || unsignedByte == 127 ||
                std::string_view{"\\<>:\"|?*"}.find(byte) != std::string_view::npos) {
                fail("portable-path", location, "path contains a forbidden byte");
            }
        }
        const auto stemEnd = segment.find('.');
        if (reserved.contains(lowerAscii(std::string(segment.substr(0, stemEnd))))) {
            fail("portable-path", location, "path contains a reserved device name");
        }
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
    return result;
}

void validatePrintableToken(const std::string_view value, const std::string_view location) {
    if (!std::ranges::all_of(value, [](const unsigned char byte) {
            return byte >= 0x21U && byte <= 0x7EU && byte != ';' && byte != '"' && byte != '\\';
        })) {
        fail("printable-token", location, "expected a portable printable token");
    }
}

[[nodiscard]] auto strictEqual(const Value& left, const Value& right) -> bool {
    if (left.storage().index() != right.storage().index()) {
        return false;
    }
    if (left.isObject()) {
        const auto& leftObject = left.asObject();
        const auto& rightObject = right.asObject();
        if (leftObject.size() != rightObject.size()) {
            return false;
        }
        for (std::size_t index = 0; index < leftObject.size(); ++index) {
            if (leftObject[index].first != rightObject[index].first ||
                !strictEqual(leftObject[index].second, rightObject[index].second)) {
                return false;
            }
        }
        return true;
    }
    if (left.isArray()) {
        const auto& leftArray = left.asArray();
        const auto& rightArray = right.asArray();
        return leftArray.size() == rightArray.size() &&
               std::ranges::equal(leftArray, rightArray, strictEqual);
    }
    if (left.isNull()) {
        return true;
    }
    if (left.isBoolean()) {
        return left.asBoolean() == right.asBoolean();
    }
    if (left.isNumber()) {
        return left.asNumber() == right.asNumber();
    }
    return left.asString() == right.asString();
}

auto artifactReference(const Value& value, const std::string_view location,
                       const FixtureContext& context) -> std::pair<std::string, std::string> {
    object(value, {"path", "sha256"}, location);
    const auto& path = portablePath(value.at("path"), std::string(location) + ".path");
    const auto& digest = digestValue(value.at("sha256"), std::string(location) + ".sha256");
    verifyFixturePayload(path, digest, location, context);
    return {path, digest};
}

void validateDate(const Value& value, const std::string_view location) {
    const auto& text = stringValue(value, location, 10);
    const auto digitAt = [&text](const std::size_t index) {
        return text[index] >= '0' && text[index] <= '9';
    };
    if (text.size() != 10 || text[4] != '-' || text[7] != '-' ||
        !std::ranges::all_of(std::array<std::size_t, 8>{0, 1, 2, 3, 5, 6, 8, 9}, digitAt)) {
        fail("date", location, "expected YYYY-MM-DD");
    }
    const auto yearNumber = std::stoi(text.substr(0, 4));
    if (yearNumber < 1) {
        fail("date", location, "Gregorian years begin at 0001");
    }
    const auto year = std::chrono::year(yearNumber);
    const auto month = std::chrono::month(static_cast<unsigned>(std::stoi(text.substr(5, 2))));
    const auto day = std::chrono::day(static_cast<unsigned>(std::stoi(text.substr(8, 2))));
    if (!std::chrono::year_month_day(year, month, day).ok()) {
        fail("date", location, "calendar date is invalid");
    }
}

void validateArtifactArray(const Value& value, const std::string_view location,
                           const FixtureContext& context, const std::size_t minimum) {
    const auto& values = array(value, location, 8192, minimum);
    for (std::size_t index = 0; index < values.size(); ++index) {
        artifactReference(values[index], std::string(location) + '[' + std::to_string(index) + ']',
                          context);
    }
    requireOrdered(
        values, [](const Value& child) { return child.at("path").asString(); }, location);
}

void validatePrefixArtifactArray(const Value& value, const std::string_view location,
                                 const FixtureContext& context, const std::size_t minimum) {
    const auto& values = array(value, location, 8192, minimum);
    for (std::size_t index = 0; index < values.size(); ++index) {
        const auto childLocation = std::string(location) + '[' + std::to_string(index) + ']';
        object(values[index], {"path", "sha256"}, childLocation);
        portablePath(values[index].at("path"), childLocation + ".path");
        digestValue(values[index].at("sha256"), childLocation + ".sha256", &context);
    }
    requireOrdered(
        values, [](const Value& child) { return child.at("path").asString(); }, location);
}
} // namespace detail

using namespace detail;

auto makeFixtureContext(const Path& inputRoot) -> FixtureContext {
    std::error_code error;
    const auto root = std::filesystem::canonical(inputRoot, error);
    if (error) {
        fail("fixture-root", inputRoot.string(), error.message());
    }
    const auto payloadRoot =
        std::filesystem::canonical(root / Path{kFixtureDirectory} / "payloads", error);
    if (error) {
        fail("fixture-payloads", (root / Path{kFixtureDirectory} / "payloads").string(),
             error.message());
    }
    FixtureContext result{
        .root = root, .payloadRoot = payloadRoot, .payloadDigests = {}, .payloadSizes = {}};
    for (std::filesystem::recursive_directory_iterator iterator(
             payloadRoot, std::filesystem::directory_options::skip_permission_denied, error),
         end;
         iterator != end; iterator.increment(error)) {
        if (error) {
            fail("fixture-payloads", payloadRoot.string(), error.message());
        }
        const auto status = iterator->symlink_status(error);
        if (error) {
            fail("fixture-payloads", iterator->path().string(), error.message());
        }
        if (std::filesystem::is_regular_file(status)) {
            const auto encoded = readBounded(iterator->path(), 1'048'576);
            const auto digest = "sha256:" + digestHex(encoded);
            result.payloadDigests.insert(digest);
            result.payloadSizes.insert_or_assign(digest, encoded.size());
        }
    }
    if (result.payloadDigests.empty()) {
        fail("fixture-payloads", payloadRoot.string(), "no synthetic payloads");
    }
    return result;
}

void rejectFixtureAsProductionPath(const Path& path, const Path& expected,
                                   const Path& fixtureRoot) {
    if (std::ranges::any_of(path, [](const Path& component) { return component == ".."; })) {
        fail("production-path", path.string(), "lexical parent-directory aliases are forbidden");
    }
    std::error_code error;
    const auto resolved = std::filesystem::weakly_canonical(path, error);
    if (error) {
        fail("production-path", path.string(), error.message());
    }
    const auto fixture = std::filesystem::weakly_canonical(fixtureRoot, error);
    if (error) {
        fail("production-path", fixtureRoot.string(), error.message());
    }
    const auto relative = resolved.lexically_relative(fixture);
    if (!relative.empty() && !relative.is_absolute() &&
        (relative.begin() == relative.end() || *relative.begin() != "..")) {
        fail("fixture-separation", path.string(), "fixture cannot be used as production artifact");
    }
    if (path != expected ||
        std::filesystem::is_symlink(std::filesystem::symlink_status(path, error))) {
        fail("production-path", path.string(), "path is not the exact production artifact path");
    }
}

void verifyFixturePayload(const std::string_view relativePath, const std::string_view digest,
                          const std::string_view location, const FixtureContext& context) {
    if (!relativePath.starts_with(kPayloadPrefix)) {
        fail("fixture-separation", location,
             "synthetic evidence must remain below fixture payloads");
    }
    auto candidate = context.root;
    for (const auto& component : Path(relativePath)) {
        candidate /= component;
        std::error_code error;
        const auto status = std::filesystem::symlink_status(candidate, error);
        if (!error && std::filesystem::is_symlink(status)) {
            fail("fixture-evidence", location, "payload path must not traverse a symlink");
        }
    }
    std::error_code error;
    const auto absolute = std::filesystem::weakly_canonical(context.root / relativePath, error);
    if (error) {
        fail("fixture-evidence", location, error.message());
    }
    const auto relative = absolute.lexically_relative(context.payloadRoot);
    if (relative.empty() || relative.is_absolute() ||
        (relative.begin() != relative.end() && *relative.begin() == "..")) {
        fail("fixture-separation", location, "path escapes fixture payloads");
    }
    const auto status = std::filesystem::symlink_status(absolute, error);
    if (error || !std::filesystem::is_regular_file(status)) {
        fail("fixture-evidence", location, "must name a checked-in regular payload");
    }
    const auto actual = "sha256:" + digestHex(readBounded(absolute, 1'048'576));
    if (actual != digest) {
        fail("fixture-evidence", location, "digest differs from the exact fixture payload bytes");
    }
}

auto timestampFromEpoch(const std::uint64_t epoch) -> std::string {
    if (epoch > 253'402'300'799ULL) {
        fail("timestamp", "$", "epoch exceeds year 9999");
    }
    using namespace std::chrono;
    const auto instant = sys_seconds{seconds{epoch}};
    const auto dayPoint = floor<days>(instant);
    const year_month_day date{dayPoint};
    const hh_mm_ss time{instant - dayPoint};
    std::ostringstream rendered;
    rendered << std::setfill('0') << std::setw(4) << int(date.year()) << '-' << std::setw(2)
             << unsigned(date.month()) << '-' << std::setw(2) << unsigned(date.day()) << 'T'
             << std::setw(2) << time.hours().count() << ':' << std::setw(2)
             << time.minutes().count() << ':' << std::setw(2) << time.seconds().count() << 'Z';
    return rendered.str();
}
} // namespace bloom::quality::dependencies
