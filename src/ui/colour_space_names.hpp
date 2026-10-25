#pragma once
#include <QObject>
#include <QString>
#include <bloom/color/display_processor_identity.hpp>
#include <bloom/color/ocio_builtin_registry.hpp>
#include <string_view>

namespace bloom::ui {
// The name an artist reads for an OCIO colour space (#380). Bloom Neutral names its spaces with
// interchange identifiers, so they get plain names; any other config's names are already written
// for people and are shown as the config spells them.
[[nodiscard]] inline QString colourSpaceDisplayName(const std::string_view id) {
    if (id == color::kDisplayProcessorIdentitySourceColorSpaceId)
        return QObject::tr("Linear Rec.709");
    if (id == color::kDisplayProcessorIdentityOutputColorSpaceId)
        return QObject::tr("sRGB");
    if (id == color::kBloomRec709CameraColorSpaceId)
        return QObject::tr("Rec.709 video");
    return QString::fromUtf8(id.data(), static_cast<qsizetype>(id.size()));
}
} // namespace bloom::ui
