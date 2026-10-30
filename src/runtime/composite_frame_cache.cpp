#include <bloom/runtime/composite_frame_cache.hpp>

#include <bloom/media/image.hpp>
#include <bloom/platform/source_identity.hpp>
#include <bloom/runtime/gpu_processing_phase_policy.hpp>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstring>
#include <functional>
#include <new>
#include <system_error>
#include <unordered_set>

namespace bloom::runtime {
namespace {

struct Source final {
    std::string path;
    core::Sha256Digest recorded{};
};

void collectAsset(const document::AssetRecord& asset, const std::filesystem::path& base,
                  std::vector<Source>& sources) {
    if (asset.manifest.members.empty()) {
        sources.push_back(
            {media::resolveImagePath(asset.locator.path, asset.locator.relinkHint, base).string(),
             asset.contentDigest});
        return;
    }
    for (const auto& member : asset.manifest.members)
        sources.push_back(
            {media::resolveImagePath(member.locator.path, member.locator.relinkHint, base).string(),
             member.contentDigest});
}

void collectPlan(const CompiledCompositionPlan& plan, const std::filesystem::path& base,
                 std::vector<Source>& sources,
                 std::unordered_set<const CompiledCompositionPlan*>& visited) {
    if (!visited.insert(&plan).second)
        return;
    for (const auto& operation : plan.operations()) {
        if (const auto* image = std::get_if<CompiledImageSource>(&operation)) {
            if (image->asset)
                collectAsset(*image->asset, base, sources);
        } else if (const auto* video = std::get_if<CompiledVideoSource>(&operation)) {
            if (video->asset)
                collectAsset(*video->asset, base, sources);
        } else if (const auto* effect = std::get_if<CompiledImageEffect>(&operation)) {
            if (const auto* file = std::get_if<FileTransformKernel>(&effect->kernel);
                file != nullptr && file->asset)
                collectAsset(*file->asset, base, sources);
        }
    }
    for (const auto& nested : plan.nestedPlans())
        if (nested)
            collectPlan(*nested, base, sources, visited);
}

// The identity-verified digest of a source (ADR 0025): the registry's cheap observation when it
// still vouches for a full hash, otherwise one full hash pass that the registry then records.
std::optional<core::Sha256Digest> verifiedDigest(const std::string& path) {
    const auto observed = platform::observeSource(path);
    if (observed.digest)
        return observed.digest;
    const auto hashed = platform::hashSourceFile(path);
    if (hashed.error != platform::SourceHashError::None)
        return std::nullopt;
    return hashed.digest;
}

} // namespace

bool operator==(const CompositeFrameKey& lhs, const CompositeFrameKey& rhs) {
    const bool plansEqual = lhs.plan == rhs.plan ||
                            (lhs.plan != nullptr && rhs.plan != nullptr && *lhs.plan == *rhs.plan);
    return plansEqual && lhs.time == rhs.time && lhs.output == rhs.output &&
           lhs.colorIntent == rhs.colorIntent && lhs.media == rhs.media;
}

std::optional<CompositeFrameKey>
compositeFrameKeyFor(const std::shared_ptr<const CompiledCompositionPlan>& plan,
                     const EvaluationRequest& request,
                     const std::filesystem::path& assetBaseDirectory) {
    if (!plan || !std::holds_alternative<CompositionFormatResolution>(request.resolution) ||
        request.quality != EvaluationQuality::Reference || request.roi || request.bypassLookNodes)
        return std::nullopt;
    auto colorIntent = captureEvaluationColorIntent(request.colorIntent);
    if (!colorIntent)
        return std::nullopt;
    std::vector<Source> sources;
    std::unordered_set<const CompiledCompositionPlan*> visited;
    try {
        collectPlan(*plan, assetBaseDirectory, sources, visited);
    } catch (...) {
        return std::nullopt;
    }
    std::sort(sources.begin(), sources.end(),
              [](const Source& lhs, const Source& rhs) { return lhs.path < rhs.path; });
    CompositeFrameKey key{.plan = plan,
                          .time = request.time,
                          .output = request.output,
                          .colorIntent = std::move(*colorIntent),
                          .media = {}};
    key.media.reserve(sources.size());
    for (const auto& source : sources) {
        if (!key.media.empty() && key.media.back().path == source.path) {
            if (key.media.back().digest != source.recorded)
                return std::nullopt;
            continue;
        }
        // The file must be exactly the content the document recorded, as for a fresh render.
        const auto digest = verifiedDigest(source.path);
        if (!digest || *digest != source.recorded)
            return std::nullopt;
        key.media.push_back({.path = source.path, .digest = *digest});
    }
    return key;
}

bool compositeFrameMediaCurrent(const CompositeFrameKey& key) {
    return std::all_of(key.media.begin(), key.media.end(), [](const MediaSourceStamp& source) {
        const auto observed = platform::observeSource(source.path);
        return observed.digest && *observed.digest == source.digest;
    });
}

std::uint16_t floatToHalf(const float value) noexcept {
    const auto bits = std::bit_cast<std::uint32_t>(value);
    const std::uint32_t sign = (bits >> 16) & 0x8000U;
    const std::uint32_t exponent = (bits >> 23) & 0xffU;
    std::uint32_t mantissa = bits & 0x7fffffU;
    if (exponent == 0xffU) // Inf or NaN (NaN keeps a quiet payload bit).
        return static_cast<std::uint16_t>(sign | 0x7c00U | (mantissa != 0 ? 0x200U : 0U));
    const int unbiased = static_cast<int>(exponent) - 127;
    if (unbiased > 15)
        return static_cast<std::uint16_t>(sign | 0x7c00U);
    if (unbiased >= -14) {
        // Normal half: round the 23-bit mantissa to 10 bits, ties to even.
        std::uint32_t half = (static_cast<std::uint32_t>(unbiased + 15) << 10) | (mantissa >> 13);
        const std::uint32_t remainder = mantissa & 0x1fffU;
        if (remainder > 0x1000U || (remainder == 0x1000U && (half & 1U) != 0))
            ++half; // A carry into the exponent is the correct rounding (up to Inf).
        return static_cast<std::uint16_t>(sign | half);
    }
    if (unbiased < -25)
        return static_cast<std::uint16_t>(sign);
    // Subnormal half.
    mantissa |= 0x800000U;
    const auto shift = static_cast<std::uint32_t>(-unbiased - 14 + 13);
    std::uint32_t half = mantissa >> shift;
    const std::uint32_t remainder = mantissa & ((1U << shift) - 1U);
    const std::uint32_t midpoint = 1U << (shift - 1U);
    if (remainder > midpoint || (remainder == midpoint && (half & 1U) != 0))
        ++half;
    return static_cast<std::uint16_t>(sign | half);
}

float halfToFloat(const std::uint16_t value) noexcept {
    const std::uint32_t sign = (static_cast<std::uint32_t>(value) & 0x8000U) << 16;
    const std::uint32_t exponent = (value >> 10) & 0x1fU;
    std::uint32_t mantissa = value & 0x3ffU;
    std::uint32_t bits = 0;
    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else {
            int shift = 0;
            while ((mantissa & 0x400U) == 0) {
                mantissa <<= 1;
                ++shift;
            }
            mantissa &= 0x3ffU;
            bits =
                sign | (static_cast<std::uint32_t>(127 - 15 + 1 - shift) << 23) | (mantissa << 13);
        }
    } else if (exponent == 0x1fU) {
        bits = sign | 0x7f800000U | (mantissa << 13);
    } else {
        bits = sign | ((exponent - 15 + 127) << 23) | (mantissa << 13);
    }
    return std::bit_cast<float>(bits);
}

