#include <bloom/media/provider/openh264_runtime.hpp>

#include <bloom/core/sha256.hpp>
#include <bloom/platform/process_supervisor.hpp>
#include <bloom/platform/user_data_directory.hpp>

#include <chrono>
#include <fstream>
#include <random>
#include <system_error>

namespace bloom::media::provider {
namespace {
constexpr std::string_view kVersion = "2.6.0";
constexpr std::string_view kLibrary = "libopenh264.so.8";
constexpr std::string_view kArchiveDigest =
    "27ab53323c110b76214c1c72222f459d17febbcd1e252136cadc292b0308d75b";
constexpr std::string_view kLibraryDigest =
    "2f0cde7c6a6abcf5cae76942894ea42897fa677bce4ed6c91a24dd1b041d5f04";
[[maybe_unused]] constexpr std::string_view kLicenseDigest =
    "bd9f363c5ea11ef723d0304cddacb5273c43c0e1194097c7a045d05273635418";
constexpr std::string_view kUrl =
    "https://ciscobinary.openh264.org/libopenh264-2.6.0-linux64.8.so.bz2";
constexpr std::string_view kLicenseUrl = "https://www.openh264.org/BINARY_LICENSE.txt";
constexpr std::string_view kLicenseText =
    R"LICENSE(-------------------------------------------------------
About The Cisco-Provided Binary of OpenH264 Video Codec
-------------------------------------------------------

Cisco provides this program under the terms of the BSD license.  

Additionally, this binary is licensed under Cisco’s AVC/H.264 Patent Portfolio License from MPEG LA, at no cost to you, provided that the requirements and conditions shown below in the AVC/H.264 Patent Portfolio sections are met.  

As with all AVC/H.264 codecs, you may also obtain your own patent license from MPEG LA or from the individual patent owners, or proceed at your own risk.  Your rights from Cisco under the BSD license are not affected by this choice.  

For more information on the OpenH264 binary licensing, please see the OpenH264 FAQ found at http://www.openh264.org/faq.html#binary 

A corresponding source code to this binary program is available under the same BSD terms, which can be found at http://www.openh264.org

-----------
BSD License
-----------

Copyright © 2014 Cisco Systems, Inc.

All rights reserved.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS “AS IS” AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

-----------------------------------------
AVC/H.264 Patent Portfolio License Notice
-----------------------------------------

The binary form of this Software is distributed by Cisco under the AVC/H.264 Patent Portfolio License from MPEG LA, and is subject to the following requirements, which may or may not be applicable to your use of this software: 

THIS PRODUCT IS LICENSED UNDER THE AVC PATENT PORTFOLIO LICENSE FOR THE PERSONAL USE OF A CONSUMER OR OTHER USES IN WHICH IT DOES NOT RECEIVE REMUNERATION TO (i) ENCODE VIDEO IN COMPLIANCE WITH THE AVC STANDARD (“AVC VIDEO”) AND/OR (ii) DECODE AVC VIDEO THAT WAS ENCODED BY A CONSUMER ENGAGED IN A PERSONAL ACTIVITY AND/OR WAS OBTAINED FROM A VIDEO PROVIDER LICENSED TO PROVIDE AVC VIDEO.  NO LICENSE IS GRANTED OR SHALL BE IMPLIED FOR ANY OTHER USE.  ADDITIONAL INFORMATION MAY BE OBTAINED FROM MPEG LA, L.L.C. SEE HTTP://WWW.MPEGLA.COM

Accordingly, please be advised that content providers and broadcasters using AVC/H.264 in their service may be required to obtain a separate use license from MPEG LA, referred to as "(b) sublicenses" in the SUMMARY OF AVC/H.264 LICENSE TERMS from MPEG LA found at http://www.openh264.org/mpegla

---------------------------------------------
AVC/H.264 Patent Portfolio License Conditions
---------------------------------------------

In addition, the Cisco-provided binary of this Software is licensed under Cisco's license from MPEG LA only if the following conditions are met:

1. The Cisco-provided binary is separately downloaded to an end user’s device, and not integrated into or combined with third party software prior to being downloaded to the end user’s device;

2. The end user must have the ability to control (e.g., to enable, disable, or re-enable) the use of the Cisco-provided binary;

3. Third party software, in the location where end users can control the use of the Cisco-provided binary, must display the following text:

