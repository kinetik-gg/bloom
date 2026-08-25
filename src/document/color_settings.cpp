#include <bloom/document/color_settings.hpp>

#include <bloom/core/utf8.hpp>

#include <algorithm>
#include <compare>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace {

[[nodiscard]] bool isAsciiAlpha(const char character) noexcept {
    return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z');
}

[[nodiscard]] bool isAsciiDigit(const char character) noexcept {
    return character >= '0' && character <= '9';
}

[[nodiscard]] bool isAsciiHexDigit(const char character) noexcept {
    return isAsciiDigit(character) || (character >= 'A' && character <= 'F') ||
           (character >= 'a' && character <= 'f');
}

[[nodiscard]] unsigned char decodeHexPair(const char high, const char low) noexcept {
    const auto nibble = [](const char value) {
        if (isAsciiDigit(value)) {
            return static_cast<unsigned char>(value - '0');
        }
        if (value >= 'A' && value <= 'F') {
            return static_cast<unsigned char>(value - 'A' + 10);
        }
        return static_cast<unsigned char>(value - 'a' + 10);
    };
    return static_cast<unsigned char>((nibble(high) << 4U) | nibble(low));
}

[[nodiscard]] bool isUnreservedUriCharacter(const char character) noexcept {
    return isAsciiAlpha(character) || isAsciiDigit(character) || character == '-' ||
           character == '.' || character == '_' || character == '~';
}

[[nodiscard]] bool isSubDelimiter(const char character) noexcept {
    constexpr std::string_view delimiters = "!$&'()*+,;=";
    return delimiters.find(character) != std::string_view::npos;
}

