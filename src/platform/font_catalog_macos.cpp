#include <bloom/platform/font_catalog.hpp>

#include <bloom/core/sha256.hpp>

#include <CoreFoundation/CoreFoundation.h>
#include <CoreText/CoreText.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if !defined(__APPLE__)
#error "font_catalog_macos.cpp requires macOS"
#endif

namespace bloom::platform::detail {
namespace {

constexpr std::uintmax_t kMaximumFontBytes = static_cast<std::uintmax_t>(64) * 1024U * 1024U;

[[nodiscard]] std::optional<core::Sha256Digest> digestFile(const std::filesystem::path& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size == 0 || size > kMaximumFontBytes)
        return std::nullopt;
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        return std::nullopt;
    core::Sha256Hasher hasher;
    std::array<std::byte, static_cast<std::size_t>(64) * 1024U> buffer{};
    std::uintmax_t remaining = size;
    while (remaining != 0) {
        const auto requested = static_cast<std::streamsize>(
            std::min<std::uintmax_t>(remaining, static_cast<std::uintmax_t>(buffer.size())));
        stream.read(reinterpret_cast<char*>(buffer.data()), requested);
        if (stream.gcount() != requested)
            return std::nullopt;
        if (!hasher.update(
                std::span<const std::byte>(buffer.data(), static_cast<std::size_t>(requested))))
            return std::nullopt;
        remaining -= static_cast<std::uintmax_t>(requested);
    }
    return hasher.finalize();
}

[[nodiscard]] std::string toUtf8(CFStringRef value) {
    if (value == nullptr)
        return {};
    const CFIndex length = CFStringGetLength(value);
    const CFIndex capacity = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
    if (capacity <= 0)
        return {};
    std::string result(static_cast<std::size_t>(capacity), '\0');
    if (CFStringGetCString(value, result.data(), capacity, kCFStringEncodingUTF8) == false)
        return {};
    result.resize(std::strlen(result.c_str()));
    return result;
}

[[nodiscard]] std::string copiedString(CTFontDescriptorRef descriptor, CFStringRef attribute) {
    auto* value = static_cast<CFStringRef>(
        CTFontDescriptorCopyAttribute(descriptor, attribute)); // +1, may be null
    const std::string result = toUtf8(value);
    if (value != nullptr)
        CFRelease(value);
    return result;
}

[[nodiscard]] std::string pathFromDescriptor(CTFontDescriptorRef descriptor) {
    auto* url =
        static_cast<CFURLRef>(CTFontDescriptorCopyAttribute(descriptor, kCTFontURLAttribute));
    if (url == nullptr)
        return {};
    CFStringRef path = CFURLCopyFileSystemPath(url, kCFURLPOSIXPathStyle);
    const std::string result = toUtf8(path);
    if (path != nullptr)
        CFRelease(path);
    CFRelease(url);
    return result;
}

[[nodiscard]] double traitValue(CFDictionaryRef traits, CFStringRef key) {
    if (traits == nullptr)
        return 0.0;
    const void* raw = CFDictionaryGetValue(traits, key);
    if (raw == nullptr || CFGetTypeID(raw) != CFNumberGetTypeID())
        return 0.0;
    double value = 0.0;
    return CFNumberGetValue(static_cast<CFNumberRef>(raw), kCFNumberDoubleType, &value) == true
               ? value
               : 0.0;
}

} // namespace

FontCatalogue enumerateSystemFonts(const FontCatalogueProvider::Cancellation& cancelled) {
    FontCatalogue result;
    result.status = FontCatalogueStatus::Available;

    CTFontCollectionRef collection = CTFontCollectionCreateFromAvailableFonts(nullptr);
    if (collection == nullptr)
        return result;
    CFArrayRef descriptors = CTFontCollectionCreateMatchingFontDescriptors(collection);
    CFRelease(collection);
    if (descriptors == nullptr)
        return result;

    // CoreText exposes family/style/traits per descriptor and the containing file through
    // kCTFontURLAttribute; one file may contribute several descriptors. The digest is the file
    // identity, so dedupe on (path, family, style) rather than emitting a row per collection entry,
    // and hash each file only once -- a collection face (TTC) can hold dozens of families.
    std::unordered_set<std::string> seen;
    std::unordered_map<std::string, std::optional<core::Sha256Digest>> digestCache;
    const CFIndex count = CFArrayGetCount(descriptors);
    for (CFIndex index = 0; index < count; ++index) {
        if (cancelled && cancelled())
            break;
        auto* descriptor = static_cast<CTFontDescriptorRef>(
            const_cast<void*>(CFArrayGetValueAtIndex(descriptors, index)));
        if (descriptor == nullptr)
            continue;
        const std::string family = copiedString(descriptor, kCTFontFamilyNameAttribute);
        const std::string style = copiedString(descriptor, kCTFontStyleNameAttribute);
        const std::string file = pathFromDescriptor(descriptor);
        if (family.empty() || style.empty() || file.empty())
            continue;
        const std::string identity = file + '\n' + family + '\n' + style;
        if (!seen.insert(identity).second)
            continue;
        auto digestEntry = digestCache.find(file);
        if (digestEntry == digestCache.end()) {
            digestEntry = digestCache.emplace(file, digestFile(std::filesystem::path(file))).first;
        }
        const auto& digest = digestEntry->second;
        if (!digest.has_value())
            continue;

        auto* rawTraits = static_cast<CFDictionaryRef>(
            CTFontDescriptorCopyAttribute(descriptor, kCTFontTraitsAttribute));
        const double weightTrait = traitValue(rawTraits, kCTFontWeightTrait);
        const double widthTrait = traitValue(rawTraits, kCTFontWidthTrait);
        const double slantTrait = traitValue(rawTraits, kCTFontSlantTrait);
        if (rawTraits != nullptr)
            CFRelease(rawTraits);

        // CoreText normalises weight/width to roughly [-1, 1] around the regular face and slant to
        // [0, 1]. Map them onto the same 100-900 / 50-200 hundredths the Linux catalogue reports.
        const auto weight = std::clamp(
            static_cast<std::int32_t>(std::lround(400.0 + weightTrait * 400.0)), 100, 900);
        const auto width =
            std::clamp(static_cast<std::int32_t>(std::lround(100.0 + widthTrait * 50.0)), 50, 200);
        result.faces.push_back({family, style, weight, width, slantTrait > 0.0,
                                std::filesystem::path(file), *digest, FontSource::System, 0});
    }
    CFRelease(descriptors);
    return result;
}

} // namespace bloom::platform::detail
