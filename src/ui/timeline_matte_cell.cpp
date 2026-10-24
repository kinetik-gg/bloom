#include "timeline_matte_cell.hpp"

#include "layer_mask_support.hpp"

#include <QCoreApplication>
#include <QVariant>
#include <bloom/document/project.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/dropdown.hpp>

namespace bloom::ui::matte_cell {
namespace {
QString text(const char* source) {
    return QCoreApplication::translate("TimelineMatteCell", source);
}
} // namespace

void populate(kit::KDropdown& dropdown, const CompositionSession& session,
              const document::LayerId layer) {
    dropdown.clearItems();
    dropdown.addItem(text("None"), QVariant::fromValue(0LL));
    const auto* composition = session.composition();
    const auto* boundary = composition ? composition->graph().findLayer(layer) : nullptr;
    if (boundary == nullptr)
        return;
    for (const auto id : masks::matteSourceCandidates(*composition, layer))
        if (const auto* other = composition->graph().findLayer(id))
            dropdown.addItem(QString::fromStdString(other->name),
                             QVariant::fromValue(static_cast<long long>(id.value())));
    const auto matte = boundary->trackMatte;
    const bool luma = matte && matte->channel == document::TrackMatteChannel::Luma;
    const auto action = [&](const QString& label, const long long code) {
        const int index = dropdown.addItem(label, QVariant::fromValue(code));
        dropdown.setItemEnabled(index, matte.has_value());
    };
    action(luma ? text("Use Alpha") : text("Alpha (current)"), kActionAlpha);
    action(luma ? text("Luma (current)") : text("Use Luma"), kActionLuma);
    action(matte && matte->inverted ? text("Inverted (on)") : text("Inverted"), kActionInverted);
    dropdown.setCurrentIndex(dropdown.findData(
        QVariant::fromValue(matte ? static_cast<long long>(matte->source.value()) : 0LL)));
    dropdown.setToolTip(matte ? text("Track matte: %1%2")
                                    .arg(masks::channelText(matte->channel),
                                         matte->inverted ? text(", inverted") : QString())
                              : text("Use another layer as this layer's matte"));
}

bool activate(CompositionSession& session, const document::LayerId layer, const long long code) {
    const auto* composition = session.composition();
    const auto* boundary = composition ? composition->graph().findLayer(layer) : nullptr;
    if (boundary == nullptr)
        return false;
    auto matte = boundary->trackMatte;
    if (code >= 0) {
        if (code == 0)
            matte.reset();
        else if (matte)
            matte->source = document::LayerId::fromRaw(static_cast<std::uint64_t>(code));
        else
            matte =
                document::TrackMatte{document::LayerId::fromRaw(static_cast<std::uint64_t>(code)),
                                     document::TrackMatteChannel::Alpha, false};
    } else {
        if (!matte)
            return false;
        if (code == kActionAlpha)
            matte->channel = document::TrackMatteChannel::Alpha;
        else if (code == kActionLuma)
            matte->channel = document::TrackMatteChannel::Luma;
        else if (code == kActionInverted)
            matte->inverted = !matte->inverted;
    }
    if (matte == boundary->trackMatte)
        return false;
    return session.setTrackMatte(layer, matte);
}

} // namespace bloom::ui::matte_cell
