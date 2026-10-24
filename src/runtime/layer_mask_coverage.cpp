#include "layer_mask_coverage.hpp"

#include <bloom/core/sha256.hpp>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <charconv>
#include <cmath>
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

void appendFloat(std::string& out, const float value) {
    char text[32];
    const auto result =
        std::to_chars(text, text + sizeof(text), value, std::chars_format::scientific, 9);
    out.append(text, result.ptr);
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
            item.factors.push_back({dy != 0.0F ? dx / dy : 0.0F,
                                    length2 > 0.0F ? 1.0F / length2 : 0.0F});
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

std::optional<std::string> layerMaskProgramText(const PreparedLayerMasks& masks,
                                                const std::int64_t originX,
                                                const std::int64_t originY) {
    if (masks.masks.empty() || masks.segmentCount > kMaximumGpuMaskSegments)
        return std::nullopt;
    std::string text;
    // Every segment of every mask in one array; each mask names its range. The per-pixel math is
    // the CPU reference (layerMaskCoverage) operation for operation, in float32.
    text += "const vec4 bloom_mask_segments[" +
            std::to_string(std::max<std::size_t>(masks.segmentCount, 1)) + "] = vec4[](";
    bool firstValue = true;
    for (const auto& mask : masks.masks)
        for (const auto& segment : mask.segments) {
            if (!firstValue)
                text += ',';
            firstValue = false;
            text += "vec4(";
            for (std::size_t i = 0; i < 4; ++i) {
                if (i)
                    text += ',';
                appendFloat(text, segment[i]);
            }
            text += ')';
        }
    if (firstValue)
        text += "vec4(0.0)";
    text += ");\n";
    text += "const vec2 bloom_mask_factors[" +
            std::to_string(std::max<std::size_t>(masks.segmentCount, 1)) + "] = vec2[](";
    firstValue = true;
    for (const auto& mask : masks.masks)
        for (const auto& factor : mask.factors) {
            if (!firstValue)
                text += ',';
            firstValue = false;
            text += "vec2(";
            appendFloat(text, factor[0]);
            text += ',';
            appendFloat(text, factor[1]);
            text += ')';
        }
    if (firstValue)
        text += "vec2(0.0)";
    text += ");\n";
    text += "float bloom_mask_sqrt(float value) {\n"
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
            "}\n";
    text +=
        "float bloom_mask_one(int first, int count, vec4 box, float width, float inverseWidth, "
        "float expansion, bool inverted, float opacity, vec2 p) {\n"
        " precise float coverage = 0.0;\n"
        " float reach = abs(expansion) + width * 0.5 + 1.0;\n"
        " if (count > 0 && p.x >= box.x - reach && p.x <= box.z + reach && p.y >= box.y - reach "
        "&& p.y <= box.w + reach) {\n"
        "  int winding = 0;\n"
        "  precise float nearest = 3.4e38;\n"
        "  for (int i = first; i < first + count; ++i) {\n"
        "   vec4 s = bloom_mask_segments[i];\n"
            "   vec2 f = bloom_mask_factors[i];\n"
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
        "  precise float signedDistance = winding != 0 ? -unsignedDistance : unsignedDistance;\n"
        "  coverage = clamp(0.5 - (signedDistance - expansion) * inverseWidth, 0.0, 1.0);\n"
        " }\n"
        " if (inverted) coverage = 1.0 - coverage;\n"
        " return coverage * opacity;\n"
        "}\n";
    text += "vec4 bloom_layer_mask(ivec2 c) {\n vec4 pixel = imageLoad(bloom_ocio_input, c);\n";
    text += " vec2 p = vec2(float(c.x + (" + std::to_string(originX) + ")) + 0.5, float(c.y + (" +
            std::to_string(originY) + ")) + 0.5);\n";
    text += " precise float a = ";
    appendFloat(text, masks.start);
    text += ";\n precise float m = 0.0;\n";
    std::size_t first = 0;
    for (const auto& mask : masks.masks) {
        text += " m = bloom_mask_one(" + std::to_string(first) + ", " +
                std::to_string(mask.segments.size()) + ", vec4(";
        for (std::size_t i = 0; i < 4; ++i) {
            if (i)
                text += ',';
            appendFloat(text, mask.segments.empty() ? 0.0F : mask.bounds[i]);
        }
        text += "), ";
        appendFloat(text, mask.width);
        text += ", ";
        appendFloat(text, mask.inverseWidth);
        text += ", ";
        appendFloat(text, mask.expansion);
        text += mask.inverted ? ", true, " : ", false, ";
        appendFloat(text, mask.opacity);
        text += ", p);\n";
        switch (mask.mode) {
        case document::LayerMaskMode::Add:
            text += " a = a + m - a * m;\n";
            break;
        case document::LayerMaskMode::Subtract:
            text += " a = a * (1.0 - m);\n";
            break;
        case document::LayerMaskMode::Intersect:
            text += " a = a * m;\n";
            break;
        case document::LayerMaskMode::Difference:
            text += " a = a + m - 2.0 * a * m;\n";
            break;
        case document::LayerMaskMode::None:
            break;
        }
        first += mask.segments.size();
    }
    text += " a = clamp(a, 0.0, 1.0);\n return pixel * a;\n}\n";
    return text;
}

render::OcioGpuProgramResult buildLayerMaskProgram(const std::string& shaderText) {
    if (shaderText.empty())
        return render::OcioGpuProgramResult::failure(render::OcioGpuProgramError::InvalidRequest);
    render::OcioGpuProgramDesc program;
    program.stage = render::OcioGpuProgramStage::ProcessNeighborhood;
    program.functionName = "bloom_layer_mask";
    program.resourcePrefix = "bloom_mask_";
    program.shaderText = shaderText;
    program.semanticsId = "bloom.render.layer-mask.v1";
    program.shaderTextDigest = render::computeOcioGpuShaderTextDigest(program.shaderText);
    program.resourceDigest = render::computeOcioGpuResourceDigest(program);
    program.contentIdentity = render::computeOcioGpuContentIdentity(
        {program.semanticsId, {}, {}, program.shaderTextDigest, program.resourceDigest});
    return render::OcioGpuProgramResult::success(std::move(program));
}

} // namespace bloom::runtime::detail
