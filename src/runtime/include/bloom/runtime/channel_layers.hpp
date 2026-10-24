#pragma once

// ADR 0026 §7: the channel layers a layered OpenEXR writes beside the RGBA process image. They are
// source values (decode layout only: selected source samples placed on the output data window,
// zero outside a channel's own window) assembled on the host at the codec boundary.

#include <bloom/core/rational_time.hpp>
#include <bloom/render/channel_set_image.hpp>
#include <bloom/runtime/cancellation.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/evaluation.hpp>

#include <filesystem>
#include <memory>
#include <string>

namespace bloom::runtime {

struct ChannelLayerAssembly final {
    // Null when the plan's output has no channel layers.
    std::shared_ptr<const render::ChannelSetImage> layers;
    std::string diagnostic;
    bool cancelled = false;
    [[nodiscard]] bool ok() const noexcept { return diagnostic.empty() && !cancelled; }
};

// Worker-only. Channels named R, G, B or A belong to the RGBA root and are never written as layers.
[[nodiscard]] ChannelLayerAssembly
assembleOutputChannelLayers(const CompiledCompositionPlan& plan, core::RationalTime time,
                            const std::filesystem::path& assetBase,
                            const EvaluationColorIntent& colorIntent,
                            render::ImageWindow dataWindow, render::ImageWindow displayWindow,
                            std::size_t byteBudget, const CancellationToken& cancellation);

} // namespace bloom::runtime
