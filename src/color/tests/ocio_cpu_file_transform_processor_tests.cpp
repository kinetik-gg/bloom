#include <array>
#include <bloom/color/ocio_cpu_file_transform_processor.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>

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
        std::filesystem::remove_all(root);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
