// Bloom BlendV1 SPIR-V embed generator (offline, standalone).
//
// Deterministic compile/validate/disassemble/embed recipe for the hand-written `blend*.comp`
// kernels. It compiles one `.comp` with the pinned `glslangValidator`, validates the result with
// the pinned `spirv-val`, disassembles it with the pinned `spirv-dis` to assert (for the portable
// kernels) that no Float64/Int64 capability or 64-bit type is declared, then writes the checked-in
// SPIR-V `.inc` array and updates the manifest pins -- or, with `--check`, verifies the existing
// `.inc` and manifest already match the freshly compiled artifact without writing anything.
//
// The output is an offline artifact. This tool does not execute the shader, and the artifacts
// contain no machine paths or timestamps.

#include <bloom/core/sha256.hpp>

#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>
#endif

namespace {

[[nodiscard]] std::string readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open for reading: " + path.string());
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

void writeFile(const std::filesystem::path& path, const std::string& contents) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot open for writing: " + path.string());
    }
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    output.flush();
    if (!output) {
        throw std::runtime_error("failed writing: " + path.string());
    }
}

[[nodiscard]] std::string digestToHex(const bloom::core::Sha256Digest& digest) {
    const auto hex = digest.toLowercaseHex();
    return std::string(hex.data(), hex.size());
}

[[nodiscard]] std::string sha256Hex(const std::span<const std::byte> bytes) {
    const auto digest = bloom::core::Sha256Hasher::hash(bytes);
    if (!digest.has_value()) {
        throw std::runtime_error("SHA-256 input exceeds the encodable message length");
    }
    return digestToHex(*digest);
}

[[nodiscard]] std::string sha256HexFile(const std::filesystem::path& path) {
    const std::string bytes = readFile(path);
    return sha256Hex(std::as_bytes(std::span(bytes.data(), bytes.size())));
}

// The temporary working directory is created beside the system temp root and removed on scope exit.
struct TempDir final {
    std::filesystem::path path;

    TempDir() = default;
    explicit TempDir(std::filesystem::path value) : path(std::move(value)) {}
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&& other) noexcept : path(std::move(other.path)) { other.path.clear(); }
    TempDir& operator=(TempDir&& other) noexcept {
        if (this != &other) {
            cleanup();
            path = std::move(other.path);
            other.path.clear();
        }
        return *this;
    }
    ~TempDir() { cleanup(); }

    void cleanup() noexcept {
        if (!path.empty()) {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    }
};

[[nodiscard]] TempDir makeTempDir() {
    const auto unique = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::path path =
        std::filesystem::temp_directory_path() / ("bloom_blend_spirv_" + unique);
    std::filesystem::create_directories(path);
    return TempDir(path);
}

[[nodiscard]] std::string shellQuote(const std::string& value) {
#ifdef _WIN32
    std::string out = "\"";
    for (const char character : value) {
        if (character == '"') {
            out += "\\\"";
        } else {
            out += character;
        }
    }
    out += "\"";
    return out;
#else
    std::string out = "'";
    for (const char character : value) {
        if (character == '\'') {
            out += "'\\''";
        } else {
            out += character;
        }
    }
    out += "'";
    return out;
#endif
}

[[nodiscard]] int runTool(const std::vector<std::string>& arguments) {
    std::string command;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        if (index != 0) {
            command.push_back(' ');
        }
        command += shellQuote(arguments[index]);
    }
    const int status = std::system(command.c_str());
#ifdef _WIN32
    return status;