       "OpenH264 Video Codec provided by Cisco Systems, Inc."

4.  Any third-party software that makes use of the Cisco-provided binary must reproduce all of the above text, as well as this last condition, in the EULA and/or in another location where licensing information is to be presented to the end user.
 


                          v1.0
)LICENSE";

std::string digestFile(const std::filesystem::path& file) {
    std::ifstream stream(file, std::ios::binary);
    if (!stream)
        return {};
    core::Sha256Hasher hasher;
    std::array<std::byte, static_cast<std::size_t>(64U * 1024U)> buffer{};
    while (stream) {
        stream.read(reinterpret_cast<char*>(buffer.data()),
                    static_cast<std::streamsize>(buffer.size()));
        const auto count = stream.gcount();
        if (count > 0 && !hasher.update(std::span(buffer).first(static_cast<std::size_t>(count))))
            return {};
    }
    if (!stream.eof())
        return {};
    const auto hex = hasher.finalize().toLowercaseHex();
    return std::string(hex.data(), hex.size());
}

bool publish(const std::filesystem::path& source, const std::filesystem::path& destination) {
    std::error_code error;
#if defined(_WIN32)
    // std::filesystem::rename cannot replace an existing file on Windows. The destination is
    // removed only on that platform; POSIX rename is the atomic replacement operation.
    std::filesystem::remove(destination, error);
#endif
    std::filesystem::rename(source, destination, error);
    return !error;
}

OpenH264InstallResult failure(const OpenH264RuntimeFailure reason, std::string detail,
                              std::filesystem::path directory) {
    return {.installed = false,
            .failure = reason,
            .directory = std::move(directory),
            .detail = std::move(detail)};
}
} // namespace

OpenH264Runtime::OpenH264Runtime(std::filesystem::path root, Launcher launcher)
    : root_(root.empty() ? defaultRoot() : std::move(root)), launcher_(std::move(launcher)) {}

std::filesystem::path OpenH264Runtime::defaultRoot() {
    if (const auto data = platform::userDataDirectory())
        return *data / "openh264" / std::string(kVersion);
    return {};
}
std::string OpenH264Runtime::version() { return std::string(kVersion); }
std::string OpenH264Runtime::libraryName() { return std::string(kLibrary); }
std::string OpenH264Runtime::libraryDigest() { return std::string(kLibraryDigest); }
std::string OpenH264Runtime::archiveDigest() { return std::string(kArchiveDigest); }
std::string OpenH264Runtime::downloadUrl() { return std::string(kUrl); }
std::string OpenH264Runtime::binaryLicenseUrl() { return std::string(kLicenseUrl); }
std::string OpenH264Runtime::binaryLicenseText() {
    std::string text(kLicenseText);
    constexpr std::string_view marker =
        "4.  Any third-party software that makes use of the Cisco-provided binary must reproduce "
        "all of the above text, as well as this last condition, in the EULA and/or in another "
        "location where licensing information is to be presented to the end user.\n";
    const auto position = text.find(marker);
    if (position != std::string::npos)
        text.replace(position + marker.size() - 1, 1, "  \n");
    return text;
}

