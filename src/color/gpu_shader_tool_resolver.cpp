#include <bloom/color/gpu_shader_tool_resolver.hpp>

#include "gpu_shader_compiler_process.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace bloom::color {
namespace {

using Clock = std::chrono::steady_clock;
using gpu_shader_detail::hashFileBounded;

constexpr std::string_view kInventoryFormat = "org.kinetik.bloom.gpu-shader-tools.inventory";
constexpr std::string_view kInventoryLayout = "executable-relative";

bool singleSegment(std::string_view name) {
    if (name.empty() || name == "." || name == "..")
        return false;
    return name.find('/') == std::string_view::npos && name.find('\\') == std::string_view::npos &&
           name.find('\0') == std::string_view::npos;
}

// True only when child is strictly below parent (both canonical absolute paths).
bool isWithin(const std::filesystem::path& parent, const std::filesystem::path& child) {
    const auto relative = child.lexically_relative(parent);
    if (relative.empty() || relative == ".")
        return false;
    const auto first = relative.begin();
    return first != relative.end() && first->string() != "..";
}

bool readBounded(const std::filesystem::path& path, std::size_t ceiling, std::string& out) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > ceiling)
        return false;
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return false;
    out.resize(static_cast<std::size_t>(size));
    input.read(out.data(), static_cast<std::streamsize>(size));
    return input.good() || input.eof();
}

bool parseDigest(const std::string& text, core::Sha256Digest& out) {
    constexpr std::string_view prefix = "sha256:";
    if (text.size() != prefix.size() + core::kSha256HexCharacters ||
        text.compare(0, prefix.size(), prefix) != 0)
        return false;
    const auto parsed =
        core::Sha256Digest::fromLowercaseHex(std::string_view(text).substr(prefix.size()));
    if (!parsed)
        return false;
    out = *parsed;
    return true;
}

// Minimal bounded JSON reader for the generated inventory. Every structural error, depth overflow,
// unsupported escape, or trailing byte is a parse failure; only the fields the resolver needs are
// interpreted and every other value is skipped generically.
class JsonReader final {
  public:
    explicit JsonReader(std::string_view text) : text_(text) {}

    bool consume(char expected) {
        skipWhitespace();
        if (position_ < text_.size() && text_[position_] == expected) {
            ++position_;
            return true;
        }
        return false;
    }
    [[nodiscard]] char peek() {
        skipWhitespace();
        return position_ < text_.size() ? text_[position_] : '\0';
    }
    void advance() { ++position_; }

    bool readString(std::string& out) {
        skipWhitespace();
        if (position_ >= text_.size() || text_[position_] != '"')
            return false;
        ++position_;
        out.clear();
        while (position_ < text_.size()) {
            const char character = text_[position_++];
            if (character == '"')
                return true;
            if (character == '\\') {
                if (position_ >= text_.size())
                    return false;
                switch (text_[position_++]) {
                case '"':
                    out.push_back('"');
                    break;
                case '\\':
                    out.push_back('\\');
                    break;
                case '/':
                    out.push_back('/');
                    break;
                case 'b':
                    out.push_back('\b');
                    break;
                case 'f':
                    out.push_back('\f');
                    break;
                case 'n':
                    out.push_back('\n');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                default:
                    return false; // \u and anything else: not produced by our generator.
                }
            } else if (static_cast<unsigned char>(character) < 0x20) {
                return false;
            } else {
                out.push_back(character);
            }
        }
        return false;
    }

    bool readBoolean(bool& out) {
        skipWhitespace();
        if (text_.substr(position_, 4) == "true") {
            out = true;
            position_ += 4;
            return true;
        }
        if (text_.substr(position_, 5) == "false") {
            out = false;
            position_ += 5;
            return true;
        }
        return false;
    }

