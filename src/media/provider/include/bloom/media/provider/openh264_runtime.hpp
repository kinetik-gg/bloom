#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
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
    [[nodiscard]] OpenH264InstallResult locate(const std::filesystem::path& file) const;

  private:
    std::filesystem::path root_;
    Launcher launcher_;
};

} // namespace bloom::media::provider
