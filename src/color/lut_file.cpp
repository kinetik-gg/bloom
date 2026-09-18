#include "lut_preflight.hpp"
#include "lut_resource_budget.hpp"
#include <algorithm>
#include <bloom/color/ocio_cpu_file_transform_processor.hpp>
#include <charconv>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#ifdef __linux__
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace bloom::color {
namespace {
std::uint32_t formatOf(const std::filesystem::path& path) {
    auto extension = path.extension().string();
    std::ranges::transform(extension, extension.begin(), [](const unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (extension == ".cube")
        return 1;
    if (extension == ".clf")
        return 2;
    if (extension == ".spi1d")
        return 3;
    if (extension == ".spi3d")
        return 4;
    return 0;
}
} // namespace

LutError detail::preflightLut(const std::string_view text, const std::uint32_t format,
                              const std::function<bool()>& cancellation, bool* identity) {
    if (identity)
        *identity = false;
    if (text.find('\0') != std::string::npos)
        return LutError::MalformedFile;
    std::istringstream stream(std::string(text.substr(0, 4096)));
    stream.imbue(std::locale::classic());
    if (format == 1) {
        std::uint64_t expected = 0, samples = 0, oneDimensionalEdge = 0, threeDimensionalEdge = 0;
        bool canonicalIdentity = true;
        std::size_t offset = 0;
        while (offset < text.size()) {
            if (cancellation && cancellation())
                return LutError::HelperCancelled;
            const auto end = text.find('\n', offset);
            auto line = text.substr(offset, end == std::string_view::npos ? text.size() - offset
                                                                          : end - offset);
            offset += line.size() + 1;
            if (line.size() > 16384)
                return LutError::MalformedFile;
            line = line.substr(0, line.find('#'));
            std::istringstream row{std::string(line)};
            row.imbue(std::locale::classic());
            std::string token;
            if (!(row >> token))
                continue;
            if (token == "TITLE")
                continue;
            if (token == "DOMAIN_MIN" || token == "DOMAIN_MAX") {
                float r = 0, g = 0, b = 0;
                if (!(row >> r >> g >> b))
                    return LutError::MalformedFile;
                const float expectedDomain = token == "DOMAIN_MIN" ? 0.0F : 1.0F;
                canonicalIdentity = canonicalIdentity && r == expectedDomain &&
                                    g == expectedDomain && b == expectedDomain;
                continue;
            }
            if (token == "LUT_1D_INPUT_RANGE" || token == "LUT_3D_INPUT_RANGE") {
                float low = 0, high = 0;
                if (!(row >> low >> high))
                    return LutError::MalformedFile;
                canonicalIdentity = canonicalIdentity && low == 0.0F && high == 1.0F;
                continue;
            }
            if (token == "LUT_3D_SIZE" || token == "LUT_1D_SIZE") {
                auto& edge = token == "LUT_3D_SIZE" ? threeDimensionalEdge : oneDimensionalEdge;
                if (edge != 0 || samples != 0 || !(row >> edge) || edge < 2)
                    return LutError::MalformedFile;
                if (token == "LUT_3D_SIZE" && edge > kMaximumLut3dEdge)
                    return LutError::EdgeTooLarge;
                if (edge > kMaximumLutBytes / 12)
                    return LutError::FileTooLarge;
                expected = oneDimensionalEdge +
                           threeDimensionalEdge * threeDimensionalEdge * threeDimensionalEdge;
                continue;
            }
            float r = 0, g = 0, b = 0;
            const auto parsed = std::from_chars(token.data(), token.data() + token.size(), r);
            if (!expected || parsed.ec != std::errc{} ||
                parsed.ptr != token.data() + token.size() || !(row >> g >> b) ||
                !std::isfinite(r) || !std::isfinite(g) || !std::isfinite(b))
                return LutError::MalformedFile;
            std::string extra;
            if (row >> extra)
                return LutError::MalformedFile;
            const bool threeDimensional = samples >= oneDimensionalEdge;
            const auto edge = threeDimensional ? threeDimensionalEdge : oneDimensionalEdge;
            if (edge < 2)
                return LutError::MalformedFile;
            const auto gridIndex = threeDimensional ? samples - oneDimensionalEdge : samples;
            const auto coordinate = [edge](const std::uint64_t index) {
                return static_cast<float>(index) / static_cast<float>(edge - 1);
            };
            canonicalIdentity =
                canonicalIdentity &&
                r == coordinate(threeDimensional ? gridIndex % edge : gridIndex) &&
                g == coordinate(threeDimensional ? (gridIndex / edge) % edge : gridIndex) &&
                b == coordinate(threeDimensional ? gridIndex / (edge * edge) : gridIndex);
            ++samples;
            if (samples > expected)
                return LutError::MalformedFile;
        }
        if (!expected || samples != expected)
            return LutError::MalformedFile;
        if (identity)
            *identity = canonicalIdentity;
        return LutError::None;
    }
    if (format == 4) {
        std::string signature;
        double version = 0;
        std::uint64_t inputs = 0, outputs = 0, x = 0, y = 0, z = 0;
        if (!(stream >> signature >> version >> inputs >> outputs >> x >> y >> z) ||
            signature != "SPILUT" || inputs != 3 || outputs != 3 || x < 2 || y < 2 || z < 2)
            return LutError::MalformedFile;
        if (x > kMaximumLut3dEdge || y > kMaximumLut3dEdge || z > kMaximumLut3dEdge)
            return LutError::EdgeTooLarge;
    }
    if (format == 2) {
        // External references and XML entities are outside the sealed single-resource contract.
        if (text.find("Reference") != std::string::npos ||
            (text.find("<!DOCTYPE") != std::string::npos ||
             text.find("<!ENTITY") != std::string::npos))
            return LutError::UnsupportedFormat;
        std::size_t cursor = 0;
        while ((cursor = text.find("<LUT3D", cursor)) != std::string::npos) {
            const auto end = text.find("</LUT3D>", cursor);
            const auto array = text.find("<Array", cursor);
            const auto dim = array == std::string::npos ? array : text.find("dim", array);
            if (end == std::string::npos || dim == std::string::npos || dim > end)
                return LutError::MalformedFile;
            const auto quote = text.find_first_of("\"'", dim);
            if (quote == std::string::npos || quote > end)
                return LutError::MalformedFile;
            std::istringstream dimensions(
                std::string(text.substr(quote + 1, std::min<std::size_t>(128, end - quote - 1))));
            std::uint64_t x = 0, y = 0, z = 0;
            if (!(dimensions >> x >> y >> z) || x < 2 || y < 2 || z < 2)
                return LutError::MalformedFile;
            if (x > kMaximumLut3dEdge || y > kMaximumLut3dEdge || z > kMaximumLut3dEdge)
                return LutError::EdgeTooLarge;
            cursor = end + 8;
        }
    }
    return LutError::None;
}

bool isLutExtension(const std::filesystem::path& path) { return formatOf(path) != 0; }

LutFile readLutFile(const std::filesystem::path& path, const std::function<bool()>& cancellation) {
    LutFile result;
    result.format = formatOf(path);
    if (!result.format) {
        result.error = LutError::UnsupportedFormat;
        return result;
    }
    if (path.native().size() > 16384) {
        result.error = LutError::MalformedFile;
        return result;
    }
    try {
#ifdef __linux__
        const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        if (descriptor < 0) {
            result.error = LutError::MissingFile;
            return result;
        }
        struct Close final {
            int fd;
            ~Close() { ::close(fd); }
        } close{descriptor};
        struct stat before{};
        if (::fstat(descriptor, &before) != 0 || !S_ISREG(before.st_mode) || before.st_size <= 0) {
            result.error = LutError::MalformedFile;
            return result;
        }
        if (static_cast<std::uint64_t>(before.st_size) > kMaximumLutBytes) {
            result.error = LutError::FileTooLarge;
            return result;
        }
        result.reservation = detail::LutReservation::acquire(kMaximumLutBytes);
        if (!result.reservation) {
            result.error = LutError::HelperMemoryLimit;
            return result;
        }
        result.bytes.resize(static_cast<std::size_t>(before.st_size));
        std::size_t offset = 0;
        while (offset < result.bytes.size()) {
            if (cancellation && cancellation()) {
                result.error = LutError::HelperCancelled;
                return result;
            }
            const auto count = ::read(descriptor, result.bytes.data() + offset,
                                      std::min<std::size_t>(65536, result.bytes.size() - offset));
            if (count <= 0) {
                result.error = LutError::ChangedFile;
                return result;
            }
            offset += static_cast<std::size_t>(count);
        }
        struct stat after{};
        if (::fstat(descriptor, &after) != 0 || before.st_size != after.st_size ||
            before.st_mtim.tv_sec != after.st_mtim.tv_sec ||
            before.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
            before.st_ctim.tv_sec != after.st_ctim.tv_sec ||
            before.st_ctim.tv_nsec != after.st_ctim.tv_nsec) {
            result.error = LutError::ChangedFile;
            return result;
        }
#else
        // External OCIO supervision is unavailable on this target. Do not parse an external LUT
        // in-process as a substitute for the missing process isolation.
        (void)path;
        (void)cancellation;
        result.error = LutError::HelperUnavailable;
        return result;
#endif
        const auto digest = core::Sha256Hasher::hash(result.bytes);
        if (!digest) {
            result.error = LutError::FileTooLarge;
            return result;
        }
        result.digest = *digest;
        result.error = detail::preflightLut(
            std::string_view(reinterpret_cast<const char*>(result.bytes.data()),
                             result.bytes.size()),
            result.format, cancellation);
    } catch (const std::bad_alloc&) {
        result.error = LutError::HelperMemoryLimit;
    } catch (const std::exception&) {
        result.error = LutError::MalformedFile;
    }
    return result;
}

std::string_view lutErrorName(const LutError error) noexcept {
    switch (error) {
#define BLOOM_LUT_ERROR(name)                                                                      \
    case LutError::name:                                                                           \
        return #name
        BLOOM_LUT_ERROR(None);
        BLOOM_LUT_ERROR(MissingFile);
        BLOOM_LUT_ERROR(UnsupportedFormat);
        BLOOM_LUT_ERROR(FileTooLarge);
        BLOOM_LUT_ERROR(EdgeTooLarge);
        BLOOM_LUT_ERROR(MalformedFile);
        BLOOM_LUT_ERROR(ChangedFile);
        BLOOM_LUT_ERROR(HelperUnavailable);
        BLOOM_LUT_ERROR(HelperProtocolViolation);
        BLOOM_LUT_ERROR(HelperMemoryLimit);
        BLOOM_LUT_ERROR(HelperDeadline);
        BLOOM_LUT_ERROR(HelperCancelled);
        BLOOM_LUT_ERROR(HelperTerminated);
        BLOOM_LUT_ERROR(TransformBuildFailed);
        BLOOM_LUT_ERROR(InvalidPixel);
#undef BLOOM_LUT_ERROR
    }
    return "HelperProtocolViolation";
}
} // namespace bloom::color