    bool skipValue() {
        if (depth_ >= kMaxDepth)
            return false;
        ++depth_;
        skipWhitespace();
        bool ok = false;
        if (position_ < text_.size()) {
            const char character = text_[position_];
            if (character == '"') {
                std::string ignored;
                ok = readString(ignored);
            } else if (character == '{' || character == '[') {
                const char close = character == '{' ? '}' : ']';
                ++position_;
                ok = true;
                bool first = true;
                while (ok) {
                    if (peek() == close) {
                        advance();
                        break;
                    }
                    if (!first && !consume(',')) {
                        ok = false;
                        break;
                    }
                    first = false;
                    if (character == '{') {
                        std::string key;
                        ok = readString(key) && consume(':') && skipValue();
                    } else {
                        ok = skipValue();
                    }
                }
            } else if (character == 't' || character == 'f') {
                bool ignored = false;
                ok = readBoolean(ignored);
            } else if (text_.substr(position_, 4) == "null") {
                position_ += 4;
                ok = true;
            } else {
                const std::size_t start = position_;
                while (position_ < text_.size()) {
                    const char c = text_[position_];
                    if (std::isdigit(static_cast<unsigned char>(c)) || c == '-' || c == '+' ||
                        c == '.' || c == 'e' || c == 'E')
                        ++position_;
                    else
                        break;
                }
                ok = position_ > start;
            }
        }
        --depth_;
        return ok;
    }

    [[nodiscard]] bool atEnd() {
        skipWhitespace();
        return position_ >= text_.size();
    }

  private:
    static constexpr int kMaxDepth = 8;
    void skipWhitespace() {
        while (position_ < text_.size() && (text_[position_] == ' ' || text_[position_] == '\n' ||
                                            text_[position_] == '\t' || text_[position_] == '\r'))
            ++position_;
    }

    std::string_view text_;
    std::size_t position_ = 0;
    int depth_ = 0;
};

struct ParsedTool {
    std::string name;
    std::string file;
    std::string source;
    core::Sha256Digest sourceSha{};
    core::Sha256Digest stagedSha{};
    bool hasSource = false;
    bool hasStaged = false;
};

struct ParsedInventory {
    std::string format;
    std::string layout;
    std::string directory;
    bool relocated = false;
    std::vector<ParsedTool> tools;
};

bool parseTools(JsonReader& reader, ParsedInventory& inventory) {
    if (!reader.consume('['))
        return false;
    bool first = true;
    while (true) {
        if (reader.peek() == ']') {
            reader.advance();
            return true;
        }
        if (!first && !reader.consume(','))
            return false;
        first = false;
        if (!reader.consume('{'))
            return false;
        ParsedTool tool;
        bool memberFirst = true;
        while (true) {
            if (reader.peek() == '}') {
                reader.advance();
                break;
            }
            if (!memberFirst && !reader.consume(','))
                return false;
            memberFirst = false;
            std::string key;
            if (!reader.readString(key) || !reader.consume(':'))
                return false;
            if (key == "name") {
                if (!reader.readString(tool.name))
                    return false;
            } else if (key == "file") {
                if (!reader.readString(tool.file))
                    return false;
            } else if (key == "source") {
                if (!reader.readString(tool.source))
                    return false;
            } else if (key == "sourceSha256") {
                std::string value;
                if (!reader.readString(value) || !parseDigest(value, tool.sourceSha))
                    return false;
                tool.hasSource = true;
            } else if (key == "stagedSha256") {
                std::string value;
                if (!reader.readString(value) || !parseDigest(value, tool.stagedSha))
                    return false;
                tool.hasStaged = true;
            } else if (!reader.skipValue()) {
                return false;
            }
        }
        inventory.tools.push_back(std::move(tool));
    }
}

