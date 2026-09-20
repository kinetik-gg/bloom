// Real FileTransform native parity: each GPU program is extracted through the isolated
// bloom-color-worker from the exact LUT bytes and executed on the device; the unchanged isolated
// CPU FileTransform processor is the oracle. Covers every supported container family (.cube, .clf,
// .spi1d, .spi3d), forward and inverse where OCIO supports the direction, a 1D curve, a 3D volume,
// and a steep/small 1D curve that exposes sub-texel sampler quantization.

#include "ocio_gpu_program_native_helpers.hpp"

#include <bloom/color/ocio_cpu_file_transform_processor.hpp>
#include <bloom/color/ocio_gpu_program.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/render/gpu_image_upload.hpp>
#include <bloom/render/gpu_ocio_program.hpp>
#include <bloom/render/ocio_gpu_program.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <span>
#include <string>
#include <vector>

namespace bloom::color::ocio_gpu_native_test {
namespace {

using bloom::render::GpuDevice;
using bloom::render::GpuImageUpload;
using bloom::render::GpuOcioProgramDiagnosticCode;
using bloom::render::GpuOcioProgramPollResult;
using bloom::render::OcioGpuInterpolation;
using bloom::render::OcioGpuProgramDesc;

struct Fixture final {
    const char* name;
    void (*write)(std::ofstream&);
    LutInterpolation interpolation;
    LutDirection direction;
};

// One program per supported container/shape. `Best` is OCIO's own default resolution: linear for
// 1D/2D resources and tetrahedral (manual, nearest-sampled) for 3D.
constexpr std::array<Fixture, 11> kFixtures{{
    {"curve1d.cube", &writeCurve1dCube, LutInterpolation::Best, LutDirection::Forward},
    {"curve1d-inverse.cube", &writeCurve1dCube, LutInterpolation::Best, LutDirection::Inverse},
    {"volume3d.cube", &writeVolume3dCube, LutInterpolation::Best, LutDirection::Forward},
    {"volume3d-inverse.cube", &writeVolume3dCube, LutInterpolation::Best, LutDirection::Inverse},
    {"steep1d.cube", &writeSteep1dCube, LutInterpolation::Best, LutDirection::Forward},
    {"identity.clf", &writeIdentityClf, LutInterpolation::Best, LutDirection::Forward},
    {"identity-inverse.clf", &writeIdentityClf, LutInterpolation::Best, LutDirection::Inverse},
    {"curve1d.spi1d", &writeCurve1dSpi, LutInterpolation::Best, LutDirection::Forward},
    {"curve1d-inverse.spi1d", &writeCurve1dSpi, LutInterpolation::Best, LutDirection::Inverse},
    {"volume3d.spi3d", &writeVolume3dSpi, LutInterpolation::Best, LutDirection::Forward},
    {"volume3d-inverse.spi3d", &writeVolume3dSpi, LutInterpolation::Best, LutDirection::Inverse},
}};

[[nodiscard]] bool hasLinearTexture(const OcioGpuProgramDesc& program) {
    return std::any_of(program.textures.begin(), program.textures.end(), [](const auto& texture) {
        return texture.interpolation == OcioGpuInterpolation::Linear;
    });
}

void verifySamplingAdapter(Expectations& expectations, const OcioGpuProgramDesc& program,
                           const std::string_view name) {
    const auto sampling = bloom::color::ocioGpuSamplingGlslFor(program);
    if (program.textures.empty()) {
        expectations.expect(!sampling.dispatches(),
                            std::string("a texture-free program needs no sampler dispatch: ") +
                                std::string(name));
        return;
    }
    // Every supported reflected texture has a valid sampler name, so the per-sampler dispatch must
    // engage; the generated body is never left on imprecise hardware linear filtering.
    expectations.expect(sampling.dispatches(),
                        std::string("a textured program dispatches per-sampler sampling: ") +
                            std::string(name));
    if (!sampling.dispatches()) {
        return;
    }
    expectations.expect(sampling.definitions.find("texelFetch(") != std::string::npos,
                        std::string("per-sampler definitions use texelFetch: ") +
                            std::string(name));
    const bool linear = hasLinearTexture(program);
    expectations.expect(linear == (sampling.definitions.find("mix(") != std::string::npos),
                        std::string("linear metadata selects the interpolating sampler: ") +
                            std::string(name));
}

} // namespace

void testFileTransform(Expectations& expectations, GpuDevice& device,
                       const ResolvedBloomNeutralConfig& resolved) {
    const auto base = std::filesystem::temp_directory_path() /
                      ("bloom_ocio_ft_" + std::to_string(std::random_device{}()));
    std::error_code directoryError;
    std::filesystem::create_directories(base, directoryError);
    if (directoryError) {
        expectations.expect(false, "the FileTransform fixture directory is creatable");
        return;
    }
    const std::string process(resolved.processColorSpaceId());
    for (const auto& fixture : kFixtures) {
        const auto path = base / fixture.name;
        {
            std::ofstream file(path);
            fixture.write(file);
        }
        const auto resource = bloom::color::readLutFile(path);
        if (resource.error != bloom::color::LutError::None) {
            expectations.expect(false, std::string("the FileTransform fixture passes preflight: ") +
                                           fixture.name);
            continue;
        }
        auto desc = bloom::color::buildOcioGpuProgramForFileTransform(
            resolved, resource, fixture.interpolation, fixture.direction, process, process);
        if (!desc.succeeded()) {
            std::cerr << "FileTransform extraction failed for " << fixture.name << ": "
                      << bloom::render::ocioGpuProgramErrorName(desc.error()) << '\n';
        }
        expectations.expect(desc.succeeded(),
                            std::string("the FileTransform GPU program extracts: ") + fixture.name);
        if (desc.program() == nullptr) {
            continue;
        }
        verifySamplingAdapter(expectations, *desc.program(), fixture.name);
        auto program = makeProgram(device, *desc.program());
        auto uploader = GpuImageUpload::create(device);
        expectations.expect(program != nullptr && uploader.hasValue(),
                            std::string("the FileTransform program hosts: ") + fixture.name);
        if (!program || *program == nullptr || !uploader) {
            continue;
        }
        constexpr std::uint32_t width = 5;
        constexpr std::uint32_t height = 3;
        const auto pixels = fixturePixels(width, height);
        const auto input = uploadImage(*uploader.upload, width, height, pixels);
        expectations.expect(input != nullptr,
                            std::string("the FileTransform input uploads: ") + fixture.name);
        if (input == nullptr) {
            continue;
        }
        const auto accepted = (*program)->beginEffect(input, {}, kBudget);
        expectations.expect(accepted.code == GpuOcioProgramDiagnosticCode::None,
                            std::string("beginEffect accepts the FileTransform: ") + fixture.name);
        auto poll = (*program)->poll();
        while (poll == GpuOcioProgramPollResult::Pending) {
            poll = (*program)->poll();
        }
        expectations.expect(poll == GpuOcioProgramPollResult::Ready,
                            std::string("the FileTransform dispatch completes: ") + fixture.name);
        if (poll != GpuOcioProgramPollResult::Ready) {
            continue;
        }
        auto output = (*program)->takeEffectOutput();
        expectations.expect(output != nullptr,
                            std::string("the FileTransform resident output publishes: ") +
                                fixture.name);
        if (output == nullptr) {
            continue;
        }
        const auto readback = bloom::render::readbackResidentImage(*output, kBudget);
        expectations.expect(readback.hasValue(),
                            std::string("the FileTransform output reads back: ") + fixture.name);
        if (!readback) {
            continue;
        }
        // Unchanged isolated CPU oracle: the same helper-built CPU FileTransform processor.
        auto cpu = bloom::color::CpuFileTransformProcessor::prepare(resource, fixture.interpolation,
                                                                    fixture.direction);
        expectations.expect(cpu.processor != nullptr,
                            std::string("the isolated CPU FileTransform oracle prepares: ") +
                                fixture.name);
        if (!cpu.processor) {
            continue;
        }
        std::size_t mismatches = 0;
        double maxAbsoluteError = 0.0;
        for (std::size_t index = 0; index < pixels.size(); ++index) {
            const auto& source = pixels[index];
            std::array<float, 4> straight{0.0F, 0.0F, 0.0F, 0.0F};
            if (source.alpha() != 0.0F) {
                straight = {source.red() / source.alpha(), source.green() / source.alpha(),
                            source.blue() / source.alpha(), source.alpha()};
            }
            std::span<std::array<float, 4>> span(&straight, 1);
            if (cpu.processor->apply(span) != bloom::color::LutError::None) {
                ++mismatches;
                continue;
            }
            const float expectedR = straight[0] * source.alpha();
            const float expectedG = straight[1] * source.alpha();
            const float expectedB = straight[2] * source.alpha();
            const auto& gpu = readback.pixels[index];
            // Strict frozen 2e-6 absolute-or-relative gate with exact alpha, unchanged from the
            // CST arm. The shared per-sampler adapter makes the 1D/2D linear resources exact; the
            // 3D tetrahedral body samples exact centers.
            const auto close = [](const float a, const float b) {
                if (a == b) {
                    return true;
                }
                const double absolute = std::fabs(static_cast<double>(a) - static_cast<double>(b));
                const double magnitude =
                    std::max(std::fabs(static_cast<double>(a)), std::fabs(static_cast<double>(b)));
                return absolute <= 2e-6 || absolute <= 2e-6 * magnitude;
            };
            if (!close(gpu.red(), expectedR) || !close(gpu.green(), expectedG) ||
                !close(gpu.blue(), expectedB) || gpu.alpha() != source.alpha()) {
                ++mismatches;
            }
            maxAbsoluteError = std::max(
                maxAbsoluteError,
                std::max(
                    {std::fabs(static_cast<double>(gpu.red()) - static_cast<double>(expectedR)),
                     std::fabs(static_cast<double>(gpu.green()) - static_cast<double>(expectedG)),
                     std::fabs(static_cast<double>(gpu.blue()) - static_cast<double>(expectedB))}));
        }
        if (mismatches != 0) {
            std::cerr << "  " << fixture.name << " mismatches=" << mismatches
                      << " maxAbsError=" << maxAbsoluteError << '\n';
        }
        expectations.expect(mismatches == 0,
                            std::string("every FileTransform pixel is within the strict 2e-6 "
                                        "CPU oracle gate: ") +
                                fixture.name);
    }
    std::filesystem::remove_all(base, directoryError);
}

} // namespace bloom::color::ocio_gpu_native_test