#else
    if (status == -1) {
        return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

[[nodiscard]] std::string symbolPrefix(const std::string_view stem) {
    // Mirrors the checked-in names: blend -> kBlendSpirv, blend_f64 -> kBlendF64Spirv,
    // blend_portable -> kBlendPortableSpirv.
    std::string out = "k";
    bool atPartStart = true;
    for (const char character : stem) {
        if (character == '_') {
            atPartStart = true;
            continue;
        }
        const auto value = static_cast<unsigned char>(character);
        out.push_back(static_cast<char>(atPartStart ? std::toupper(value) : std::tolower(value)));
        atPartStart = false;
    }
    out += "Spirv";
    return out;
}

[[nodiscard]] std::string hex8(const std::uint32_t value) {
    static constexpr char kDigits[] = "0123456789ABCDEF";
    std::string out(8, '0');
    std::uint32_t remaining = value;
    for (int index = 7; index >= 0; --index) {
        out[static_cast<std::size_t>(index)] = kDigits[remaining & 0xFU];
        remaining >>= 4U;
    }
    return out;
}

[[nodiscard]] std::vector<std::uint32_t> readWords(const std::filesystem::path& path) {
    const std::string bytes = readFile(path);
    if (bytes.size() % 4U != 0U) {
        throw std::runtime_error("SPIR-V byte count is not a whole number of words: " +
                                 path.string());
    }
    std::vector<std::uint32_t> words(bytes.size() / 4U);
    for (std::size_t index = 0; index < words.size(); ++index) {
        const auto* base = reinterpret_cast<const unsigned char*>(bytes.data()) + index * 4U;
        words[index] = static_cast<std::uint32_t>(base[0]) |
                       (static_cast<std::uint32_t>(base[1]) << 8U) |
                       (static_cast<std::uint32_t>(base[2]) << 16U) |
                       (static_cast<std::uint32_t>(base[3]) << 24U);
    }
    return words;
}

[[nodiscard]] std::string renderInc(const std::string_view stem, const std::string& compSha,
                                    const std::string& spvSha,
                                    const std::vector<std::uint32_t>& words) {
    const std::string prefix = symbolPrefix(stem);
    const std::string byteCount = std::to_string(words.size() * 4U);
    const std::string wordCount = std::to_string(words.size());
    std::ostringstream out;
    out << "// Generated from " << stem << ".comp; do not edit by hand.\n";
    out << "//\n";
    out << "// Source shader: " << stem << ".comp\n";
    out << "//   sha256 " << compSha << "\n";
    out << "// SPIR-V " << stem << ".spv\n";
    out << "//   sha256 " << spvSha << "\n";
    out << "//   byte count " << byteCount << "\n";
    out << "//   word count " << wordCount << "\n";
    out << "//\n";
    out << "// SPIR-V as deterministic little-endian 32-bit words, produced by\n";
    out << "// glslangValidator --target-env vulkan1.2 -V and validated by spirv-val. No runtime "
           "shader load.\n";
    out << "\n";
    out << "#pragma once\n";
    out << "\n";
    out << "#include <cstdint>\n";
    out << "\n";
    out << "namespace bloom::render::vulkan_detail {\n";
    out << "\n";
    out << "inline constexpr std::uint32_t " << prefix << "ByteCount = " << byteCount << "U;\n";
    out << "inline constexpr std::uint32_t " << prefix << "WordCount = " << wordCount << "U;\n";
    out << "inline constexpr char " << prefix << "Digest[] = \"" << spvSha << "\";\n";
    out << "inline constexpr std::uint32_t " << prefix << "Code[" << prefix << "WordCount] = {\n";
    for (std::size_t index = 0; index < words.size(); index += 4U) {
        out << "    ";
        for (std::size_t lane = 0; lane < 4U && index + lane < words.size(); ++lane) {
            if (lane != 0U) {
                out << ' ';
            }
            out << "0x" << hex8(words[index + lane]) << "u,";
        }
        out << "\n";
    }
    out << "};\n";
    out << "\n";
    out << "} // namespace bloom::render::vulkan_detail\n";
    return out.str();
}

// Replaces the 64-hex value of the first `"<key>": "..."` field, matching the former tool's
// manifest-pin update without reformatting any other byte of the hand-written manifest.
[[nodiscard]] bool replaceDigestField(std::string& text, const std::string_view key,
                                      const std::string& digest) {
    const std::string marker = "\"" + std::string(key) + "\"";
    const auto position = text.find(marker);
    if (position == std::string::npos) {
        return false;
    }
    auto cursor = position + marker.size();
    while (cursor < text.size() && text[cursor] != ':') {
        ++cursor;
    }
    if (cursor == text.size()) {
        return false;
    }
    ++cursor;
    while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor])) != 0) {
        ++cursor;
    }
    if (cursor >= text.size() || text[cursor] != '"') {
        return false;
    }
    const auto start = cursor + 1;
    const auto end = start + 64;
    if (end >= text.size() || text[end] != '"') {
        return false;
    }
    for (auto index = start; index < end; ++index) {
        if (std::isxdigit(static_cast<unsigned char>(text[index])) == 0) {
            return false;
        }
    }
    text.replace(start, 64, digest);
    return true;
}

[[nodiscard]] bool containsForbidden64Bit(const std::string& disassembly) {
    return disassembly.find("OpCapability Float64") != std::string::npos ||
           disassembly.find("OpCapability Int64") != std::string::npos ||
           disassembly.find("OpTypeFloat 64") != std::string::npos ||
           disassembly.find("OpTypeInt 64") != std::string::npos;
}

struct Options final {
    std::filesystem::path comp;
    std::filesystem::path manifest;
    std::filesystem::path inc;
    std::string glslang;
    std::string spirvVal;
    std::string spirvDis;
    std::string targetEnv = "vulkan1.2";
    bool forbid64Bit = false;
    bool check = false;
};

void printUsage() {
    std::cerr << "usage: bloom_gpu_blend_spirv_generator --comp <file.comp> --manifest "
                 "<file.manifest> --inc <file_spirv.inc> --glslang <path> --spirv-val <path> "
                 "--spirv-dis <path> [--target-env vulkan1.2] [--forbid-64bit] [--check]\n";
}