bool parseInventory(std::string_view text, ParsedInventory& inventory) {
    JsonReader reader(text);
    if (!reader.consume('{'))
        return false;
    bool first = true;
    while (true) {
        if (reader.peek() == '}') {
            reader.advance();
            return reader.atEnd();
        }
        if (!first && !reader.consume(','))
            return false;
        first = false;
        std::string key;
        if (!reader.readString(key) || !reader.consume(':'))
            return false;
        if (key == "format") {
            if (!reader.readString(inventory.format))
                return false;
        } else if (key == "layout") {
            if (!reader.readString(inventory.layout))
                return false;
        } else if (key == "directory") {
            if (!reader.readString(inventory.directory))
                return false;
        } else if (key == "relocated") {
            if (!reader.readBoolean(inventory.relocated))
                return false;
        } else if (key == "tools") {
            if (!parseTools(reader, inventory))
                return false;
        } else if (!reader.skipValue()) {
            return false;
        }
    }
}

GpuShaderToolResolveResult failed(GpuShaderToolResolveError code, std::string diagnostic) {
    GpuShaderToolResolveResult result;
    result.failure = GpuShaderToolResolveFailure{code, std::move(diagnostic)};
    return result;
}

std::string digestKey(const std::optional<core::Sha256Digest>& digest) {
    if (!digest)
        return "-";
    const auto text = digest->toLowercaseHex();
    return std::string(text.data(), text.size());
}

std::string packageKey(const std::filesystem::path& executable,
                       const GpuShaderToolPackage& package) {
    return executable.generic_string() + '\n' + package.toolsDirectory + '\n' +
           package.inventoryName + '\n' + package.glslangValidatorName + '\n' +
           package.spirvValName + '\n' + (package.relocated ? "1" : "0") +
           (package.bundleRelative ? "1" : "0") + '\n' + digestKey(package.glslangStagedDigest) +
           '\n' + digestKey(package.spirvValStagedDigest);
}

std::optional<GpuShaderToolResolveError> mapHashError(GpuShaderCompileError error) {
    switch (error) {
    case GpuShaderCompileError::Cancelled:
        return GpuShaderToolResolveError::Cancelled;
    case GpuShaderCompileError::Timeout:
        return GpuShaderToolResolveError::Timeout;
    case GpuShaderCompileError::IoFailure:
    case GpuShaderCompileError::InvalidTool:
    default:
        return GpuShaderToolResolveError::IoFailure;
    }
}

// Nonsensical limits are rejected before any filesystem work, so a warm cache lookup can trust the
// caller's ceilings and a zero/negative deadline never becomes an accidental immediate timeout.
std::optional<GpuShaderToolResolveError> validateLimits(const GpuShaderToolResolveLimits& limits) {
    if (limits.maxToolBytes == 0 || limits.maxInventoryBytes == 0 ||
        limits.deadline <= std::chrono::milliseconds::zero())
        return GpuShaderToolResolveError::LimitsInvalid;
    return std::nullopt;
}

} // namespace

struct GpuShaderToolResolver::State {
    std::mutex mutex;
    bool hasCached = false;
    std::string cacheKey;
    GpuShaderToolResolveResult cached;
    std::size_t cachedGlslangBytes = 0;
    std::size_t cachedSpirvBytes = 0;
    std::size_t cachedInventoryBytes = 0;
    GpuShaderToolResolverStats stats;
};

GpuShaderToolResolver::GpuShaderToolResolver() : state_(std::make_unique<State>()) {}
GpuShaderToolResolver::~GpuShaderToolResolver() = default;

void GpuShaderToolResolver::reset() {
    const std::lock_guard lock(state_->mutex);
    state_->hasCached = false;
    state_->cacheKey.clear();
    state_->cached = GpuShaderToolResolveResult{};
    state_->cachedGlslangBytes = 0;
    state_->cachedSpirvBytes = 0;
    state_->cachedInventoryBytes = 0;
}

GpuShaderToolResolverStats GpuShaderToolResolver::stats() const {
    const std::lock_guard lock(state_->mutex);
    return state_->stats;
}