[[nodiscard]] bool startsWithFileScheme(const std::string_view uri) noexcept {
    constexpr std::string_view scheme = "file:";
    if (uri.size() < scheme.size()) {
        return false;
    }
    for (std::size_t index = 0; index < scheme.size(); ++index) {
        const auto character = uri[index];
        const auto folded = character >= 'A' && character <= 'Z'
                                ? static_cast<char>(character - 'A' + 'a')
                                : character;
        if (folded != scheme[index]) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool validPercentEscape(const std::string_view value,
                                      const std::size_t percentIndex) noexcept {
    return percentIndex + 2 < value.size() && isAsciiHexDigit(value[percentIndex + 1]) &&
           isAsciiHexDigit(value[percentIndex + 2]);
}

[[nodiscard]] bool validFileAuthority(const std::string_view authority) noexcept {
    if (authority.empty()) {
        return true;
    }
    if (authority.find('@') != std::string_view::npos) {
        return false;
    }

    if (authority.find_first_of("[]:") != std::string_view::npos) {
        return false;
    }

    for (std::size_t index = 0; index < authority.size(); ++index) {
        const auto character = authority[index];
        if (character == '%') {
            if (!validPercentEscape(authority, index)) {
                return false;
            }
            const auto decoded = decodeHexPair(authority[index + 1], authority[index + 2]);
            if (decoded == 0 || decoded == '/' || decoded == '\\' || decoded == '@') {
                return false;
            }
            index += 2;
        } else if (!(isUnreservedUriCharacter(character) || isSubDelimiter(character))) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool validFilePath(const std::string_view path) noexcept {
    if (path.empty() || path.front() != '/') {
        return false;
    }
    for (std::size_t index = 0; index < path.size(); ++index) {
        const auto character = path[index];
        if (character == '%') {
            if (!validPercentEscape(path, index)) {
                return false;
            }
            const auto decoded = decodeHexPair(path[index + 1], path[index + 2]);
            if (decoded == 0 || decoded == '/' || decoded == '\\') {
                return false;
            }
            index += 2;
        } else if (!(isUnreservedUriCharacter(character) || isSubDelimiter(character) ||
                     character == ':' || character == '@' || character == '/')) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::string decodePathSegment(const std::string_view segment) {
    std::string decoded;
    decoded.reserve(segment.size());
    for (std::size_t index = 0; index < segment.size(); ++index) {
        if (segment[index] != '%') {
            decoded.push_back(segment[index]);
            continue;
        }
        decoded.push_back(static_cast<char>(decodeHexPair(segment[index + 1], segment[index + 2])));
        index += 2;
    }
    return decoded;
}

enum class ExternalFileKind : std::uint8_t {
    Ocioz,
    Config,
};

[[nodiscard]] bool isValidExternalFileUri(const std::string_view uri,
                                          const ExternalFileKind expectedKind) {
    if (uri.empty() || uri.size() > bloom::document::kMaxOcioExternalUriBytes ||
        !startsWithFileScheme(uri) ||
        !std::ranges::all_of(
            uri,
            [](const char character) { return static_cast<unsigned char>(character) <= 0x7FU; }) ||
        uri.find('\0') != std::string_view::npos || uri.find('?') != std::string_view::npos ||
        uri.find('#') != std::string_view::npos) {
        return false;
    }

    const auto hierarchy = uri.substr(5);
    std::string_view path;
    if (hierarchy.starts_with("//")) {
        const auto pathStart = hierarchy.find('/', 2);
        if (pathStart == std::string_view::npos ||
            !validFileAuthority(hierarchy.substr(2, pathStart - 2))) {
            return false;
        }
        path = hierarchy.substr(pathStart);
    } else {
        path = hierarchy;
    }
    if (!validFilePath(path)) {
        return false;
    }

    const auto finalSeparator = path.find_last_of('/');
    const auto filename = decodePathSegment(path.substr(finalSeparator + 1));
    if (expectedKind == ExternalFileKind::Config) {
        return filename == "config.ocio";
    }
    constexpr std::string_view suffix = ".ocioz";
    return filename.ends_with(suffix);
}

[[nodiscard]] bool isValidProjectRelativeOciozPath(const std::string_view path) noexcept {
    constexpr std::string_view suffix = ".ocioz";
    if (path.empty() || path.size() > bloom::document::kMaxOcioProjectRelativePathBytes ||
        !bloom::core::isValidUtf8(path) || path.front() == '/' ||
        path.find('\\') != std::string_view::npos || path.find('\0') != std::string_view::npos ||
        !path.ends_with(suffix) || (path.size() >= 2 && isAsciiAlpha(path[0]) && path[1] == ':')) {
        return false;
    }

    std::size_t componentStart = 0;
    while (componentStart <= path.size()) {
        const auto separator = path.find('/', componentStart);
        const auto component = path.substr(componentStart, separator == std::string_view::npos
                                                               ? std::string_view::npos
                                                               : separator - componentStart);
        if (component.empty() || component == "." || component == "..") {
            return false;
        }
        if (separator == std::string_view::npos) {
            break;
        }
        componentStart = separator + 1;
    }
    return true;
}

[[nodiscard]] bool isValidContextName(const std::string_view name) noexcept {
    if (name.empty() || name.size() > bloom::document::kMaxOcioContextNameBytes ||
        !(isAsciiAlpha(name.front()) || name.front() == '_')) {
        return false;
    }
    return std::ranges::all_of(name.substr(1), [](const char character) {
        return isAsciiAlpha(character) || isAsciiDigit(character) || character == '_';
    });
}

[[nodiscard]] bool isValidContextValue(const std::string_view value) noexcept {
    return value.size() <= bloom::document::kMaxOcioContextValueBytes &&
           value.find('\0') == std::string_view::npos && bloom::core::isValidUtf8(value);
}

[[nodiscard]] std::optional<bloom::document::OcioConfigPortability>
requiredPortability(const bloom::document::OcioConfigLocator& locator) {
    if (locator.valueless_by_exception()) {
        return std::nullopt;
    }
    return std::visit(
        [](const auto& value) {
            using Locator = std::remove_cvref_t<decltype(value)>;
            if constexpr (std::is_same_v<Locator, bloom::document::BuiltInOcioConfigLocator>) {
                return bloom::document::OcioConfigPortability::BuiltIn;
            } else if constexpr (std::is_same_v<Locator,
                                                bloom::document::ProjectRelativeOciozLocator>) {
                return bloom::document::OcioConfigPortability::ProjectRelative;
            } else {
                return bloom::document::OcioConfigPortability::External;
            }
        },
        locator);
}

} // namespace

namespace bloom::document {

ValidationResult OcioConfigReference::validate() const {
    ValidationResult result;
    if (schemaVersion != kOcioConfigReferenceSchemaVersionV1) {
        result.add(ValidationCode::InvalidValue, "schemaVersion",
                   "OCIO config reference schema version must be exactly 1.0");
    }

    if (locator.valueless_by_exception()) {
        result.add(ValidationCode::InvalidValue, "locator",
                   "OCIO config locator must contain one known v1 locator family");
    } else {
        std::visit(
            [&result](const auto& value) {
                using Locator = std::remove_cvref_t<decltype(value)>;
                if constexpr (std::is_same_v<Locator, BuiltInOcioConfigLocator>) {
                    if (value.uri != kBloomNeutralConfigUriV1) {
                        result.add(ValidationCode::InvalidValue, "locator.uri",
                                   "Built-in OCIO URI must name immutable Bloom Neutral v1");
                    }
                } else if constexpr (std::is_same_v<Locator, ProjectRelativeOciozLocator>) {
                    if (!isValidProjectRelativeOciozPath(value.path)) {
                        result.add(
                            ValidationCode::InvalidValue, "locator.path",
                            "Project-relative OCIO archive path is not normalized and valid");
                    }
                } else if constexpr (std::is_same_v<Locator, ExternalOciozLocator>) {
                    if (!isValidExternalFileUri(value.uri, ExternalFileKind::Ocioz)) {
                        result.add(ValidationCode::InvalidValue, "locator.uri",
                                   "External OCIO archive must be an absolute file URI to .ocioz");
                    }
                } else if constexpr (std::is_same_v<Locator, ExternalOcioConfigLocator>) {
                    if (!isValidExternalFileUri(value.uri, ExternalFileKind::Config)) {
                        result.add(ValidationCode::InvalidValue, "locator.uri",
                                   "External loose OCIO config must be an absolute file URI to "
                                   "config.ocio");
                    }
                }
            },
            locator);
    }

    if (expectedRevision.algorithm != OcioRevisionAlgorithm::Sha256) {
        result.add(ValidationCode::InvalidValue, "expectedRevision.algorithm",
                   "OCIO content revision algorithm must be SHA-256");
    }

    const auto expectedPortability = requiredPortability(locator);
    if (expectedPortability.has_value() && portability != *expectedPortability) {
        result.add(ValidationCode::TypeMismatch, "portability",
                   "OCIO portability must agree with the locator family");
    }

    if (contextVariables.size() > kMaxOcioContextVariables) {
        result.add(ValidationCode::InvalidValue, "contextVariables",
                   "OCIO context variable count exceeds 256");
    }
    std::string_view previousName;
    bool hasPreviousName = false;
    const auto contextVariablesToValidate =
        std::min(contextVariables.size(), kMaxOcioContextVariables);
    for (std::size_t index = 0; index < contextVariablesToValidate; ++index) {
        const auto& variable = contextVariables[index];
        const auto path = "contextVariables[" + std::to_string(index) + "]";
        if (!isValidContextName(variable.name)) {
            result.add(variable.name.empty() ? ValidationCode::EmptyKey
                                             : ValidationCode::InvalidValue,
                       path + ".name", "OCIO context name must match [A-Za-z_][A-Za-z0-9_]{0,127}");
        }
        if (!isValidContextValue(variable.value)) {
            result.add(ValidationCode::InvalidValue, path + ".value",
                       "OCIO context value must be valid NUL-free UTF-8 of at most 4096 bytes");
        }
        if (hasPreviousName &&
            core::compareUtf8Bytes(previousName, variable.name) != std::strong_ordering::less) {
            result.add(ValidationCode::InvalidOrder, path + ".name",
                       "OCIO context names must be unique and sorted by UTF-8 bytes");
        }
        previousName = variable.name;
        hasPreviousName = true;
    }
    return result;
}

ValidationResult ColorSettings::validate() const {
    ValidationResult result;
    if (schemaVersion != kColorSettingsSchemaVersionV1) {
        result.add(ValidationCode::InvalidValue, "schemaVersion",
                   "Color settings schema version must be exactly 1.0");
    }
    if (processColorSpaceId != kProcessColorSpaceIdV1) {
        result.add(ValidationCode::InvalidValue, "processColorSpaceId",
                   "Version 1 process color space must be lin_rec709_scene");
    }
    result.append("ocioConfig", ocioConfig.validate());
    return result;
}

ColorSettings makeBloomNeutralColorSettingsV1(const core::Sha256Digest expectedRevision) {
    return {
        .schemaVersion = kColorSettingsSchemaVersionV1,
        .processColorSpaceId = std::string(kProcessColorSpaceIdV1),
        .ocioConfig =
            {
                .schemaVersion = kOcioConfigReferenceSchemaVersionV1,
                .locator = BuiltInOcioConfigLocator{std::string(kBloomNeutralConfigUriV1)},
                .expectedRevision =
                    {
                        .algorithm = OcioRevisionAlgorithm::Sha256,
                        .digest = expectedRevision,
                    },
                .portability = OcioConfigPortability::BuiltIn,
                .contextVariables = {},
            },
    };
}

} // namespace bloom::document