[[nodiscard]] bool parseOptions(const int argc, char** argv, Options& options) {
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        const auto value = [&](std::string& destination) {
            if (index + 1 >= argc) {
                return false;
            }
            destination = argv[++index];
            return true;
        };
        std::string text;
        if (argument == "--comp" && value(text)) {
            options.comp = text;
        } else if (argument == "--manifest" && value(text)) {
            options.manifest = text;
        } else if (argument == "--inc" && value(text)) {
            options.inc = text;
        } else if (argument == "--glslang" && value(text)) {
            options.glslang = text;
        } else if (argument == "--spirv-val" && value(text)) {
            options.spirvVal = text;
        } else if (argument == "--spirv-dis" && value(text)) {
            options.spirvDis = text;
        } else if (argument == "--target-env" && value(text)) {
            options.targetEnv = text;
        } else if (argument == "--forbid-64bit") {
            options.forbid64Bit = true;
        } else if (argument == "--check") {
            options.check = true;
        } else {
            std::cerr << "unrecognized argument: " << argument << "\n";
            return false;
        }
    }
    return !options.comp.empty() && !options.manifest.empty() && !options.inc.empty() &&
           !options.glslang.empty() && !options.spirvVal.empty() && !options.spirvDis.empty();
}

[[nodiscard]] std::string requiredTool(const std::string& path, const char* name) {
    if (path.empty()) {
        throw std::runtime_error(std::string(name) + " path is required");
    }
    return path;
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (argc <= 1) {
        printUsage();
        return 2;
    }
    if (!parseOptions(argc, argv, options)) {
        printUsage();
        return 2;
    }

    try {
        const std::string stem = options.comp.stem().string();
        const std::string compSha = sha256HexFile(options.comp);
        const std::string glslang = requiredTool(options.glslang, "--glslang");
        const std::string spirvVal = requiredTool(options.spirvVal, "--spirv-val");
        const std::string spirvDis = requiredTool(options.spirvDis, "--spirv-dis");

        TempDir temp = makeTempDir();
        const std::filesystem::path spv = temp.path / (stem + ".spv");
        const std::filesystem::path asmPath = temp.path / (stem + ".spvasm");

        if (runTool({glslang, "--target-env", options.targetEnv, "-V", options.comp.string(), "-o",
                     spv.string()}) != 0) {
            throw std::runtime_error("glslangValidator failed to compile " + options.comp.string());
        }
        if (runTool({spirvVal, "--target-env", options.targetEnv, spv.string()}) != 0) {
            throw std::runtime_error("spirv-val rejected the regenerated " + stem + " SPIR-V");
        }
        if (runTool({spirvDis, spv.string(), "-o", asmPath.string()}) != 0) {
            throw std::runtime_error("spirv-dis failed to disassemble the " + stem + " SPIR-V");
        }
        if (options.forbid64Bit && containsForbidden64Bit(readFile(asmPath))) {
            throw std::runtime_error(
                stem + ".comp requires a forbidden Float64/Int64 capability or 64-bit "
                       "type; it must run on standard Float32 hardware");
        }

        const std::string spvSha = sha256HexFile(spv);
        const std::string generated = renderInc(stem, compSha, spvSha, readWords(spv));

        if (options.check) {
            if (readFile(options.inc) != generated) {
                throw std::runtime_error(options.inc.string() +
                                         " is stale; regenerate it with this tool");
            }
            const std::string manifest = readFile(options.manifest);
            if (manifest.find(compSha) == std::string::npos) {
                throw std::runtime_error(options.manifest.string() + " does not bind the " + stem +
                                         ".comp SHA-256");
            }
            if (manifest.find(spvSha) == std::string::npos) {
                throw std::runtime_error(options.manifest.string() + " does not bind the " + stem +
                                         " SPIR-V SHA-256");
            }
            std::cout << stem << ": up to date (comp " << compSha << ", spirv " << spvSha << ")\n";
            return 0;
        }

        writeFile(options.inc, generated);
        std::string manifest = readFile(options.manifest);
        if (!replaceDigestField(manifest, "sha256", compSha) ||
            !replaceDigestField(manifest, "spirvSha256", spvSha)) {
            throw std::runtime_error(options.manifest.string() +
                                     " does not expose the expected sha256/spirvSha256 pins");
        }
        writeFile(options.manifest, manifest);
        std::cout << stem << ": wrote " << options.inc.string() << "\n";
        std::cout << "  COMP_SHA256 \"" << compSha << "\"\n";
        std::cout << "  SPV_SHA256  \"" << spvSha << "\"\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "embed generator failed: " << error.what() << "\n";
        return 1;
    }
}
