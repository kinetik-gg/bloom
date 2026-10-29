#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace bloom::media::provider {

enum class OpenH264RuntimeFailure {
    None,
    UnsupportedPlatform,
    ConsentRequired,
    DownloadFailed,
    ArchiveDigestMismatch,
    LibraryDigestMismatch,
    InstallFailed,
    NotInstalled,
};

struct OpenH264RuntimeStatus final {
    bool installed = false;
    OpenH264RuntimeFailure failure = OpenH264RuntimeFailure::NotInstalled;
    std::string version;
    std::string digest;
    std::filesystem::path directory;
    std::string detail;
};

struct OpenH264InstallResult final {
    bool installed = false;
    OpenH264RuntimeFailure failure = OpenH264RuntimeFailure::None;
    std::filesystem::path directory;
    std::string detail;
};

// Host-side, Qt-free policy object for Cisco's runtime binary. This class never bundles or loads
// OpenH264. It only verifies a user-owned binary and supervises curl/bunzip2 for an explicit,
// consented install.
class OpenH264Runtime final {
  public:
    using Launcher = std::function<bool(const std::string&, const std::vector<std::string>&,
                                        std::chrono::milliseconds)>;
    explicit OpenH264Runtime(std::filesystem::path root = {}, Launcher launcher = {});

    // Cisco publishes the 2.6.0 binary Bloom verifies for Linux x86-64 only.
    [[nodiscard]] static constexpr bool platformSupported() noexcept {
#if defined(__linux__) && defined(__x86_64__)
        return true;
#else
        return false;
#endif
    }
    [[nodiscard]] static std::filesystem::path defaultRoot();
    [[nodiscard]] static std::string version();
    [[nodiscard]] static std::string libraryName();
    [[nodiscard]] static std::string libraryDigest();
    [[nodiscard]] static std::string archiveDigest();
    [[nodiscard]] static std::string downloadUrl();
    [[nodiscard]] static std::string binaryLicenseUrl();
    [[nodiscard]] static std::string binaryLicenseText();

    [[nodiscard]] OpenH264RuntimeStatus verify() const;
    [[nodiscard]] OpenH264InstallResult
    install(bool explicitConsent, const std::function<void(std::uint64_t)>& progress = {}) const;
    // Verifies and installs a binary the artist downloaded. The file is hashed and copied in one
    // pass into a staging file beside the install, so the artist's own file is never moved; a
    // true `cancelled()` between chunks stops it and removes the staging file.
    [[nodiscard]] OpenH264InstallResult locate(const std::filesystem::path& file,
                                               const std::function<bool()>& cancelled = {}) const;

    // `Bloom.conf` text with exactly one `[media]` `openh264-consent=<version>` entry: an existing
    // entry (or earlier duplicates) is replaced, every other line kept.
    [[nodiscard]] static std::string withConsentRecorded(std::string_view existing);

  private:
    std::filesystem::path root_;
    Launcher launcher_;
};

} // namespace bloom::media::provider
