#include "support.hpp"

#include <bloom/media/provider/openh264_runtime.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace bloom::media::provider;

namespace {
std::filesystem::path root() { return std::filesystem::path(BLOOM_OPENH264_RUNTIME_TEST_ROOT); }

void clear(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::remove_all(path, error);
    test::check(!error, "clear OpenH264 test root");
}

// The OpenH264 runtime behaviours below are only reachable where the Cisco linux64.8 binary is
// supported (Linux x86_64). Elsewhere only the typed UnsupportedPlatform fallback is compiled, so
// these helpers are compiled out to avoid unused-function failures under -Werror.
#if defined(__linux__) && defined(__x86_64__)

void mismatchIsNotInstalled() {
    const auto directory = root() / "mismatch";
    clear(directory);
    std::filesystem::create_directories(directory);
    std::ofstream file(directory / OpenH264Runtime::libraryName(), std::ios::binary);
    file << "truncated";
    file.close();
    const auto status = OpenH264Runtime(directory).verify();
    test::check(!status.installed &&
                    status.failure == OpenH264RuntimeFailure::LibraryDigestMismatch,
                "digest mismatch is not installed");
}

void consentIsRequired() {
    const auto directory = root() / "consent";
    clear(directory);
    bool launched = false;
    OpenH264Runtime runtime(directory, [&](const std::string&, const std::vector<std::string>&,
                                           std::chrono::milliseconds) {
        launched = true;
        return false;
    });
    const auto result = runtime.install(false);
    test::check(!result.installed && result.failure == OpenH264RuntimeFailure::ConsentRequired,
                "missing consent refuses install");
    test::check(!launched, "missing consent does not launch a downloader");
}

void truncatedDownloadIsTyped() {
    const auto directory = root() / "truncated";
    clear(directory);
    auto launcher = [](const std::string& executable, const std::vector<std::string>& arguments,
                       std::chrono::milliseconds) {
        if (executable == "/usr/bin/curl") {
            const auto output = std::ranges::find(arguments, "--output");
            if (output == arguments.end() || std::next(output) == arguments.end())
                return false;
            std::ofstream file(*std::next(output), std::ios::binary);
            file << "truncated Cisco archive";
            return static_cast<bool>(file);
        }
        return false;
    };
    const auto result = OpenH264Runtime(directory, launcher).install(true);
    test::check(!result.installed &&
                    result.failure == OpenH264RuntimeFailure::ArchiveDigestMismatch,
                "truncated download has a typed digest failure");
}

void emptyDirectoryIsNotInstalled() {
    const auto directory = root() / "empty";
    clear(directory);
    const auto status = OpenH264Runtime(directory).verify();
    test::check(!status.installed && status.failure == OpenH264RuntimeFailure::NotInstalled,
                "empty OpenH264 directory is not installed");
}

#endif // Linux x86_64 OpenH264 runtime behaviours

void licenseTextMatchesCiscoFixture() {
    std::ifstream file(BLOOM_OPENH264_LICENSE_FIXTURE, std::ios::binary);
    if (!file)
        return;
    const std::string expected((std::istreambuf_iterator<char>(file)),
                               std::istreambuf_iterator<char>());
    test::check(expected == OpenH264Runtime::binaryLicenseText(),
                "embedded Cisco licence text matches the downloaded terms");
}

// Every other behaviour in this file is only reachable where the Cisco OpenH264 runtime is
// supported (Linux x86_64). Elsewhere the declared contract is a typed UnsupportedPlatform result
// that never launches a downloader -- asserted here instead of skipped, so the fallback is tested.
void unsupportedPlatformIsTyped() {
    const auto directory = root() / "unsupported";
    clear(directory);
    const auto status = OpenH264Runtime(directory).verify();
    test::check(!status.installed && status.failure == OpenH264RuntimeFailure::UnsupportedPlatform,
                "verify reports the unsupported platform");
    bool launched = false;
    OpenH264Runtime runtime(directory, [&](const std::string&, const std::vector<std::string>&,
                                           std::chrono::milliseconds) {
        launched = true;
        return false;
    });
    const auto result = runtime.install(true);
    test::check(!result.installed && result.failure == OpenH264RuntimeFailure::UnsupportedPlatform,
                "install reports the unsupported platform");
    test::check(!launched, "the unsupported platform never launches a downloader");
}
} // namespace

int main() {
    try {
        clear(root());
        licenseTextMatchesCiscoFixture();
#if defined(__linux__) && defined(__x86_64__)
        emptyDirectoryIsNotInstalled();
        mismatchIsNotInstalled();
        consentIsRequired();
        truncatedDownloadIsTyped();
#else
        unsupportedPlatformIsTyped();
#endif
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
