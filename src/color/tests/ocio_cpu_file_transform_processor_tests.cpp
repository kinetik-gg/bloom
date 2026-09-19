#include <OpenColorIO/OpenColorIO.h>
#include <array>
#include <bloom/color/ocio_cpu_file_transform_processor.hpp>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {
using namespace bloom::color;
int failures = 0;
void expect(bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << message << '\n';
    }
}
void cube(const std::filesystem::path& path, const float gain) {
    std::ofstream file(path);
    file << "LUT_3D_SIZE 2\n";
    for (int b = 0; b < 2; ++b)
        for (int g = 0; g < 2; ++g)
            for (int r = 0; r < 2; ++r)
                file << static_cast<float>(r) * gain << ' ' << static_cast<float>(g) * gain << ' '
                     << static_cast<float>(b) * gain << '\n';
}
void testFormatsAndRefusals([[maybe_unused]] const std::filesystem::path& root) {
#ifdef __linux__
    const auto testFormat = [&root](const char* name, const std::string& contents) {
        const auto path = root / name;
        {
            std::ofstream file(path);
            file << contents;
        }
        const auto resource = readLutFile(path);
        expect(resource.error == LutError::None, "supported LUT format passes preflight");
        const auto prepared = CpuFileTransformProcessor::prepare(resource, LutInterpolation::Best,
                                                                 LutDirection::Forward);
        expect(prepared.processor != nullptr,
               "supported OCIO file format prepares through the helper");
        if (prepared.processor) {
            std::array<std::array<float, 4>, 1> pixels{{{0.25F, 0.5F, 0.75F, 0.5F}}};
            const auto before = pixels;
            expect(prepared.processor->apply(pixels) == LutError::None && pixels == before,
                   "supported identity file preserves RGB and alpha");
        }
    };
    testFormat("explicit-domain.cube", "DOMAIN_MIN 0 0 0\nDOMAIN_MAX 1 1 1\nLUT_3D_SIZE 2\n0 0 "
                                       "0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n");
    testFormat("shaper.cube", "LUT_1D_SIZE 2\nLUT_3D_SIZE 2\n0 0 0\n1 1 1\n0 0 0\n1 0 0\n0 1 0\n1 "
                              "1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n");
    testFormat("identity.spi1d",
               "Version 1\nFrom 0 1\nLength 2\nComponents 3\n{\n0 0 0\n1 1 1\n}\n");
    std::string spi3d = "SPILUT 1.0\n3 3\n2 2 2\n";
    for (int r = 0; r < 2; ++r)
        for (int g = 0; g < 2; ++g)
            for (int b = 0; b < 2; ++b) {
                const auto triple =
                    std::to_string(r) + " " + std::to_string(g) + " " + std::to_string(b);
                spi3d.append(triple).append(" ").append(triple).append("\n");
            }
    testFormat("identity.spi3d", spi3d);
    testFormat("identity.clf", R"(<?xml version="1.0" encoding="UTF-8"?>
<ProcessList id="identity" compCLFversion="3">
<Matrix inBitDepth="32f" outBitDepth="32f"><Array dim="3 3">1 0 0 0 1 0 0 0 1</Array></Matrix>
</ProcessList>)");
    const auto path = root / "gain.cube";
    cube(path, 2);
    const auto resource = readLutFile(path);
    const auto inverse =
        CpuFileTransformProcessor::prepare(resource, LutInterpolation::Best, LutDirection::Inverse);
    expect(inverse.processor != nullptr, "inverse LUT prepares");
    if (inverse.processor) {
        std::array<std::array<float, 4>, 1> pixels{{{0.5F, 1.0F, 1.5F, 0.375F}}};
        auto expected = pixels;
        auto transform = OCIO_NAMESPACE::FileTransform::Create();
        transform->setSrc(path.string().c_str());
        transform->setInterpolation(OCIO_NAMESPACE::INTERP_BEST);
        transform->setDirection(OCIO_NAMESPACE::TRANSFORM_DIR_INVERSE);
        const auto oracle =
            OCIO_NAMESPACE::Config::CreateRaw()->getProcessor(transform)->getDefaultCPUProcessor();
        oracle->applyRGB(expected[0].data());
        expect(inverse.processor->apply(pixels) == LutError::None && pixels == expected,
               "inverse Best interpolation matches the independent OCIO processor exactly");
        unsigned polls = 0;
        const auto cancelDuringExchange = [&polls] {
            ++polls;
            return polls >= 2;
        };
        expect(inverse.processor->apply(pixels, cancelDuringExchange) ==
                       LutError::HelperCancelled &&
                   !inverse.processor->isAvailable(),
               "in-flight cancellation invalidates the helper token");
    }
    auto forged = resource;
    const std::string hostile = "LUT_3D_SIZE 130\n";
    const auto bytes = std::as_bytes(std::span(hostile));
    forged.bytes.assign(bytes.begin(), bytes.end());
    const auto digest = bloom::core::Sha256Hasher::hash(forged.bytes);
    if (digest)
        forged.digest = *digest;
    const auto refused =
        CpuFileTransformProcessor::prepare(forged, LutInterpolation::Linear, LutDirection::Forward);
    expect(refused.error == LutError::EdgeTooLarge && !refused.processor,
           "helper independently refuses hostile bytes that bypassed host preflight");
    for (const auto format : {2U, 3U, 4U}) {
        forged.format = format;
        const auto disguised = CpuFileTransformProcessor::prepare(forged, LutInterpolation::Linear,
                                                                  LutDirection::Forward);
        expect(disguised.error == LutError::MalformedFile && !disguised.processor,
               "declaring another format cannot bypass the helper's cube edge refusal");
    }
    forged.digest = resource.digest;
    expect(
        CpuFileTransformProcessor::prepare(forged, LutInterpolation::Linear, LutDirection::Forward)
                .error == LutError::ChangedFile,
        "resource bytes must agree with their digest before IPC");
    const auto clf = root / "hostile.clf";
    {
        std::ofstream file(clf);
        file << R"(<ProcessList><LUT3D><Array dim="130 130 130 3"></Array></LUT3D></ProcessList>)";
    }
    expect(readLutFile(clf).error == LutError::EdgeTooLarge, "CLF 3D edge cap is enforced");
    {
        std::ofstream file(clf);
        file << R"(<ProcessList><Reference path="external.clf"/></ProcessList>)";
    }
    expect(readLutFile(clf).error == LutError::UnsupportedFormat,
           "CLF external resource access is refused");
    {
        // The declared CLF signature appears only in a cube title. OCIO auto-detects the cube
        // from the descriptor, so the parsed-operation check must enforce the edge limit too.
        std::ofstream file(clf);
        file << "TITLE \"<ProcessList>\"\nLUT_3D_SIZE 130\n";
        for (std::size_t row = 0; row < std::size_t{130} * 130U * 130U; ++row)
            file << "0 0 0\n";
    }
    const auto disguisedCube = readLutFile(clf);
    expect(disguisedCube.error == LutError::None,
           "disguised cube fixture reaches the supervised parser");
    const auto parsedRefusal = CpuFileTransformProcessor::prepare(
        disguisedCube, LutInterpolation::Linear, LutDirection::Forward);
    const bool gridRefused = parsedRefusal.error == LutError::EdgeTooLarge ||
                             parsedRefusal.error == LutError::MalformedFile;
    if (!gridRefused)
        std::cerr << "disguised cube refusal: " << lutErrorName(parsedRefusal.error) << '\n';
    expect(gridRefused && !parsedRefusal.processor,
           "OCIO auto-detection cannot admit a parsed 3D grid above the edge limit");
    std::error_code error;
    const auto link = root / "symlink.cube";
    std::filesystem::create_symlink(path, link, error);
    expect(!error && readLutFile(link).error == LutError::MissingFile,
           "LUT intake never follows a final symlink");
#else
    (void)root;
#endif
}

} // namespace
int main() {
    try {
        const auto root = std::filesystem::current_path() / "color3-lut-fixtures";
        std::filesystem::create_directories(root);
        const auto path = root / "identity.cube";
        cube(path, 1);
        const auto file = readLutFile(path);
#ifdef __linux__
        expect(file.error == LutError::None, "identity cube passes bounded preflight");
        auto result = CpuFileTransformProcessor::prepare(file, LutInterpolation::Linear,
                                                         LutDirection::Forward);
        if (!result.processor)
            std::cerr << "prepare: " << lutErrorName(result.error) << '\n';
        expect(result.processor != nullptr, "supervised identity cube prepares");
        if (result.processor) {
            std::array<std::array<float, 4>, 1> pixels{{{0.25F, 0.5F, 0.75F, 0.375F}}};
            const auto before = pixels;
            const auto error = result.processor->apply(pixels);
            if (error != LutError::None)
                std::cerr << "apply: " << lutErrorName(error) << '\n';
            expect(error == LutError::None && pixels == before,
                   "sealed slab identity transform is bit-identical");
        }
        cube(path, 2);
        const auto gain = readLutFile(path);
        expect(gain.digest != file.digest, "changed LUT bytes change content digest");
        auto doubled = CpuFileTransformProcessor::prepare(gain, LutInterpolation::Tetrahedral,
                                                          LutDirection::Forward);
        expect(doubled.processor != nullptr, "gain LUT prepares");
        if (doubled.processor) {
            std::array<std::array<float, 4>, 1> pixels{{{0.25F, 0.5F, 0.75F, 0.375F}}};
            expect(doubled.processor->apply(pixels) == LutError::None &&
                       pixels[0] == std::array<float, 4>{0.5F, 1, 1.5F, 0.375F},
                   "gain LUT changes RGB and preserves alpha");
            expect(doubled.processor->apply(pixels, [] { return true; }) ==
                       LutError::HelperCancelled,
                   "cancelled slab is refused");
        }
        {
            std::ofstream huge(path);
            huge << "LUT_3D_SIZE 130\n";
        }
        expect(readLutFile(path).error == LutError::EdgeTooLarge,
               "huge LUT edge is refused before OCIO");
        {
            std::ofstream truncated(path);
            truncated << "LUT_3D_SIZE 2\n0 0 0\n";
        }
        expect(readLutFile(path).error == LutError::MalformedFile, "truncated cube is refused");
        std::filesystem::resize_file(path, kMaximumLutBytes + 1);
        expect(readLutFile(path).error == LutError::FileTooLarge,
               "oversized LUT is refused before allocation");
#else
        expect(file.error == LutError::HelperUnavailable,
               "unqualified supervision has an explicit fallback");
#endif
        testFormatsAndRefusals(root);
        std::filesystem::remove_all(root);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