OpenH264RuntimeStatus OpenH264Runtime::verify() const {
    OpenH264RuntimeStatus result{.installed = false,
                                 .failure = OpenH264RuntimeFailure::NotInstalled,
                                 .version = version(),
                                 .digest = {},
                                 .directory = root_,
                                 .detail = {}};
#if !defined(__linux__) || !defined(__x86_64__)
    result.failure = OpenH264RuntimeFailure::UnsupportedPlatform;
    result.detail = "Cisco OpenH264 2.6.0 linux64.8 is not available for this platform";
    return result;
#else
    if (root_.empty()) {
        result.detail = "Per-user data directory is unavailable";
        return result;
    }
    const auto library = root_ / std::string(kLibrary);
    if (!std::filesystem::is_regular_file(library)) {
        result.detail = "H.264 encoder not installed";
        return result;
    }
    const auto digest = digestFile(library);
    result.digest = digest;
    if (digest != kLibraryDigest) {
        result.failure = OpenH264RuntimeFailure::LibraryDigestMismatch;
        result.detail = "Installed OpenH264 binary digest does not match Cisco's verified release";
        return result;
    }
    const auto license = root_ / "BINARY_LICENSE.txt";
    if (!std::filesystem::is_regular_file(license) || digestFile(license) != kLicenseDigest) {
        result.failure = OpenH264RuntimeFailure::NotInstalled;
        result.detail = "OpenH264 binary licence is missing or does not match Cisco's terms";
        return result;
    }
    result.installed = true;
    result.failure = OpenH264RuntimeFailure::None;
    result.detail = "OpenH264 2.6.0 verified";
    return result;
#endif
}

OpenH264InstallResult OpenH264Runtime::install(
    [[maybe_unused]] const bool explicitConsent,
    [[maybe_unused]] const std::function<void(std::uint64_t)>& progress) const {
#if !defined(__linux__) || !defined(__x86_64__)
    return failure(OpenH264RuntimeFailure::UnsupportedPlatform,
                   "Cisco OpenH264 2.6.0 linux64.8 is not available for this platform", root_);
#else
    if (!explicitConsent)
        return failure(OpenH264RuntimeFailure::ConsentRequired,
                       "OpenH264 installation requires explicit Cisco licence consent", root_);
    if (root_.empty())
        return failure(OpenH264RuntimeFailure::InstallFailed,
                       "Per-user data directory is unavailable", root_);
    std::error_code error;
    std::filesystem::create_directories(root_, error);
    if (error)
        return failure(OpenH264RuntimeFailure::InstallFailed, "Could not create OpenH264 directory",
                       root_);
    const auto staging = root_ / ".libopenh264.so.bz2.staging";
    const auto extracted = root_ / ".libopenh264.so.staging";
    std::filesystem::remove(staging, error);
    std::filesystem::remove(extracted, error);
    platform::ProcessOptions curl;
    curl.executable = "/usr/bin/curl";
    curl.arguments = {"--fail",         "--location",     "--max-time",     "55",
                      "--max-filesize", "16777216",       "--silent",       "--show-error",
                      "--output",       staging.string(), std::string(kUrl)};
    curl.addressSpaceBytes = 256ULL * 1024U * 1024U;
    curl.openFiles = 32;
    bool launched = false;
    if (launcher_)
        launched = launcher_(curl.executable, curl.arguments, std::chrono::seconds(60));
    else {
        auto child = platform::ProcessSupervisor::launch(curl);
        if (const auto* processError = std::get_if<platform::ProcessFailure>(&child)) {
            (void)processError;
        } else {
            auto process = std::get<std::unique_ptr<platform::ProcessSupervisor>>(std::move(child));
            const auto finished =
                process->finish(std::chrono::steady_clock::now() + std::chrono::seconds(60));
            launched = std::get_if<platform::ProcessFailure>(&finished) == nullptr;
        }
    }
    if (!launched)
        return failure(OpenH264RuntimeFailure::DownloadFailed,
                       "Cisco OpenH264 download could not be started", root_);
    if (progress)
        progress(0);
    if (digestFile(staging) != kArchiveDigest)
        return failure(OpenH264RuntimeFailure::ArchiveDigestMismatch,
                       "Cisco OpenH264 archive digest mismatch or truncated download", root_);
    platform::ProcessOptions bunzip;
    bunzip.executable = "/usr/bin/bunzip2";
    bunzip.arguments = {"--force", "--keep", staging.string()};
    bunzip.addressSpaceBytes = 256ULL * 1024U * 1024U;
    bunzip.openFiles = 32;
    if (launcher_)
        launched = launcher_(bunzip.executable, bunzip.arguments, std::chrono::seconds(10));
    else {
        auto child = platform::ProcessSupervisor::launch(bunzip);
        if (const auto* processError = std::get_if<platform::ProcessFailure>(&child)) {
            (void)processError;
            launched = false;
        } else {
            auto process = std::get<std::unique_ptr<platform::ProcessSupervisor>>(std::move(child));
            const auto decompressedResult =
                process->finish(std::chrono::steady_clock::now() + std::chrono::seconds(10));
            launched = std::get_if<platform::ProcessFailure>(&decompressedResult) == nullptr;
        }
    }
    if (!launched)
        return failure(OpenH264RuntimeFailure::DownloadFailed,
                       "OpenH264 archive decompression failed", root_);
    const auto decompressed = staging.parent_path() / staging.filename().replace_extension();
    if (!std::filesystem::is_regular_file(decompressed) ||
        digestFile(decompressed) != kLibraryDigest)
        return failure(OpenH264RuntimeFailure::LibraryDigestMismatch,
                       "Cisco OpenH264 decompressed library digest mismatch", root_);
    if (!publish(decompressed, root_ / std::string(kLibrary)))
        return failure(OpenH264RuntimeFailure::InstallFailed,
                       "Could not atomically publish OpenH264 binary", root_);
    std::ofstream license(root_ / "BINARY_LICENSE.txt", std::ios::binary | std::ios::trunc);
    const auto licenseText = binaryLicenseText();
    license.write(licenseText.data(), static_cast<std::streamsize>(licenseText.size()));
    license.close();
    if (!license)
        return failure(OpenH264RuntimeFailure::InstallFailed,
                       "Could not publish OpenH264 binary licence", root_);
    std::ofstream consent(root_.parent_path().parent_path() / "Bloom.conf",
                          std::ios::binary | std::ios::app);
    consent << "[media]\nopenh264-consent=" << kVersion << "\n";
    if (!consent)
        return failure(OpenH264RuntimeFailure::InstallFailed, "Could not persist OpenH264 consent",
                       root_);
    std::filesystem::remove(staging, error);
    if (progress)
        progress(100);
    return {.installed = true,
            .failure = OpenH264RuntimeFailure::None,
            .directory = root_,
            .detail = "OpenH264 2.6.0 installed and verified"};
#endif
}

