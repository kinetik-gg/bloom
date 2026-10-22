#pragma once

#include <bloom/color/bloom_neutral_builtin.hpp>
#include <bloom/document/color_settings.hpp>
#include <bloom/ui/frame_export_controller.hpp>

#include <string_view>
#include <variant>

namespace bloom::ui::detail {

struct NamedDeliverableAvailability final {
    bool available;
    std::string_view reason;
};

// These named deliverables use ACES2065-1 and ACES SDR display/view ids. They are not generic
// EXR/H.264 presets: the selected project config must contain those names by contract.
[[nodiscard]] inline NamedDeliverableAvailability
namedDeliverableAvailability(const document::ColorSettings& settings,
                             const DeliverablePreset deliverable) noexcept {
    if (deliverable == DeliverablePreset::Custom)
        return {true, {}};
    const auto* builtIn =
        std::get_if<document::BuiltInOcioConfigLocator>(&settings.ocioConfig.locator);
    if (!builtIn || std::string_view(builtIn->uri) != color::kAcesCgV1ConfigUri)
        return {false, "This deliverable requires the built-in ACES CG configuration. Select it "
                       "in Project Settings, or use a custom export preset."};
    if (settings.ocioConfig.expectedRevision.algorithm != document::OcioRevisionAlgorithm::Sha256 ||
        settings.ocioConfig.expectedRevision.digest == core::Sha256Digest{})
        return {false, "The selected ACES configuration has no pinned revision. Check Project "
                       "Settings before using this deliverable."};
    if (deliverable == DeliverablePreset::Review) {
        const auto video = output::outputPresetAvailabilityV1(output::OutputPresetV1::H264MovV1);
        if (!video.available)
            return {false, video.reason};
    }
    return {true, {}};
}

} // namespace bloom::ui::detail
