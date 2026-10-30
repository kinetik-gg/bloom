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
                "embedded Cisco license text matches the downloaded terms");
}

// Consent is recorded once in Bloom.conf, however often Bloom installs, and earlier duplicated
// groups (written by appending) collapse; every other line is kept.
void consentIsRecordedOnce() {
    const auto count = [](const std::string& text, const std::string& needle) {
        std::size_t found = 0;
        for (auto at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1))
            ++found;
        return found;
    };
    const auto fresh = OpenH264Runtime::withConsentRecorded({});
    test::check(fresh == "[media]\nopenh264-consent=2.6.0\n", "a new file gets one entry");
    const auto again = OpenH264Runtime::withConsentRecorded(fresh);
    test::check(again == fresh, "recording consent twice changes nothing");
    const auto legacy = OpenH264Runtime::withConsentRecorded(
        "[general]\nname=1\n[media]\nopenh264-consent=2.6.0\n[media]\nopenh264-consent=2.6.0\n"
        "[media]\nother=2\n");
    test::check(count(legacy, "[media]") == 1 && count(legacy, "openh264-consent=") == 1 &&
                    legacy.find("[general]\nname=1\n") != std::string::npos &&
                    legacy.find("other=2") != std::string::npos,
                "appended duplicate groups collapse to one entry and keep other values");
}

// Locate hashes and copies in one pass: a wrong file is refused without moving the artist's own
// file, and a cancelled Locate installs nothing.
void locateKeepsTheSourceAndCancels() {
    const auto directory = root() / "locate";
    clear(directory);
    std::filesystem::create_directories(directory);
    const auto source = directory / "downloaded.so";
    {
        std::ofstream file(source, std::ios::binary);
        file << "not the Cisco binary";
    }
    const auto install = directory / "install" / "openh264" / "2.6.0";
    const auto refused = OpenH264Runtime(install).locate(source);
    test::check(!refused.installed &&
                    refused.failure == OpenH264RuntimeFailure::LibraryDigestMismatch,
                "a wrong file is refused by digest");
    test::check(std::filesystem::exists(source), "the artist's file is never moved");
    test::check(!std::filesystem::exists(install / OpenH264Runtime::libraryName()),
                "nothing is installed");
    const auto cancelled = OpenH264Runtime(install).locate(source, [] { return true; });
    test::check(!cancelled.installed && cancelled.detail.find("cancelled") != std::string::npos,
                "a cancelled Locate installs nothing");
    test::check(std::filesystem::is_empty(install), "a cancelled Locate leaves no staging file");
}

// Every other behaviour in this file is only reachable where the Cisco OpenH264 runtime is
// supported (Linux x86_64). Elsewhere the declared contract is a typed UnsupportedPlatform result
// that never launches a downloader -- asserted here instead of skipped, so the fallback is tested.
#if !defined(__linux__) || !defined(__x86_64__)
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
#endif // !Linux x86_64 only: the typed unsupported-platform fallback
} // namespace

int main() {
    try {
        clear(root());
        licenseTextMatchesCiscoFixture();
        consentIsRecordedOnce();
        locateKeepsTheSourceAndCancels();
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
