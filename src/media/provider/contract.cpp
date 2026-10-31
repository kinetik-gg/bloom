#include "wire.hpp"
#include <algorithm>
#include <cmath>
#include <future>
#include <numeric>
#include <set>

namespace bloom::media::provider {
bool wire::validText(std::string_view s) {
    std::uint32_t value = 0, minimum = 0;
    unsigned remaining = 0;
    for (const char character : s) {
        const auto c = static_cast<unsigned char>(character);
        if (c == 0)
            return false;
        if (remaining != 0) {
            if ((c & 0xc0U) != 0x80U)
                return false;
            value = (value << 6U) | (c & 0x3fU);
            if (--remaining == 0 &&
                (value < minimum || value > 0x10ffffU || (value >= 0xd800U && value <= 0xdfffU)))
                return false;
        }

        else if (c < 0x80U)
            continue;
        else if ((c & 0xe0U) == 0xc0U) {
            value = c & 0x1fU;
            minimum = 0x80;
            remaining = 1;
        } else if ((c & 0xf0U) == 0xe0U) {
            value = c & 0xfU;
            minimum = 0x800;
            remaining = 2;
        } else if ((c & 0xf8U) == 0xf0U) {
            value = c & 7U;
            minimum = 0x10000;
            remaining = 3;
        } else
            return false;
    }
    return remaining == 0;
}
bool valid(const Rational& v) {
    const auto magnitude = v.numerator >= 0 ? static_cast<std::uint64_t>(v.numerator)
                                            : static_cast<std::uint64_t>(-(v.numerator + 1)) + 1;
    return v.denominator > 0 && std::gcd(magnitude, static_cast<std::uint64_t>(v.denominator)) == 1;
}
std::uint64_t workerAddressSpaceBytes(const std::uint32_t width,
                                      const std::uint32_t height) noexcept {
    constexpr std::uint64_t granule = 256ULL << 20U;
    const auto pixels = static_cast<std::uint64_t>(std::min(width, Limits::dimension)) *
                        std::min(height, Limits::dimension);
    const auto wanted = (1ULL << 30U) + std::min(pixels, Limits::pixels) * 8U * 12U;
    const auto rounded = (wanted + granule - 1) / granule * granule;
    return std::clamp<std::uint64_t>(rounded, kWorkerAddressSpaceFloor, kWorkerAddressSpaceCeiling);
}
std::uint64_t av1EncodeAddressSpaceBytes(const std::uint32_t width,
                                         const std::uint32_t height) noexcept {
    constexpr std::uint64_t granule = 256ULL << 20U;
    const auto pixels = static_cast<std::uint64_t>(std::min(width, Limits::dimension)) *
                        std::min(height, Limits::dimension);
    const auto wanted = (2ULL << 30U) + std::min(pixels, Limits::pixels) * 256U;
    const auto rounded = (wanted + granule - 1) / granule * granule;
    return std::clamp<std::uint64_t>(rounded, kWorkerAddressSpaceFloor, kWorkerAddressSpaceCeiling);
}
namespace {
bool extent(std::uint32_t w, std::uint32_t h) {
    return w > 0 && h > 0 && w <= Limits::dimension && h <= Limits::dimension &&
           static_cast<std::uint64_t>(w) * h <= Limits::pixels;
}
bool text(const std::string& s) {
    return !s.empty() && s.size() <= Limits::stringBytes && wire::validText(s);
}
bool colour(const ColourTags& c) {
    return c.primaries >= -1 && c.primaries <= 255 && c.transfer >= -1 && c.transfer <= 255 &&
           c.matrix >= -1 && c.matrix <= 255 && c.range >= -1 && c.range <= 2;
}
bool layout(const std::vector<std::string>& channels) {
    if (channels.empty() || channels.size() > Limits::channels)
        return false;
    std::set<std::string> unique;
    for (const auto& c : channels)
        if (!text(c) || !unique.insert(c).second)
            return false;
    return true;
}
} // namespace
Digest digestBytes(std::span<const std::byte> bytes) {
    const auto result = core::Sha256Hasher::hash(bytes);
    if (!result)
        throw std::length_error("SHA-256 input limit");
    return *result;
}
bool valid(const ProbeResult& v) {
    if (!text(v.container) || !text(v.version) || v.streams.empty() ||
        v.streams.size() > Limits::entries)
        return false;
    std::set<std::uint32_t> ids;
    for (const auto& s : v.streams) {
        if (!ids.insert(s.id).second || !text(s.codec) || !text(s.profile) || !valid(s.timebase) ||
            s.timebase.numerator <= 0 || !valid(s.rate) || s.rate.numerator <= 0 ||
            !colour(s.colour) || !valid(s.duration) || s.duration.numerator < 0 ||
            !text(s.pixelFormat) || s.timecode.size() > Limits::stringBytes ||
            !wire::validText(s.timecode))
            return false;
        if (s.format < PixelFormat::Rgba8 || s.format > PixelFormat::Rgba16 ||
            s.kind < MediaKind::Video || s.kind > MediaKind::Data)
            return false;
        if (s.kind == MediaKind::Video && !extent(s.width, s.height))
            return false;
        if (s.kind == MediaKind::Audio &&
            (s.sampleRate == 0 || s.sampleRate > Limits::sampleRate || !layout(s.channelLayout)))
            return false;
        if (s.kind != MediaKind::Audio && (!s.channelLayout.empty() || s.sampleRate != 0))
            return false;
    }
    return true;
}
namespace {
// Planes of a large frame are hashed on separate threads; small products stay on the caller.
template <typename Work> void eachPlane(std::size_t count, std::uint64_t bytes, Work work) {
    if (count < 2 || bytes < (std::uint64_t{4} << 20U)) {
        for (std::size_t i = 0; i < count; ++i)
            work(i);
        return;
    }
    std::vector<std::future<void>> others;
    others.reserve(count - 1);
    for (std::size_t i = 1; i < count; ++i)
        others.push_back(std::async(std::launch::async, [&work, i] { work(i); }));
    std::exception_ptr failure;
    try {
        work(0);
    } catch (...) {
        failure = std::current_exception();
    }
    for (auto& other : others) {
        try {
            other.get();
        } catch (...) {
            if (!failure)
                failure = std::current_exception();
        }
    }
    if (failure)
        std::rethrow_exception(failure);
}
std::uint64_t planeBytes(const std::vector<CpuPlane>& planes) {
    std::uint64_t total = 0;
    for (const auto& p : planes)
        total += p.bytes.size();
    return total;
}
} // namespace
void assignPlaneDigests(std::vector<CpuPlane>& planes) {
    eachPlane(planes.size(), planeBytes(planes),
              [&](std::size_t i) { planes[i].digest = digestBytes(planes[i].bytes); });
}
bool planeDigestsMatch(const std::vector<CpuPlane>& planes) {
    std::vector<char> matches(planes.size(), 0);
    eachPlane(planes.size(), planeBytes(planes), [&](std::size_t i) {
        matches[i] = digestBytes(planes[i].bytes) == planes[i].digest ? 1 : 0;
    });
    return std::ranges::all_of(matches, [](char m) { return m != 0; });
}
bool valid(const FrameProduct& v) { return validLayout(v) && planeDigestsMatch(v.planes); }
bool validLayout(const FrameProduct& v) {
    if (!valid(v.pts) || !colour(v.colour) || v.format < PixelFormat::Rgba8 ||
        v.format > PixelFormat::Rgba16 || v.planes.empty() || v.planes.size() > Limits::planes)
        return false;
    const bool yuv = v.format == PixelFormat::Yuv420p8;
    const bool yuva = v.format == PixelFormat::Yuva444p16;
    if (v.planes.size() != (yuva ? 4U : (yuv ? 3U : 1U)))
        return false;
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < v.planes.size(); ++i) {
        const auto& p = v.planes[i];
        const auto bytesPerPixel =
            yuva ? 2U
                 : (yuv ? 1U
                        : (v.format == PixelFormat::Rgba8
                               ? 4U
                               : (v.format == PixelFormat::Rgba16 ? 8U : 16U)));
        if (!extent(p.width, p.height) || p.stride < p.width * bytesPerPixel ||
            p.bytes.size() != static_cast<std::uint64_t>(p.stride) * p.height)
            return false;
        if (i != 0 && (p.width != (yuva ? v.planes[0].width : (v.planes[0].width + 1) / 2) ||
                       p.height != (yuva ? v.planes[0].height : (v.planes[0].height + 1) / 2)))
            return false;
        total += p.bytes.size();
        if (total > Limits::productBytes)
            return false;
    }
    return true;
}
bool valid(const AudioBlock& v) {
    if (!valid(v.pts) || v.sampleRate == 0 || v.sampleRate > Limits::sampleRate ||
        !layout(v.channelLayout) || v.channels.size() != v.channelLayout.size())
        return false;
    const auto n = v.channels.front().size();
    if (n == 0 || n > Limits::audioSamples)
        return false;
    for (const auto& c : v.channels)
        if (c.size() != n || !std::ranges::all_of(c, [](float f) { return std::isfinite(f); }))
            return false;
    return true;
}
bool valid(const DemuxIndex& value) {
    return value.keyframes.size() <= Limits::indexEntries &&
           std::ranges::all_of(value.keyframes, [](const auto& entry) {
               return entry.stream < Limits::entries && valid(entry.pts) && valid(entry.dts);
           });
}
#define BLOOM_MEDIA_RECORD(Type)                                                                   \
    Result<Bytes> canonicalBytes(const Type& v) {                                                  \
        try {                                                                                      \
            wire::Writer w;                                                                        \
            w.text(#Type);                                                                         \
            w.number<std::uint16_t>(1);                                                            \
            wire::write(w, v);                                                                     \
            return std::move(w.bytes);                                                             \
        } catch (wire::Failure f) {                                                                \
            return Unavailable{f.code, "Invalid " #Type};                                          \
        }                                                                                          \
    }                                                                                              \
    Result<Digest> digest(const Type& v) {                                                         \
        auto bytes = canonicalBytes(v);                                                            \
        if (auto* b = std::get_if<Bytes>(&bytes))                                                  \
            return digestBytes(*b);                                                                \
        return std::get<Unavailable>(bytes);                                                       \
    }
BLOOM_MEDIA_RECORD(MediaCapabilityKeyV1)
BLOOM_MEDIA_RECORD(ProviderExecutionKeyV1)
BLOOM_MEDIA_RECORD(QualificationEvidenceV1)
BLOOM_MEDIA_RECORD(PipelineQualificationV1)
BLOOM_MEDIA_RECORD(MediaQcEvidenceV1)
#undef BLOOM_MEDIA_RECORD
} // namespace bloom::media::provider
