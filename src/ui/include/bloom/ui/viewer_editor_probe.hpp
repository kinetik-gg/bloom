#pragma once

#include <bloom/core/color.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/render/image_types.hpp>

#include <QMetaType>
#include <QString>

#include <optional>

namespace bloom::ui {
struct ProbeReadout final {
    bool valid = false;
    document::Vec2d coordinate{};
    render::Rgba8 display{};
    render::Rgba8 displayEncoded{};
    core::Color4d normalized{};
    // Exact premultiplied reference values, widened from Float32 without a color transform.
    std::optional<core::Color4d> reference = std::nullopt;
    std::optional<core::Color4d> working = std::nullopt;
    std::optional<core::Color4d> displayLinear = std::nullopt;
    QString workingColorSpaceId{};
    QString displayName{};
    bool pending = false;
    QString diagnostic{};
};
} // namespace bloom::ui
Q_DECLARE_METATYPE(bloom::ui::ProbeReadout)
