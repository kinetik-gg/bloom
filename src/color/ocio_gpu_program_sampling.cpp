#include <bloom/color/ocio_gpu_program.hpp>

#include <algorithm>
#include <string>
#include <string_view>
#include <unordered_set>

namespace bloom::color {
namespace {

[[nodiscard]] bool isAsciiAlpha(const char character) noexcept {
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
           character == '_';
}

[[nodiscard]] bool validIdentifier(const std::string_view name) noexcept {
    if (name.empty() || !isAsciiAlpha(name.front())) {
        return false;
    }
    return std::all_of(name.begin() + 1, name.end(), [](const char character) {
        return isAsciiAlpha(character) || (character >= '0' && character <= '9');
    });
}

[[nodiscard]] const char*
coordinateType(const render::OcioGpuTextureDimensions dimensions) noexcept {
    switch (dimensions) {
    case render::OcioGpuTextureDimensions::OneD:
        return "float";
    case render::OcioGpuTextureDimensions::TwoD:
        return "vec2";
    case render::OcioGpuTextureDimensions::ThreeD:
        return "vec3";
    }
    return nullptr;
}

[[nodiscard]] std::string linearSampling(const std::string& name,
                                         const render::OcioGpuTextureDimensions dimensions) {
    switch (dimensions) {
    case render::OcioGpuTextureDimensions::OneD:
        return "    const int size = textureSize(" + name +
               ", 0);\n    const float position = coordinate * float(size) - 0.5;\n"
               "    const float base = floor(position);\n    const float fraction = position - "
               "base;\n    const int lower = clamp(int(base), 0, size - 1);\n"
               "    const int upper = clamp(int(base) + 1, 0, size - 1);\n"
               "    return mix(texelFetch(" +
               name + ", lower, 0), texelFetch(" + name + ", upper, 0), fraction);\n";
    case render::OcioGpuTextureDimensions::TwoD:
        return "    const ivec2 size = textureSize(" + name +
               ", 0);\n    const vec2 position = coordinate * vec2(size) - vec2(0.5);\n"
               "    const vec2 base = floor(position);\n    const vec2 fraction = position - "
               "base;\n    const ivec2 lower = clamp(ivec2(base), ivec2(0), size - ivec2(1));\n"
               "    const ivec2 upper = clamp(ivec2(base) + ivec2(1), ivec2(0), size - ivec2(1));\n"
               "    const vec4 v00 = texelFetch(" +
               name + ", ivec2(lower.x, lower.y), 0);\n    const vec4 v10 = texelFetch(" + name +
               ", ivec2(upper.x, lower.y), 0);\n    const vec4 v01 = texelFetch(" + name +
               ", ivec2(lower.x, upper.y), 0);\n    const vec4 v11 = texelFetch(" + name +
               ", ivec2(upper.x, upper.y), 0);\n    return mix(mix(v00, v10, fraction.x), "
               "mix(v01, v11, fraction.x), fraction.y);\n";
    case render::OcioGpuTextureDimensions::ThreeD:
        return "    const ivec3 size = textureSize(" + name +
               ", 0);\n    const vec3 position = coordinate * vec3(size) - vec3(0.5);\n"
               "    const vec3 base = floor(position);\n    const vec3 fraction = position - "
               "base;\n    const ivec3 lower = clamp(ivec3(base), ivec3(0), size - ivec3(1));\n"
               "    const ivec3 upper = clamp(ivec3(base) + ivec3(1), ivec3(0), size - ivec3(1));\n"
               "    const vec4 v000 = texelFetch(" +
               name + ", ivec3(lower.x, lower.y, lower.z), 0);\n    const vec4 v100 = texelFetch(" +
               name + ", ivec3(upper.x, lower.y, lower.z), 0);\n    const vec4 v010 = texelFetch(" +
               name + ", ivec3(lower.x, upper.y, lower.z), 0);\n    const vec4 v110 = texelFetch(" +
               name + ", ivec3(upper.x, upper.y, lower.z), 0);\n    const vec4 v001 = texelFetch(" +
               name + ", ivec3(lower.x, lower.y, upper.z), 0);\n    const vec4 v101 = texelFetch(" +
               name + ", ivec3(upper.x, lower.y, upper.z), 0);\n    const vec4 v011 = texelFetch(" +
               name + ", ivec3(lower.x, upper.y, upper.z), 0);\n    const vec4 v111 = texelFetch(" +
               name +
               ", ivec3(upper.x, upper.y, upper.z), 0);\n    const vec4 v00 = mix(v000, "
               "v100, fraction.x);\n    const vec4 v01 = mix(v010, v110, fraction.x);\n"
               "    const vec4 v10 = mix(v001, v101, fraction.x);\n    const vec4 v11 = mix(v011, "
               "v111, fraction.x);\n    return mix(mix(v00, v01, fraction.y), mix(v10, v11, "
               "fraction.y), fraction.z);\n";
    }
    return {};
}

[[nodiscard]] std::string nearestSampling(const std::string& name,
                                          const render::OcioGpuTextureDimensions dimensions) {
    switch (dimensions) {
    case render::OcioGpuTextureDimensions::OneD:
        return "    const int size = textureSize(" + name + ", 0);\n    return texelFetch(" + name +
               ", clamp(int(floor(coordinate * float(size))), 0, size - 1), 0);\n";
    case render::OcioGpuTextureDimensions::TwoD:
        return "    const ivec2 size = textureSize(" + name + ", 0);\n    return texelFetch(" +
               name +
               ", clamp(ivec2(floor(coordinate * vec2(size))), ivec2(0), size - ivec2(1)), 0);\n";
    case render::OcioGpuTextureDimensions::ThreeD:
        return "    const ivec3 size = textureSize(" + name + ", 0);\n    return texelFetch(" +
               name +
               ", clamp(ivec3(floor(coordinate * vec3(size))), ivec3(0), size - ivec3(1)), 0);\n";
    }
    return {};
}

} // namespace

OcioGpuSamplingGlsl ocioGpuSamplingGlslFor(const render::OcioGpuProgramDesc& program) {
    OcioGpuSamplingGlsl result;
    if (program.textures.empty()) {
        return result;
    }
    std::unordered_set<std::string> samplerNames;
    for (const auto& texture : program.textures) {
        if (!validIdentifier(texture.samplerName) ||
            coordinateType(texture.dimensions) == nullptr ||
            !samplerNames.insert(texture.samplerName).second) {
            // An unnameable/duplicated sampler cannot receive a safe per-sampler override; leave
            // the whole program on the generated hardware sampling rather than emitting a partial
            // or ambiguous adapter.
            return {};
        }
    }
    std::string declarations;
    std::string definitions = "#undef texture\n";
    for (const auto& texture : program.textures) {
        const std::string function = "bloom_ocio_sample_" + texture.samplerName;
        const std::string coordinate = coordinateType(texture.dimensions);
        declarations += "vec4 " + function + "(" + coordinate + " coordinate);\n";
        definitions += "vec4 " + function + "(" + coordinate + " coordinate)\n{\n";
        switch (texture.interpolation) {
        case render::OcioGpuInterpolation::Linear:
            definitions += linearSampling(texture.samplerName, texture.dimensions);
            break;
        case render::OcioGpuInterpolation::Nearest:
        case render::OcioGpuInterpolation::Tetrahedral:
            // OCIO samples its tetrahedral body only at exact texel centers, so a nearest fetch is
            // exact. Linear-interpolated resources instead get the full-precision linear sampler.
            definitions += nearestSampling(texture.samplerName, texture.dimensions);
            break;
        default:
            definitions += "    return texture(" + texture.samplerName + ", coordinate);\n";
            break;
        }
        definitions += "}\n\n";
    }
    result.preamble = declarations + "#define texture(s, c) bloom_ocio_sample_##s(c)\n";
    result.definitions = std::move(definitions);
    return result;
}

} // namespace bloom::color
