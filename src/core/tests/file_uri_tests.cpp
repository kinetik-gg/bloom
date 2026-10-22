#include <bloom/core/file_uri.hpp>

#include <filesystem>
#include <stdexcept>
#include <string_view>

namespace {
void require(bool condition) {
    if (!condition)
        throw std::runtime_error("file URI regression");
}
void rejected(std::string_view uri) {
    require(!bloom::core::absolutePathFromFileUri(uri));
    require(!bloom::core::isAbsoluteFileUri(uri));
}
} // namespace

int main() {
    using bloom::core::absolutePathFromFileUri;
    using bloom::core::fileUriFromAbsolutePath;
    const auto path =
        std::filesystem::path(u8"/tmp/GVFS/smb-share:server=host/caf\u00e9 image.png");
    const auto uri = fileUriFromAbsolutePath(path);
#if !defined(_WIN32)
    require(uri == "file:///tmp/GVFS/smb-share%3Aserver%3Dhost/caf%C3%A9%20image.png");
    require(absolutePathFromFileUri(*uri) == path);
    require(!absolutePathFromFileUri("file://server/share/image.png"));
    require(absolutePathFromFileUri("file:///C:/Media/image.png") ==
            std::filesystem::path("/C:/Media/image.png"));
#else
    require(!uri);
    const auto drive = std::filesystem::path(L"C:\\Media\\caf\u00e9 image.png");
    require(fileUriFromAbsolutePath(drive) == "file:///C:/Media/caf%C3%A9%20image.png");
    require(absolutePathFromFileUri("file:///C:/Media/caf%C3%A9%20image.png") == drive);
    const auto unc = std::filesystem::path(L"\\\\server\\share\\image.png");
    require(fileUriFromAbsolutePath(unc) == "file://server/share/image.png");
    require(absolutePathFromFileUri("file://server/share/image.png") == unc);
#endif
    // A foreign platform's valid locator must still open and round-trip in a project.
    require(bloom::core::isAbsoluteFileUri("file://server/share/image.png"));
    require(bloom::core::isAbsoluteFileUri("file:///C:/Media/image.png"));
    require(bloom::core::isAbsoluteFileUri("file:///tmp/image.png"));
    rejected("http:///tmp/image.png");
    rejected("file:relative/image.png");
    rejected("file://server");
    rejected("file:///tmp/bad%XX.png");
    rejected("file:///tmp/bad%.png");
    rejected("file:///tmp/bad%00.png");
    rejected("file:///tmp/bad%2Fname.png");
    require(bloom::core::isAbsoluteFileUri("file:///tmp/back%5Cslash.png"));
#if defined(_WIN32)
    require(!absolutePathFromFileUri("file:///C:/tmp/back%5Cslash.png"));
#else
    require(absolutePathFromFileUri("file:///tmp/back%5Cslash.png") ==
            std::filesystem::path("/tmp/back\\slash.png"));
    const auto colonPath = std::filesystem::path("/C:/Media/image.png");
    require(absolutePathFromFileUri(*fileUriFromAbsolutePath(colonPath)) == colonPath);
#endif
    rejected("file:///tmp/image.png?query");
    rejected("file:///tmp/image.png#fragment");
    rejected("file://user@server/share/image.png");
    rejected("file:///tmp/raw space.png");
    rejected("file:///tmp/raw\u00e9.png");
    require(absolutePathFromFileUri("FILE:///tmp/name.png") ==
                std::filesystem::path("/tmp/name.png") ||
            !std::filesystem::path("/tmp/name.png").is_absolute());
    return 0;
}
