#include <bloom/ui/composition_authoring.hpp>

#include "composition_editor_support.hpp"

#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/timeline_editor.hpp>

#include <bloom/core/color.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/parameter.hpp>

#include <QCoreApplication>

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <system_error>
#include <variant>

namespace bloom::ui {
namespace {

constexpr std::array kDefaultSolidPalette{
    core::Color4d{0.62, 0.08, 0.04, 1.0},
    core::Color4d{0.04, 0.20, 0.72, 1.0},
    core::Color4d{0.06, 0.52, 0.16, 1.0},
    core::Color4d{0.46, 0.07, 0.58, 1.0},
};

qulonglong nextLayerNumber(const CompositionSession& session, const std::string_view typeId,
                           const std::uint32_t schemaVersion) {
    qulonglong count = 0;
    const auto* composition = session.composition();
    if (composition == nullptr) {
        return 1;
    }
    for (const auto& entry : composition->graph().layerStack().entries()) {
        const auto* sourceNode = directSourceNode(session, entry.layerId);
        if (isKnownSource(sourceNode, typeId, schemaVersion)) {
            ++count;
        }
    }
    return count + 1;
}

QString exactNumber(const double value) {
    std::array<char, 64> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                                      std::chars_format::general);
    if (result.ec != std::errc{}) {
        return QString::number(value, 'g', std::numeric_limits<double>::max_digits10);
    }
    return QString::fromLatin1(buffer.data(), static_cast<qsizetype>(result.ptr - buffer.data()));
}

} // namespace

QString parameterSourceDescription(const document::ParameterRecord& parameter) {
    if (std::holds_alternative<document::AnimationCurveSource>(parameter.source)) {
        return QStringLiteral("Animated");
    }
    if (std::holds_alternative<document::DriverBindingSource>(parameter.source)) {
        return QStringLiteral("Driven by graph");
    }
    return QStringLiteral("Constant");
}

QString blendModeDisplayName(const core::BlendMode mode) {
    // QCoreApplication::translate() rather than QStringLiteral: this is an artist-facing vocabulary
    // shown in three controls, so it has to be translatable, and a free function has no tr() of its
    // own. The context is the vocabulary, not any one widget, because all three surfaces show the
    // same words.
    const auto* const name = [mode]() -> const char* {
        switch (mode) {
        case core::BlendMode::Normal:
            return "Normal";
        case core::BlendMode::Add:
            return "Add";
        case core::BlendMode::Multiply:
            return "Multiply";
        case core::BlendMode::Screen:
            return "Screen";
        case core::BlendMode::Overlay:
            return "Overlay";
        case core::BlendMode::Darken:
            return "Darken";
        case core::BlendMode::Lighten:
            return "Lighten";
        case core::BlendMode::Difference:
            return "Difference";
        }
        return "Normal";
    }();
    return QCoreApplication::translate("bloom::ui::BlendMode", name);
}

QString exactColorText(const core::Color4d color) {
    return QStringLiteral("R %1  G %2  B %3  A %4")
        .arg(exactNumber(color.red), exactNumber(color.green), exactNumber(color.blue),
             exactNumber(color.alpha));
}

bool addDefaultSolidLayer(CompositionSession& session) {
    const auto layerNumber = nextLayerNumber(session, document::kSolidSourceNodeType,
                                             document::kSolidSourceNodeSchemaVersion);
    const auto paletteIndex =
        static_cast<std::size_t>(layerNumber - 1) % kDefaultSolidPalette.size();
    return session.addSolidLayer(TimelineEditor::tr("Solid %1").arg(layerNumber),
                                 kDefaultSolidPalette[paletteIndex]);
}

bool addDefaultTextLayer(CompositionSession& session) {
    const auto layerNumber = nextLayerNumber(session, document::kTextSourceNodeType,
                                             document::kTextSourceNodeSchemaVersion);
    return session.addTextLayer(TimelineEditor::tr("Text %1").arg(layerNumber),
                                TimelineEditor::tr("Text"));
}

} // namespace bloom::ui