OpenH264InstallResult OpenH264Runtime::locate(const std::filesystem::path& file) const {
    if (!std::filesystem::is_regular_file(file))
        return failure(OpenH264RuntimeFailure::NotInstalled, "Selected OpenH264 file is missing",
                       root_);
    if (digestFile(file) != kLibraryDigest)
        return failure(OpenH264RuntimeFailure::LibraryDigestMismatch,
                       "Selected OpenH264 file does not match Cisco's verified digest", root_);
    std::error_code error;
    std::filesystem::create_directories(root_, error);
    if (error || !publish(file, root_ / std::string(kLibrary)))
        return failure(OpenH264RuntimeFailure::InstallFailed,
                       "Could not publish selected OpenH264 file", root_);
    std::ofstream license(root_ / "BINARY_LICENSE.txt", std::ios::binary | std::ios::trunc);
    const auto licenseText = binaryLicenseText();
    license.write(licenseText.data(), static_cast<std::streamsize>(licenseText.size()));
    license.close();
    if (!license)
        return failure(OpenH264RuntimeFailure::InstallFailed, "Could not write licence", root_);
    std::ofstream consent(root_.parent_path().parent_path() / "Bloom.conf",
                          std::ios::binary | std::ios::app);
    consent << "[media]\nopenh264-consent=" << kVersion << "\n";
    if (!consent)
        return failure(OpenH264RuntimeFailure::InstallFailed, "Could not persist OpenH264 consent",
                       root_);
    return {.installed = true,
            .failure = OpenH264RuntimeFailure::None,
            .directory = root_,
            .detail = "OpenH264 file installed and verified"};
}
} // namespace bloom::media::provider
