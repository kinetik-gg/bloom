#include "layer_mask_coverage.hpp"

#include <bloom/core/sha256.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

namespace bloom::runtime::detail {
namespace {

using Point = std::array<double, 2>;

[[nodiscard]] Point scaled(const document::Vec2d point, const double h, const double v) noexcept {
    return {point.x * h, point.y * v};
}

[[nodiscard]] double distance(const Point a, const Point b) noexcept {
    return std::hypot(b[0] - a[0], b[1] - a[1]);
}

void appendCubic(std::vector<std::array<float, 4>>& out, const Point p0, const Point p1,
                 const Point p2, const Point p3, const bool curved) {
    // A deterministic subdivision from the scaled control polygon length, shared by every route.
    const double length = distance(p0, p1) + distance(p1, p2) + distance(p2, p3);
    const auto steps = curved ? std::clamp(static_cast<int>(std::ceil(length / 4.0)), 1, 64) : 1;
    Point previous = p0;
    for (int step = 1; step <= steps; ++step) {
        const double t = static_cast<double>(step) / static_cast<double>(steps);
        const double u = 1.0 - t;
        const double a = u * u * u;
        const double b = 3.0 * u * u * t;
        const double c = 3.0 * u * t * t;
        const double d = t * t * t;
        const Point next{a * p0[0] + b * p1[0] + c * p2[0] + d * p3[0],
                         a * p0[1] + b * p1[1] + c * p2[1] + d * p3[1]};
        out.push_back({static_cast<float>(previous[0]), static_cast<float>(previous[1]),
                       static_cast<float>(next[0]), static_cast<float>(next[1])});
        previous = next;
    }
}

// A square root from correctly rounded Float32 multiply and subtract only (a bit-pattern seed and
// three Newton steps of the reciprocal root), so the CPU reference and the GPU program, which
// spells the same operations in the same order, agree bit for bit. Both builds forbid contraction.
[[nodiscard]] float maskSqrt(const float value) noexcept {
    if (!(value > 0.0F))
        return 0.0F;
    const auto bits = 0x5f3759dfU - (std::bit_cast<std::uint32_t>(value) >> 1U);
    float root = std::bit_cast<float>(bits);
    const float half = 0.5F * value;
    for (int step = 0; step < 3; ++step) {
        float term = root * root;
        term = half * term;
        term = 1.5F - term;
        root = root * term;
    }
    return value * root;
}

[[nodiscard]] float maskCoverage(const PreparedMask& mask, const float x, const float y) noexcept {
    float coverage = 0.0F;
    const float reach = std::fabs(mask.expansion) + mask.width * 0.5F + 1.0F;
    if (!mask.segments.empty() && x >= mask.bounds[0] - reach && x <= mask.bounds[2] + reach &&
        y >= mask.bounds[1] - reach && y <= mask.bounds[3] + reach) {
        int winding = 0;
        float nearest = std::numeric_limits<float>::infinity();
        for (std::size_t index = 0; index < mask.segments.size(); ++index) {
            const auto& segment = mask.segments[index];
            const auto& factor = mask.factors[index];
            const float x0 = segment[0];
            const float y0 = segment[1];
            const float x1 = segment[2];
            const float y1 = segment[3];
            if ((y0 <= y) != (y1 <= y)) {
                const float crossing = x0 + (y - y0) * factor[0];
                if (crossing > x)
                    winding += y1 > y0 ? 1 : -1;
            }
            const float dx = x1 - x0;
            const float dy = y1 - y0;
            float t = ((x - x0) * dx + (y - y0) * dy) * factor[1];
            t = std::clamp(t, 0.0F, 1.0F);
            const float qx = x0 + t * dx - x;
            const float qy = y0 + t * dy - y;
            nearest = std::min(nearest, qx * qx + qy * qy);
        }
        const float unsignedDistance = maskSqrt(nearest);
        const float signedDistance = winding != 0 ? -unsignedDistance : unsignedDistance;
        coverage =
            std::clamp(0.5F - (signedDistance - mask.expansion) * mask.inverseWidth, 0.0F, 1.0F);
    }
    if (mask.inverted)
        coverage = 1.0F - coverage;
    return coverage * mask.opacity;
}

void feed(core::Sha256Hasher& hasher, const void* data, const std::size_t size) noexcept {
    (void)hasher.update(std::span(static_cast<const std::byte*>(data), size));
}

} // namespace

std::vector<std::array<float, 4>> flattenMaskPath(const document::PathValue& path,
                                                  const double horizontalScale,
                                                  const double verticalScale) {
    std::vector<std::array<float, 4>> segments;
    if (!path.closed || path.anchors.size() < 2)
        return segments;
    const auto count = path.anchors.size();
    for (std::size_t index = 0; index < count; ++index) {
        const auto& from = path.anchors[index];
        const auto& to = path.anchors[(index + 1) % count];
        const auto p0 = scaled(from.point, horizontalScale, verticalScale);
        const auto p3 = scaled(to.point, horizontalScale, verticalScale);
        const auto p1 = scaled(from.outHandle.value_or(from.point), horizontalScale, verticalScale);
        const auto p2 = scaled(to.inHandle.value_or(to.point), horizontalScale, verticalScale);
        appendCubic(segments, p0, p1, p2, p3,
                    from.outHandle.has_value() || to.inHandle.has_value());
    }
    return segments;
}

std::optional<PreparedLayerMasks> prepareLayerMasks(const std::span<const SampledLayerMask> masks,
                                                    const double horizontalScale,
                                                    const double verticalScale) {
    PreparedLayerMasks prepared;
    const double scale = std::sqrt(std::fabs(horizontalScale * verticalScale));
    core::Sha256Hasher hasher;
    bool first = true;
    for (const auto& mask : masks) {
        if (!std::isfinite(mask.feather) || mask.feather < 0.0 ||
            mask.feather > document::kMaximumLayerMaskFeatherPixels ||
            !std::isfinite(mask.expansion) ||
            std::fabs(mask.expansion) > document::kMaximumLayerMaskExpansionPixels ||
            !std::isfinite(mask.opacity) || mask.opacity < 0.0 || mask.opacity > 1.0 ||
            !mask.path.isValid())
            return std::nullopt;
        if (mask.mode == document::LayerMaskMode::None)
            continue;
        if (first) {
            prepared.start = mask.mode == document::LayerMaskMode::Add ||
                                     mask.mode == document::LayerMaskMode::Difference
                                 ? 0.0F
                                 : 1.0F;
            first = false;
        }
        PreparedMask item;
        item.mode = mask.mode;
        item.inverted = mask.inverted;
        item.width = static_cast<float>(std::max(mask.feather * scale, 1.0));
        item.expansion = static_cast<float>(mask.expansion * scale);
        item.opacity = static_cast<float>(mask.opacity);
        item.inverseWidth = 1.0F / item.width;
        item.segments = flattenMaskPath(mask.path, horizontalScale, verticalScale);
        item.factors.reserve(item.segments.size());
        for (const auto& segment : item.segments) {
            const float dx = segment[2] - segment[0];
            const float dy = segment[3] - segment[1];
            const float length2 = dx * dx + dy * dy;
            item.factors.push_back(
                {dy != 0.0F ? dx / dy : 0.0F, length2 > 0.0F ? 1.0F / length2 : 0.0F});
        }
        item.bounds = {
            std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity(),
            -std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()};
        for (const auto& segment : item.segments) {
            item.bounds[0] = std::min({item.bounds[0], segment[0], segment[2]});
            item.bounds[1] = std::min({item.bounds[1], segment[1], segment[3]});
            item.bounds[2] = std::max({item.bounds[2], segment[0], segment[2]});
            item.bounds[3] = std::max({item.bounds[3], segment[1], segment[3]});
        }
        prepared.segmentCount += item.segments.size();
        const auto mode = static_cast<std::int64_t>(item.mode);
        const auto inverted = static_cast<std::uint8_t>(item.inverted);
        const auto segmentCount = static_cast<std::uint64_t>(item.segments.size());
        feed(hasher, &mode, sizeof(mode));
        feed(hasher, &inverted, sizeof(inverted));
        feed(hasher, &item.width, sizeof(item.width));
        feed(hasher, &item.expansion, sizeof(item.expansion));
        feed(hasher, &item.opacity, sizeof(item.opacity));
        feed(hasher, &segmentCount, sizeof(segmentCount));
        feed(hasher, item.segments.data(), item.segments.size() * sizeof(item.segments.front()));
        feed(hasher, item.factors.data(), item.factors.size() * sizeof(item.factors.front()));
        feed(hasher, &item.inverseWidth, sizeof(item.inverseWidth));
        prepared.masks.push_back(std::move(item));
    }
    feed(hasher, &prepared.start, sizeof(prepared.start));
    const auto digest = hasher.finalize();
    prepared.identity = "layer-mask-v1:";
    for (const auto byte : digest.bytes())
        prepared.identity.push_back(static_cast<char>(byte));
    return prepared;
}

float layerMaskCoverage(const PreparedLayerMasks& masks, const float x, const float y) noexcept {
    float accumulated = masks.start;
    for (const auto& mask : masks.masks) {
        const float c = maskCoverage(mask, x, y);
        switch (mask.mode) {
        case document::LayerMaskMode::Add:
            accumulated = accumulated + c - accumulated * c;
            break;
        case document::LayerMaskMode::Subtract:
            accumulated = accumulated * (1.0F - c);
            break;
        case document::LayerMaskMode::Intersect:
            accumulated = accumulated * c;
            break;
        case document::LayerMaskMode::Difference:
            accumulated = accumulated + c - 2.0F * accumulated * c;
            break;
        case document::LayerMaskMode::None:
            break;
        }
    }
    return std::clamp(accumulated, 0.0F, 1.0F);
}

const std::string& layerMaskProgramText() {
    // The CPU reference (layerMaskCoverage) operation for operation, in Float32, over the geometry
    // in the storage buffer: values[0] is (mask count, accumulation start, origin x, origin y);
    // mask k is values[1 + 3k] (first segment, segment count, mode, inverted), values[2 + 3k]
    // (width, 1/width, expansion, opacity) and values[3 + 3k] (bounds); segment i of the stack is
    // values[1 + 3n + 2i] (x0, y0, x1, y1) and values[2 + 3n + 2i] (slope, 1/length^2). Every
    // integer is exact in Float32 (the generator refuses an origin beyond 2^24).
    static const std::string text = [] {
        const auto mode = [](const document::LayerMaskMode value) {
            return std::to_string(static_cast<std::int64_t>(value));
        };
        std::string glsl =
            "layout(set = 0, binding = 0, std430) readonly buffer BloomLayerMaskData { vec4 "
            "values[]; } bloom_mask_data;\n"
            "float bloom_mask_sqrt(float value) {\n"
            " if (!(value > 0.0)) return 0.0;\n"
            " precise float root = uintBitsToFloat(0x5f3759dfu - (floatBitsToUint(value) >> 1u));\n"
            " precise float half_ = 0.5 * value;\n"
            " for (int step = 0; step < 3; ++step) {\n"
            "  precise float term = root * root;\n"
            "  term = half_ * term;\n"
            "  term = 1.5 - term;\n"
            "  root = root * term;\n"
            " }\n"
            " precise float result = value * root;\n"
            " return result;\n"
            "}\n"
            "float bloom_mask_one(int segments, int count, vec4 box, vec4 shape, bool inverted, "
            "vec2 p) {\n"
            " float width = shape.x;\n"
            " float inverseWidth = shape.y;\n"
            " float expansion = shape.z;\n"
            " float opacity = shape.w;\n"
            " precise float coverage = 0.0;\n"
            " precise float reach = abs(expansion) + width * 0.5 + 1.0;\n"
            " if (count > 0 && p.x >= box.x - reach && p.x <= box.z + reach && p.y >= box.y - "
            "reach && p.y <= box.w + reach) {\n"
            "  int winding = 0;\n"
            "  precise float nearest = uintBitsToFloat(0x7f800000u);\n"
            "  for (int i = 0; i < count; ++i) {\n"
            "   vec4 s = bloom_mask_data.values[segments + 2 * i];\n"
            "   vec2 f = bloom_mask_data.values[segments + 2 * i + 1].xy;\n"
            "   if ((s.y <= p.y) != (s.w <= p.y)) {\n"
            "    precise float crossing = s.x + (p.y - s.y) * f.x;\n"
            "    if (crossing > p.x) winding += s.w > s.y ? 1 : -1;\n"
            "   }\n"
            "   precise float dx = s.z - s.x;\n"
            "   precise float dy = s.w - s.y;\n"
            "   precise float t = ((p.x - s.x) * dx + (p.y - s.y) * dy) * f.y;\n"
            "   t = clamp(t, 0.0, 1.0);\n"
            "   precise float qx = s.x + t * dx - p.x;\n"
            "   precise float qy = s.y + t * dy - p.y;\n"
            "   nearest = min(nearest, qx * qx + qy * qy);\n"
            "  }\n"
            "  precise float unsignedDistance = bloom_mask_sqrt(nearest);\n"
            "  precise float signedDistance = winding != 0 ? -unsignedDistance : "
            "unsignedDistance;\n"
            "  coverage = clamp(0.5 - (signedDistance - expansion) * inverseWidth, 0.0, 1.0);\n"
            " }\n"
            " if (inverted) coverage = 1.0 - coverage;\n"
            " precise float result = coverage * opacity;\n"
            " return result;\n"
            "}\n"
            "vec4 bloom_layer_mask(ivec2 c) {\n"
            " vec4 pixel = imageLoad(bloom_ocio_input, c);\n"
            " vec4 header = bloom_mask_data.values[0];\n"
            " int maskCount = int(header.x);\n"
            " vec2 p = vec2(float(c.x + int(header.z)) + 0.5, float(c.y + int(header.w)) + 0.5);\n"
            " precise float a = header.y;\n"
            " int segments = 1 + 3 * maskCount;\n"
            " for (int k = 0; k < maskCount; ++k) {\n"
            "  vec4 range = bloom_mask_data.values[1 + 3 * k];\n"
            "  int mode = int(range.z);\n"
            "  precise float m = bloom_mask_one(segments + 2 * int(range.x), int(range.y), "
            "bloom_mask_data.values[3 + 3 * k], bloom_mask_data.values[2 + 3 * k], range.w != 0.0, "
            "p);\n";
        glsl += "  if (mode == " + mode(document::LayerMaskMode::Add) + ") a = a + m - a * m;\n";
        glsl += "  else if (mode == " + mode(document::LayerMaskMode::Subtract) +
                ") a = a * (1.0 - m);\n";
        glsl += "  else if (mode == " + mode(document::LayerMaskMode::Intersect) + ") a = a * m;\n";
        glsl += "  else if (mode == " + mode(document::LayerMaskMode::Difference) +
                ") a = a + m - 2.0 * a * m;\n";
        glsl += " }\n"
                " a = clamp(a, 0.0, 1.0);\n"
                " return pixel * a;\n"
                "}\n";
        return glsl;
    }();
    return text;
}

std::optional<std::vector<std::byte>> layerMaskProgramData(const PreparedLayerMasks& masks,
                                                           const std::int64_t originX,
                                                           const std::int64_t originY) {
    constexpr std::int64_t kExactInteger = std::int64_t{1} << 24;
    if (masks.masks.empty() || masks.masks.size() > kMaximumGpuMasks ||
        masks.segmentCount > kMaximumGpuMaskSegments || originX <= -kExactInteger ||
        originX >= kExactInteger || originY <= -kExactInteger || originY >= kExactInteger)
        return std::nullopt;
    const std::size_t maskCount = masks.masks.size();
    const std::size_t used = 1 + 3 * maskCount + 2 * masks.segmentCount;
    const std::size_t capacity = std::max<std::size_t>(64, std::bit_ceil(used));
    std::vector<float> values(capacity * 4, 0.0F);
    const auto put = [&values](const std::size_t index, const float x, const float y, const float z,
                               const float w) {
        values[index * 4] = x;
        values[index * 4 + 1] = y;
        values[index * 4 + 2] = z;
        values[index * 4 + 3] = w;
    };
    put(0, static_cast<float>(maskCount), masks.start, static_cast<float>(originX),
        static_cast<float>(originY));
    const std::size_t segmentBase = 1 + 3 * maskCount;
    std::size_t first = 0;
    for (std::size_t k = 0; k < maskCount; ++k) {
        const auto& mask = masks.masks[k];
        put(1 + 3 * k, static_cast<float>(first), static_cast<float>(mask.segments.size()),
            static_cast<float>(static_cast<std::int64_t>(mask.mode)), mask.inverted ? 1.0F : 0.0F);
        put(2 + 3 * k, mask.width, mask.inverseWidth, mask.expansion, mask.opacity);
        if (!mask.segments.empty())
            put(3 + 3 * k, mask.bounds[0], mask.bounds[1], mask.bounds[2], mask.bounds[3]);
        for (std::size_t i = 0; i < mask.segments.size(); ++i) {
            const auto& segment = mask.segments[i];
            const auto index = segmentBase + 2 * (first + i);
            put(index, segment[0], segment[1], segment[2], segment[3]);
            put(index + 1, mask.factors[i][0], mask.factors[i][1], 0.0F, 0.0F);
        }
        first += mask.segments.size();
    }
    std::vector<std::byte> bytes(values.size() * sizeof(float));
    std::memcpy(bytes.data(), values.data(), bytes.size());
    return bytes;
}

render::OcioGpuProgramResult buildLayerMaskProgram(const std::span<const std::byte> data) {
    if (data.empty() || data.size() % (4 * sizeof(float)) != 0)
        return render::OcioGpuProgramResult::failure(render::OcioGpuProgramError::InvalidRequest);
    render::OcioGpuProgramDesc program;
    program.stage = render::OcioGpuProgramStage::ProcessNeighborhood;
    program.functionName = "bloom_layer_mask";
    program.resourcePrefix = "bloom_mask_";
    program.shaderText = layerMaskProgramText();
    program.semanticsId = "bloom.render.layer-mask.v2";
    program.uniformBufferStorage = true;
    program.uniformBufferSize = data.size();
    program.uniformBufferData.assign(data.begin(), data.end());
    program.shaderTextDigest = render::computeOcioGpuShaderTextDigest(program.shaderText);
    program.resourceDigest = render::computeOcioGpuResourceDigest(program);
    program.contentIdentity = render::computeOcioGpuContentIdentity(
        {program.semanticsId, {}, {}, program.shaderTextDigest, program.resourceDigest});
    return render::OcioGpuProgramResult::success(std::move(program));
}

} // namespace bloom::runtime::detail
