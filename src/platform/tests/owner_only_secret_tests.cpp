// bloom.platform.owner-only-secret (ADR 0034, section 10): a secret is created owner-only, an
// exposed file is reported and replaced, an atomic replace leaves no partial or temporary file, and
// the value never appears in any message.
#include <bloom/platform/owner_only_secret.hpp>

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

namespace {

using namespace bloom::platform;
namespace fs = std::filesystem;

int failures = 0;
void expect(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAILED: " << message << '\n';
    }
}

fs::path scratchFolder() {
    std::array<std::byte, 8> random{};
    expect(fillSecureRandom(random), "the system random source works");
    std::string name = "bloom-secret-test-";
    for (const auto byte : random)
        name += std::to_string(std::to_integer<unsigned>(byte) % 10U);
    return fs::temp_directory_path() / name;
}

std::size_t entries(const fs::path& folder) {
    std::size_t count = 0;
    for ([[maybe_unused]] const auto& entry : fs::directory_iterator(folder))
        ++count;
    return count;
}

} // namespace

int main() {
    const auto root = scratchFolder();
    const auto file = root / "mcp" / "token";
    const std::string first = "bloom_first-secret-value-0123456789abcdefghijklmnop";
    const std::string second = "bloom_second-secret-value-0123456789abcdefghijklmno";
    const auto quiet = [&](const std::string& message) {
        return message.find(first) == std::string::npos &&
               message.find(second) == std::string::npos;
    };

    std::array<std::byte, 32> a{};
    std::array<std::byte, 32> b{};
    expect(fillSecureRandom(a) && fillSecureRandom(b) && a != b, "two CSPRNG draws differ");

    const auto missing = readOwnerOnlySecret(file, 256);
    expect(missing.status == OwnerOnlySecretStatus::Missing && missing.value.empty(),
           "a missing secret is Missing");

    const auto written = writeOwnerOnlySecret(file, first);
    expect(written.status == OwnerOnlySecretStatus::Ok && quiet(written.message),
           "the secret is written");
    const auto read = readOwnerOnlySecret(file, 256);
    expect(read.status == OwnerOnlySecretStatus::Ok && read.value == first,
           "the secret reads back");
    expect(readOwnerOnlySecret(file, 8).status == OwnerOnlySecretStatus::Malformed,
           "a secret above the caller's limit is Malformed");
    expect(writeOwnerOnlySecret(file, "has space").status == OwnerOnlySecretStatus::Malformed,
           "a non-printable secret is refused");

#if !defined(_WIN32)
    struct stat status{};
    expect(::stat(file.c_str(), &status) == 0 && (status.st_mode & 0777U) == 0600U,
           "the file is created 0600");
    expect(::stat(file.parent_path().c_str(), &status) == 0 && (status.st_mode & 0777U) == 0700U,
           "the folder is created 0700");
    // Another user could read it: reported without the value, then replaced owner-only.
    fs::permissions(file, fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read |
                              fs::perms::others_read);
    const auto exposed = readOwnerOnlySecret(file, 256);
    expect(exposed.status == OwnerOnlySecretStatus::ExposedToOtherUsers && exposed.value.empty() &&
               quiet(exposed.message),
           "an exposed secret is reported without its value");
    const auto replaced = writeOwnerOnlySecret(file, second);
    expect(replaced.status == OwnerOnlySecretStatus::Ok, "the exposed secret is replaced");
    expect(::stat(file.c_str(), &status) == 0 && (status.st_mode & 0777U) == 0600U,
           "the replacement is 0600");
#else
    const auto replaced = writeOwnerOnlySecret(file, second);
    expect(replaced.status == OwnerOnlySecretStatus::Ok, "the secret is replaced");
#endif
    const auto again = readOwnerOnlySecret(file, 256);
    expect(again.status == OwnerOnlySecretStatus::Ok && again.value == second,
           "the replacement reads back whole");
    expect(entries(file.parent_path()) == 1, "no temporary or partial file is left behind");

    {
        std::ofstream garbage(root / "mcp" / "other", std::ios::binary);
        garbage << "a\x01"
                   "b";
    }
#if !defined(_WIN32)
    fs::permissions(root / "mcp" / "other", fs::perms::owner_read | fs::perms::owner_write);
    const auto malformed = readOwnerOnlySecret(root / "mcp" / "other", 256);
    expect(malformed.status == OwnerOnlySecretStatus::Malformed && quiet(malformed.message),
           "control bytes are Malformed");
#endif

    std::error_code ignored;
    fs::remove_all(root, ignored);
    if (failures == 0)
        std::cout << "owner-only secret: creation, exposure, atomic replace and quiet messages "
                     "passed\n";
    return failures == 0 ? 0 : 1;
}
