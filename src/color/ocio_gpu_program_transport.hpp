#pragma once

// A plain, link-light result used inside the bloom-color-worker boundary. It carries the same
// portable render::OcioGpuProgramDesc and error vocabulary as render::OcioGpuProgramResult, but
// needs no bloom_render symbol, so the isolated helper never links the native renderer. The host
// converts it into the public render::OcioGpuProgramResult.

#include <bloom/render/ocio_gpu_program.hpp>

#include <optional>

namespace bloom::color::detail {

struct OcioGpuProgramTransport final {
    std::optional<render::OcioGpuProgramDesc> program;
    render::OcioGpuProgramError error = render::OcioGpuProgramError::None;

    [[nodiscard]] bool succeeded() const noexcept { return program.has_value(); }
    [[nodiscard]] const render::OcioGpuProgramDesc* reflected() const& noexcept {
        return program ? &*program : nullptr;
    }
};

} // namespace bloom::color::detail