GpuShaderToolResolveResult GpuShaderToolResolver::resolve(
    const std::filesystem::path& applicationExecutable, const GpuShaderToolPackage& package,
    const GpuShaderToolResolveLimits& limits, const GpuShaderCancellation& cancel) {
    if (cancel && cancel())
        return failed(GpuShaderToolResolveError::Cancelled, "resolution cancelled");
    if (const auto limitsError = validateLimits(limits))
        return failed(*limitsError, "resolution limits must be positive");

    if (package.toolsDirectory.empty() || package.inventoryName.empty() ||
        package.glslangValidatorName.empty() || package.spirvValName.empty())
        return failed(GpuShaderToolResolveError::Disabled, "no packaged shader tools configured");
    if (!singleSegment(package.toolsDirectory) || !singleSegment(package.inventoryName) ||
        !singleSegment(package.glslangValidatorName) || !singleSegment(package.spirvValName))
        return failed(GpuShaderToolResolveError::InvalidPackage,
                      "packaging descriptor names must be single path segments");
    if (!package.relocated && (!package.glslangStagedDigest || !package.spirvValStagedDigest))
        return failed(GpuShaderToolResolveError::InvalidPackage,
                      "a non-relocated package requires expected staged digests");
    if (applicationExecutable.empty() || !applicationExecutable.is_absolute() ||
        applicationExecutable.string().find('\0') != std::string::npos)
        return failed(GpuShaderToolResolveError::InvalidExecutable,
                      "application executable must be an absolute path");

    const std::string key = packageKey(applicationExecutable, package);
    const Clock::time_point deadline = Clock::now() + limits.deadline;
    {
        const std::lock_guard lock(state_->mutex);
        if (state_->hasCached && state_->cacheKey == key) {
            // A warm hit performs zero hashing but must still honor the current caller's ceilings
            // and deadline. The retained sizes are the validated staged byte counts.
            const std::size_t largestTool =
                std::max(state_->cachedGlslangBytes, state_->cachedSpirvBytes);
            if (largestTool > limits.maxToolBytes)
                return failed(GpuShaderToolResolveError::SizeLimit,
                              "cached tools exceed the current byte ceiling");
            if (state_->cachedInventoryBytes > limits.maxInventoryBytes)
                return failed(GpuShaderToolResolveError::SizeLimit,
                              "cached inventory exceeds the current byte ceiling");
            if (Clock::now() >= deadline)
                return failed(GpuShaderToolResolveError::Timeout,
                              "resolution deadline elapsed before the warm cache lookup");
            ++state_->stats.cacheHits;
            return state_->cached;
        }
    }

    // Failures are never cached: a cancellation or timeout is transient, so a later uncancelled
    // call with the same resolver and package must retry and can succeed.
    auto finishFailure = [&](GpuShaderToolResolveResult result) {
        const std::lock_guard lock(state_->mutex);
        ++state_->stats.resolves;
        return result;
    };

    if (!std::filesystem::is_regular_file(applicationExecutable))
        return finishFailure(failed(GpuShaderToolResolveError::InvalidExecutable,
                                    "application executable is not a regular file"));

    std::error_code error;
    const auto executableDirectory =
        std::filesystem::weakly_canonical(applicationExecutable.parent_path(), error);
    if (error)
        return finishFailure(failed(GpuShaderToolResolveError::IoFailure,
                                    "cannot canonicalize the executable directory"));
    const auto toolsDirectory =
        std::filesystem::weakly_canonical(executableDirectory / package.toolsDirectory, error);
    if (error || !isWithin(executableDirectory, toolsDirectory))
        return finishFailure(failed(GpuShaderToolResolveError::OutsideToolsDirectory,
                                    "packaged tools directory escapes the application directory"));
    if (!std::filesystem::is_directory(toolsDirectory))
        return finishFailure(
            failed(GpuShaderToolResolveError::ToolsMissing, "packaged tools directory is absent"));

    struct ToolCheck {
        const char* role;
        std::string name;
        core::Sha256Digest actual{};
        std::size_t bytes = 0;
    };
    ToolCheck glslang{"glslangValidator", package.glslangValidatorName, {}, 0};
    ToolCheck validator{"spirv-val", package.spirvValName, {}, 0};

    for (ToolCheck* tool : {&glslang, &validator}) {
        if (cancel && cancel())
            return finishFailure(
                failed(GpuShaderToolResolveError::Cancelled, "resolution cancelled"));
        const auto candidate = toolsDirectory / tool->name;
        const auto status = std::filesystem::symlink_status(candidate, error);
        if (error || status.type() == std::filesystem::file_type::not_found)
            return finishFailure(failed(GpuShaderToolResolveError::ToolsMissing,
                                        std::string("packaged tool is absent: ") + tool->name));
        if (std::filesystem::is_symlink(status))
            return finishFailure(failed(GpuShaderToolResolveError::OutsideToolsDirectory,
                                        std::string("packaged tool is a symlink: ") + tool->name));
        const auto canonical = std::filesystem::weakly_canonical(candidate, error);
        if (error || canonical.parent_path() != toolsDirectory ||
            !isWithin(toolsDirectory, canonical))
            return finishFailure(
                failed(GpuShaderToolResolveError::OutsideToolsDirectory,
                       std::string("packaged tool escapes its directory: ") + tool->name));
        if (!std::filesystem::is_regular_file(canonical, error) || error)
            return finishFailure(
                failed(GpuShaderToolResolveError::ToolsMissing,
                       std::string("packaged tool is not a regular file: ") + tool->name));
        const auto size = std::filesystem::file_size(canonical, error);
        if (error)
            return finishFailure(failed(GpuShaderToolResolveError::IoFailure,
                                        std::string("cannot stat packaged tool: ") + tool->name));
        if (size > limits.maxToolBytes)
            return finishFailure(
                failed(GpuShaderToolResolveError::SizeLimit,
                       std::string("packaged tool exceeds the byte ceiling: ") + tool->name));
        const auto hashed =
            hashFileBounded(canonical.string(), limits.maxToolBytes, deadline, cancel);
        if (!hashed.digest)
            return finishFailure(
                failed(mapHashError(hashed.error.value_or(GpuShaderCompileError::IoFailure))
                           .value_or(GpuShaderToolResolveError::IoFailure),
                       std::string("cannot hash packaged tool: ") + tool->name));
        tool->actual = hashed.digest.value();
        tool->bytes = static_cast<std::size_t>(size);
    }

    // The inventory is inside the owned directory exactly like the tools: a symlink that escapes,
    // a non-regular entry, or a canonical path outside the directory fails before any read.
    std::filesystem::path inventoryPath;
    bool inventoryPresent = false;
    {
        const auto candidate = toolsDirectory / package.inventoryName;
        const auto status = std::filesystem::symlink_status(candidate, error);
        if (!error && status.type() != std::filesystem::file_type::not_found) {
            if (std::filesystem::is_symlink(status))
                return finishFailure(failed(GpuShaderToolResolveError::OutsideToolsDirectory,
                                            "packaged inventory is a symlink"));
            const auto canonical = std::filesystem::weakly_canonical(candidate, error);
            if (error || canonical.parent_path() != toolsDirectory ||
                !isWithin(toolsDirectory, canonical))
                return finishFailure(failed(GpuShaderToolResolveError::OutsideToolsDirectory,
                                            "packaged inventory escapes its directory"));
            if (!std::filesystem::is_regular_file(canonical, error) || error)
                return finishFailure(failed(GpuShaderToolResolveError::InventoryInvalid,
                                            "packaged inventory is not a regular file"));
            inventoryPath = canonical;
            inventoryPresent = true;
        }
    }

    ParsedInventory inventory;
    std::size_t inventoryBytes = 0;
    if (package.relocated || inventoryPresent) {
        std::string text;
        if (!readBounded(inventoryPath, limits.maxInventoryBytes, text))
            return finishFailure(failed(GpuShaderToolResolveError::InventoryInvalid,
                                        "packaged inventory is missing or exceeds the ceiling"));
        inventoryBytes = text.size();
        if (!parseInventory(text, inventory) || inventory.format != kInventoryFormat)
            return finishFailure(failed(GpuShaderToolResolveError::InventoryInvalid,
                                        "packaged inventory is malformed"));
        if (inventory.layout != kInventoryLayout || inventory.directory != package.toolsDirectory)
            return finishFailure(failed(GpuShaderToolResolveError::InventoryMismatch,
                                        "packaged inventory layout disagrees with the descriptor"));
    }

    const auto checkAgainstInventory =
        [&](const ToolCheck& tool) -> std::optional<GpuShaderToolResolveError> {
        const auto found =
            std::find_if(inventory.tools.begin(), inventory.tools.end(),
                         [&](const ParsedTool& entry) { return entry.name == tool.role; });
        if (found == inventory.tools.end())
            return GpuShaderToolResolveError::InventoryMismatch;
        if (!found->hasStaged || found->stagedSha != tool.actual)
            return GpuShaderToolResolveError::InventoryMismatch;
        if (found->file != tool.name || found->source.empty() || found->source.front() == '/')
            return GpuShaderToolResolveError::InventoryMismatch;
        if (!package.relocated) {
            if (!found->hasSource)
                return GpuShaderToolResolveError::InventoryMismatch;
            if (found->sourceSha != found->stagedSha)
                return GpuShaderToolResolveError::SourceStagedMismatch;
            if (found->sourceSha != tool.actual)
                return GpuShaderToolResolveError::InventoryMismatch;
        }
        return std::nullopt;
    };

    if (package.relocated) {
        if (!inventoryPresent)
            return finishFailure(failed(GpuShaderToolResolveError::InventoryInvalid,
                                        "a relocated package requires an inventory"));
        for (const ToolCheck& tool : {glslang, validator}) {
            if (const auto mismatch = checkAgainstInventory(tool))
                return finishFailure(
                    failed(*mismatch, std::string("inventory disagrees for ") + tool.name));
        }
        if (package.glslangStagedDigest && *package.glslangStagedDigest != glslang.actual)
            return finishFailure(failed(GpuShaderToolResolveError::DigestMismatch,
                                        "staged glslangValidator digest disagrees"));
        if (package.spirvValStagedDigest && *package.spirvValStagedDigest != validator.actual)
            return finishFailure(failed(GpuShaderToolResolveError::DigestMismatch,
                                        "staged spirv-val digest disagrees"));
    } else {
        if (glslang.actual != *package.glslangStagedDigest ||
            validator.actual != *package.spirvValStagedDigest)
            return finishFailure(failed(GpuShaderToolResolveError::DigestMismatch,
                                        "staged tool bytes disagree with the descriptor"));
        if (inventoryPresent) {
            for (const ToolCheck& tool : {glslang, validator}) {
                if (const auto mismatch = checkAgainstInventory(tool))
                    return finishFailure(
                        failed(*mismatch, std::string("inventory disagrees for ") + tool.name));
            }
        }
    }

    GpuShaderToolResolveResult result;
    result.ok = true;
    result.paths.glslangValidator = (toolsDirectory / glslang.name).string();
    result.paths.spirvVal = (toolsDirectory / validator.name).string();
    {
        // Successful qualification only: cached with the validated staged sizes so a later warm
        // call can enforce tightened ceilings without rehashing.
        const std::lock_guard lock(state_->mutex);
        state_->hasCached = true;
        state_->cacheKey = key;
        state_->cached = result;
        state_->cachedGlslangBytes = glslang.bytes;
        state_->cachedSpirvBytes = validator.bytes;
        state_->cachedInventoryBytes = inventoryBytes;
        ++state_->stats.resolves;
    }
    return result;
}

} // namespace bloom::color
