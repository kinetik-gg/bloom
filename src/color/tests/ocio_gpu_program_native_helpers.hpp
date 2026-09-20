#pragma once

// Private shared machinery for the native OCIO GPU program tests. Kept separate so each handwritten
// test translation unit stays focused and small. Not installed and not part of any public header
// set.

#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/render/gpu_image_upload.hpp>
#include <bloom/render/gpu_ocio_program.hpp>
#include <bloom/render/ocio_gpu_program.hpp>

#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bloom::color::ocio_gpu_native_test {

inline constexpr std::uint64_t kBudget = std::uint64_t{1} << 32;
inline constexpr int kSkipExit = 77;

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << "FAILED: " << message << '\n';
    }
    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

// Fixture LUT writers, one per supported container family and shape.
void writeCurve1dCube(std::ofstream& file);
void writeVolume3dCube(std::ofstream& file);
void writeSteep1dCube(std::ofstream& file);
void writeIdentityClf(std::ofstream& file);
void writeCurve1dSpi(std::ofstream& file);
void writeVolume3dSpi(std::ofstream& file);

[[nodiscard]] bool parseRequireDevice(int argc, char** argv);

[[nodiscard]] std::vector<bloom::render::Rgba32f> fixturePixels(std::uint32_t width,
                                                                std::uint32_t height);

[[nodiscard]] std::shared_ptr<const bloom::render::GpuImage>
uploadImage(bloom::render::GpuImageUpload& uploader, std::uint32_t width, std::uint32_t height,
            const std::vector<bloom::render::Rgba32f>& pixels);

[[nodiscard]] std::optional<std::vector<std::uint32_t>> compileGlsl(const std::string& glsl);

[[nodiscard]] std::string buildWrapperGlsl(const bloom::render::OcioGpuProgramDesc& desc,
                                           bool display);

[[nodiscard]] std::optional<std::shared_ptr<bloom::render::GpuOcioProgram>>
makeProgram(bloom::render::GpuDevice& device, bloom::render::OcioGpuProgramDesc desc);

// Bounded, non-busy completion wait for one native OCIO job. Polls with a short sleep and never
// loops without a finite attempt ceiling; a job that does not retire reports a failure instead of
// hanging. This is the shared wait for new tests (existing tests keep their own loops).
[[nodiscard]] bloom::render::GpuOcioProgramPollResult
awaitOcioCompletion(bloom::render::GpuOcioProgram& program, Expectations& expectations);

[[nodiscard]] std::uint8_t quantize(double value);

// Test entry points, one per focused translation unit.
void testEffectCst(Expectations& expectations, bloom::render::GpuDevice& device,
                   const ResolvedBloomNeutralConfig& resolved);
void testDisplay(Expectations& expectations, bloom::render::GpuDevice& device,
                 const ResolvedBloomNeutralConfig& resolved);
void testFileTransform(Expectations& expectations, bloom::render::GpuDevice& device,
                       const ResolvedBloomNeutralConfig& resolved);
// Geometry grow/shrink across jobs on one program, strict parity, budget refusal + recovery, and
// the actual-allocation accessor for both arms.
void testGeometryAndBudgetLifecycle(Expectations& expectations, bloom::render::GpuDevice& device,
                                    const ResolvedBloomNeutralConfig& neutral,
                                    const ResolvedBloomNeutralConfig& aces);

} // namespace bloom::color::ocio_gpu_native_test