std::shared_ptr<const CompositeFrame> CompositeFrame::fromImage(const render::Rgba32fImage& image) {
    const auto* descriptor = image.descriptor();
    if (descriptor == nullptr)
        return nullptr;
    try {
        const auto pixels = image.pixels();
        std::vector<std::uint16_t> halves(pixels.size() * 4);
        for (std::size_t index = 0; index < pixels.size(); ++index) {
            const auto components = pixels[index].components();
            for (std::size_t channel = 0; channel < 4; ++channel)
                halves[index * 4 + channel] = floatToHalf(components[channel]);
        }
        return std::shared_ptr<const CompositeFrame>(
            new CompositeFrame(*descriptor, std::move(halves)));
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
}

render::ImageResult<render::Rgba32fImage> CompositeFrame::toImage() const {
    auto builder = render::Rgba32fImageBuilder::create(descriptor_, halves_.size() * sizeof(float));
    if (!builder.hasValue())
        return render::ImageResult<render::Rgba32fImage>::failure(*builder.error());
    auto& image = *builder.value();
    const auto window = descriptor_.dataWindow();
    const auto width = static_cast<std::size_t>(window.extent().width());
    for (std::int64_t y = 0; y < window.extent().height(); ++y) {
        auto row = image.row(window.originY() + y);
        if (!row.hasValue())
            return render::ImageResult<render::Rgba32fImage>::failure(*row.error());
        auto span = *row.value();
        const auto offset = static_cast<std::size_t>(y) * width * 4;
        for (std::size_t x = 0; x < width; ++x) {
            const auto* half = &halves_[offset + x * 4];
            auto pixel =
                render::Rgba32f::fromPremultiplied(halfToFloat(half[0]), halfToFloat(half[1]),
                                                   halfToFloat(half[2]), halfToFloat(half[3]));
            if (!pixel.hasValue())
                return render::ImageResult<render::Rgba32fImage>::failure(*pixel.error());
            span[x] = *pixel.value();
        }
    }
    return std::move(image).freeze();
}

CompositeFrameCache::CompositeFrameCache(std::shared_ptr<GpuProcessingPhasePolicy> policy)
    : policy_(std::move(policy)) {}

std::size_t CompositeFrameCache::bucketOf(const CompositeFrameKey& key) noexcept {
    // Time and output select the bucket; the deep plan/media/colour comparison decides the hit.
    const auto time = key.time;
    std::size_t hash = std::hash<std::int64_t>{}(time.numerator());
    hash ^= std::hash<std::int64_t>{}(time.denominator()) + 0x9e3779b97f4a7c15ULL + (hash << 6) +
            (hash >> 2);
    hash ^= std::hash<std::size_t>{}(key.output.value()) + 0x9e3779b97f4a7c15ULL + (hash << 6) +
            (hash >> 2);
    if (key.plan)
        hash ^= std::hash<std::uint64_t>{}(key.plan->compositionId().value()) +
                0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
    return hash;
}

std::shared_ptr<const CompositeFrame> CompositeFrameCache::lookup(const CompositeFrameKey& key) {
    // Re-stat outside the lock: a changed file on disk must miss.
    const bool mediaCurrent = compositeFrameMediaCurrent(key);
    const std::lock_guard lock(mutex_);
    const auto [first, last] = index_.equal_range(bucketOf(key));
    for (auto it = first; it != last; ++it) {
        if (it->second->key == key) {
            if (!mediaCurrent) {
                stats_.bytes -= it->second->frame->bytes();
                entries_.erase(it->second);
                index_.erase(it);
                --stats_.entries;
                ++stats_.staleMediaMisses;
                ++stats_.misses;
                return nullptr;
            }
            entries_.splice(entries_.begin(), entries_, it->second);
            ++stats_.hits;
            return entries_.front().frame;
        }
    }
    ++stats_.misses;
    return nullptr;
}

bool CompositeFrameCache::insert(CompositeFrameKey key,
                                 std::shared_ptr<const CompositeFrame> frame) {
    if (!frame || !key.plan)
        return false;
    const auto bytes = frame->bytes();
    std::optional<UsableCapacity> capacity;
    std::uint64_t sampleSerial = 0;
    if (policy_) {
        capacity = policy_->usableCapacity();
        sampleSerial = policy_->snapshot().sampleSerial;
    }
    const std::lock_guard lock(mutex_);
    if (capacity && capacity->hostReady) {
        // Retention fills usable memory. The measured free capacity already excludes what this
        // cache held at that sample; growth since then is subtracted and shrinkage added back.
        if (sampleSerial != sampleSerial_) {
            sampleSerial_ = sampleSerial;
            bytesAtSample_ = stats_.bytes;
        }
        if (bytes > capacity->hostUsableBytes)
            return false;
        const auto room = capacity->hostFreeBytes + bytesAtSample_;
        if (bytes + stats_.bytes > room)
            evictLocked(bytes + stats_.bytes - room);
        if (bytes + stats_.bytes > room)
            return false;
    }
    const auto bucket = bucketOf(key);
    const auto [first, last] = index_.equal_range(bucket);
    for (auto it = first; it != last; ++it) {
        if (it->second->key == key) {
            stats_.bytes -= it->second->frame->bytes();
            entries_.erase(it->second);
            index_.erase(it);
            --stats_.entries;
            break;
        }
    }
    bool pushed = false;
    try {
        entries_.push_front({.key = std::move(key), .frame = std::move(frame)});
        pushed = true;
        index_.emplace(bucket, entries_.begin());
    } catch (const std::bad_alloc&) {
        if (pushed)
            entries_.pop_front();
        return false;
    }
    stats_.bytes += bytes;
    ++stats_.entries;
    return true;
}

std::uint64_t CompositeFrameCache::evictLocked(const std::uint64_t bytes) {
    std::uint64_t released = 0;
    while (released < bytes && !entries_.empty()) {
        auto victim = std::prev(entries_.end());
        const auto bucket = bucketOf(victim->key);
        const auto [first, last] = index_.equal_range(bucket);
        for (auto it = first; it != last; ++it) {
            if (it->second == victim) {
                index_.erase(it);
                break;
            }
        }
        released += victim->frame->bytes();
        stats_.bytes -= victim->frame->bytes();
        --stats_.entries;
        ++stats_.evictions;
        entries_.erase(victim);
    }
    return released;
}

std::uint64_t CompositeFrameCache::evict(const std::uint64_t bytes) {
    const std::lock_guard lock(mutex_);
    return evictLocked(bytes);
}

void CompositeFrameCache::clear() {
    const std::lock_guard lock(mutex_);
    stats_.evictions += entries_.size();
    entries_.clear();
    index_.clear();
    stats_.entries = 0;
    stats_.bytes = 0;
}

CompositeFrameCacheStats CompositeFrameCache::stats() const {
    const std::lock_guard lock(mutex_);
    return stats_;
}

} // namespace bloom::runtime
