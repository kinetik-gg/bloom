#include "channel_shuffle.hpp"

#include "operation_key.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace bloom::runtime::detail {
namespace {

constexpr std::int64_t kColourMode = 0;

struct SlotSource final {
    const float* plane = nullptr;
    std::optional<render::ImageWindow> window;
    float constant = 0.0F;
};

[[nodiscard]] const media::ImagePartInfo* partOf(const ImageSourceSelection& selection,
                                                 const std::string& part) {
    if (!selection.available || !selection.sourceRevision)
        return nullptr;
    return selection.sourceRevision->probe().findPart(part);
}

} // namespace

ChannelSetSourceSelection
selectChannelSetSources(const CompiledChannelExpression& expression, const core::RationalTime time,
                        const document::FrameRate rate, const std::filesystem::path& base,
                        const CancellationToken& cancel, const EvaluationColorIntent& colorIntent,
                        OperationCache* revisionCache,
                        const MediaSourceRevisionPolicy revisionPolicy,
                        const std::size_t hostByteBudget, const bool allowRevisionInsertion,
                        LinearPassRetention* const linearPass) {
    ChannelSetSourceSelection result;
    for (const auto& source : expression.sources) {
        const CompiledImageSource image{source.sourceNodeId,
                                        source.asset,
                                        source.startFrame,
                                        source.loopMode,
                                        source.colorSpace,
                                        source.inputColorSpaceId,
                                        false,
                                        source.part,
                                        {}};
        auto selected =
            selectImageSource(image, time, rate, base, cancel, colorIntent, revisionCache,
                              revisionPolicy, hostByteBudget, allowRevisionInsertion, linearPass);
        if (selected.cancelled) {
            result.cancelled = true;
            return result;
        }
        result.budgetExceeded = result.budgetExceeded || selected.budgetExceeded;
        if (!selected.available && result.warning.empty())
            result.warning =
                selected.warning.empty() ? "A channel source is unavailable" : selected.warning;
        const auto* part = partOf(selected, source.part);
        if (selected.available && part == nullptr && result.warning.empty())
            result.warning = "The selected OpenEXR part is not present: " + source.part;
        result.parts.push_back(part);
        result.sources.push_back(std::move(selected));
    }
    if (!result.warning.empty())
        return result;
    result.resolution = resolveChannelExpression(expression, result.parts);
    if (!result.resolution.ok())
        result.warning = result.resolution.error;
    return result;
}

ChannelShuffleSelection
selectChannelShuffle(const CompiledChannelShuffle& shuffle, const core::RationalTime time,
                     const document::FrameRate rate, const std::filesystem::path& base,
                     const CancellationToken& cancel, const EvaluationColorIntent& colorIntent,
                     OperationCache* revisionCache, const MediaSourceRevisionPolicy revisionPolicy,
                     const std::size_t hostByteBudget, const bool allowRevisionInsertion,
                     LinearPassRetention* const linearPass) {
    ChannelShuffleSelection result;
    result.associated = shuffle.associated;
    std::vector<const media::ImagePartInfo*> parts;
    for (const auto& source : shuffle.channels.sources) {
        const CompiledImageSource image{source.sourceNodeId, source.asset,     source.startFrame,
                                        source.loopMode,     source.colorSpace, source.inputColorSpaceId,
                                        false,               source.part,       {}};
        auto selected =
            selectImageSource(image, time, rate, base, cancel, colorIntent, revisionCache,
                              revisionPolicy, hostByteBudget, allowRevisionInsertion, linearPass);
        if (selected.cancelled) {
            result.cancelled = true;
            return result;
        }
        result.budgetExceeded = result.budgetExceeded || selected.budgetExceeded;
        if (!selected.available && result.warning.empty())
            result.warning = selected.warning.empty() ? "A channel source is unavailable"
                                                      : selected.warning;
        const auto* part = partOf(selected, source.part);
        if (selected.available && part == nullptr && result.warning.empty())
            result.warning = "The selected OpenEXR part is not present: " + source.part;
        parts.push_back(part);
        result.sources.push_back(std::move(selected));
    }
    OperationKey key;
    key.add(std::string{"channel-shuffle-v1"});
    for (const auto& source : result.sources)
        key.add(source.cacheKey);
    if (!result.warning.empty()) {
        result.cacheKey = key.bytes();
        return result;
    }
    const auto resolution = resolveChannelExpression(shuffle.channels, parts);
    if (!resolution.ok()) {
        result.warning = resolution.error;
        result.cacheKey = key.bytes();
        return result;
    }
    bool anchored = false;
    for (std::size_t slot = 0; slot < 4; ++slot) {
        auto resolved = resolveChannelSlot(resolution.channels, shuffle.mapping[slot]);
        if (!resolved) {
            result.warning = "Shuffle names a channel that is not in its input: " +
                             shuffle.mapping[slot];
            result.cacheKey = key.bytes();
            return result;
        }
        if (resolved->source && !anchored) {
            result.anchor = *resolved->source;
            anchored = true;
        }
        if (resolved->source && slot < 3 && shuffle.mode == kColourMode &&
            !result.colourSource)
            result.colourSource = *resolved->source;
        result.mapping[slot] = std::move(*resolved);
    }
    if (!anchored)
        result.anchor = 0; // every slot is a constant: the output uses the first source's frame
    if (result.anchor >= result.sources.size()) {
        result.warning = "The channel set has no source";
        result.cacheKey = key.bytes();
        return result;
    }
    OperationKey decodeKey;
    decodeKey.add(std::string{"channel-shuffle-raw-v1"});
    for (const auto& source : result.sources)
        decodeKey.add(source.decodeKey);
    decodeKey.add(result.anchor);
    for (const auto& slot : result.mapping) {
        for (auto* target : {&key, &decodeKey}) {
            target->add(slot.source.has_value());
            target->add(slot.source.value_or(0U));
            target->add(slot.sourceChannel);
            target->add(static_cast<double>(slot.constant));
        }
    }
    decodeKey.add(shuffle.associated);
    key.add(shuffle.associated);
    key.add(shuffle.mode);
    key.add(result.colourSource.has_value());
    key.add(result.colourSource.value_or(0U));
    result.cacheKey = key.bytes();
    result.decodeKey = decodeKey.digest();
    result.available = true;
    return result;
}

media::ImageResult<std::shared_ptr<const render::Rgba32fImage>>
gatherChannelShuffle(const ChannelShuffleSelection& selection, const bool convert,
                     const std::size_t budget, const CancellationToken& cancel) {
    using Result = media::ImageResult<std::shared_ptr<const render::Rgba32fImage>>;
    if (!selection.available)
        return {{}, selection.warning};
    const auto cancelled = [&cancel] { return cancel.isCancellationRequested(); };
    // Decode only the named channels of each contributing source.
    std::vector<std::vector<std::string>> names(selection.sources.size());
    for (const auto& slot : selection.mapping)
        if (slot.source && std::ranges::find(names[*slot.source], slot.sourceChannel) ==
                               names[*slot.source].end())
            names[*slot.source].push_back(slot.sourceChannel);
    std::vector<std::shared_ptr<const render::ChannelSetImage>> sets(selection.sources.size());
    std::size_t remaining = budget;
    std::uint64_t repairedPixels = 0;
    for (std::size_t index = 0; index < selection.sources.size(); ++index) {
        if (names[index].empty())
            continue;
        const auto& source = selection.sources[index];
        if (!source.sourceRevision)
            return {{}, "Channel source has no immutable revision"};
        media::ImageChannelSelection request;
        request.part = source.interpretation.part;
        request.channels = names[index];
        auto decoded = media::decodeImageChannels(*source.sourceRevision, request, cancelled, {},
                                                  remaining);
        if (!decoded.value.has_value())
            return {{}, decoded.diagnostic, decoded.cancelled, decoded.code};
        const auto cost = (*decoded.value)->byteCost();
        remaining = cost < remaining ? remaining - cost : 0;
        sets[index] = std::move(*decoded.value);
    }
    const auto* anchorPart = partOf(selection.sources[selection.anchor],
                                    selection.sources[selection.anchor].interpretation.part);
    if (anchorPart == nullptr)
        return {{}, "The channel source frame is unavailable"};
    const auto display = render::ImageWindow::create(anchorPart->displayX, anchorPart->displayY,
                                                     anchorPart->displayWidth,
                                                     anchorPart->displayHeight);
    if (!display)
        return {{}, "The channel source frame is invalid", false,
                media::ImageDiagnosticCode::InvalidDataWindow};
    // The output data window is the union of the contributing channels' windows; samples outside
    // a channel's own window are explicit zeros (ADR 0026 §4).
    std::optional<render::ImageWindow> union_;
    for (const auto& set : sets) {
        if (!set)
            continue;
        const auto window = set->dataWindow();
        if (!union_) {
            union_ = window;
            continue;
        }
        const auto left = std::min(union_->originX(), window.originX());
        const auto top = std::min(union_->originY(), window.originY());
        const auto right = std::max(union_->maxXExclusive(), window.maxXExclusive());
        const auto bottom = std::max(union_->maxYExclusive(), window.maxYExclusive());
        const auto merged = render::ImageWindow::create(
            left, top, static_cast<std::uint64_t>(right - left),
            static_cast<std::uint64_t>(bottom - top));
        if (!merged)
            return {{}, "The channel windows cannot be combined", false,
                    media::ImageDiagnosticCode::InvalidDataWindow};
        union_ = *merged.value();
    }
    const auto dataWindow = union_.value_or(*display.value());
    const auto descriptor = render::Rgba32fImageDescriptor::create(
        dataWindow, *display.value(), core::PixelAspectRatio::square());
    if (!descriptor)
        return {{}, "Shuffle output descriptor is invalid", false,
                media::ImageDiagnosticCode::InvalidDataWindow};
    auto builder = render::Rgba32fImageBuilder::create(*descriptor.value(), remaining);
    if (!builder)
        return {{}, "Shuffle output exceeds the pixel storage budget", false,
                media::ImageDiagnosticCode::PixelBudgetExceeded};

    std::array<SlotSource, 4> slots{};
    for (std::size_t slot = 0; slot < 4; ++slot) {
        const auto& resolved = selection.mapping[slot];
        if (!resolved.source) {
            slots[slot].constant = resolved.constant;
            continue;
        }
        const auto& set = sets[*resolved.source];
        const auto plane = set->find(resolved.sourceChannel);
        if (!plane)
            return {{}, "A shuffled channel was not decoded"};
        slots[slot].plane = set->plane(*plane).data();
        slots[slot].window = set->dataWindow();
    }
    const color::CpuColorSpaceProcessor* processor =
        convert && selection.colourSource
            ? selection.sources[*selection.colourSource].inputProcessor.get()
            : nullptr;
    const bool associated = selection.associated;
    const auto width = static_cast<std::size_t>(dataWindow.extent().width());
    std::vector<std::array<float, 4>> row(width);
    std::vector<bool> repairedRow(width);
    for (auto y = dataWindow.originY(); y < dataWindow.maxYExclusive(); ++y) {
        if (cancelled())
            return {{}, {}, true, media::ImageDiagnosticCode::Cancelled};
        for (std::size_t column = 0; column < width; ++column) {
            const auto x = dataWindow.originX() + static_cast<std::int64_t>(column);
            auto& target = row[column];
            for (std::size_t slot = 0; slot < 4; ++slot) {
                const auto& source = slots[slot];
                if (source.plane == nullptr) {
                    target[slot] = source.constant;
                } else if (source.window->contains(x, y)) {
                    const auto offset =
                        static_cast<std::size_t>(y - source.window->originY()) *
                            static_cast<std::size_t>(source.window->extent().width()) +
                        static_cast<std::size_t>(x - source.window->originX());
                    target[slot] = source.plane[offset];
                } else {
                    target[slot] = 0.0F;
                }
            }
            // The OpenEXR ingest policy (media-io.md): non-finite components become zero and
            // alpha is clamped to [0, 1] before any colour conversion.
            bool repaired = false;
            for (auto& component : target)
                if (!std::isfinite(component)) {
                    component = 0.0F;
                    repaired = true;
                }
            if (target[3] < 0.0F || target[3] > 1.0F) {
                target[3] = std::clamp(target[3], 0.0F, 1.0F);
                repaired = true;
            }
            if (associated && target[3] == 0.0F &&
                (target[0] != 0.0F || target[1] != 0.0F || target[2] != 0.0F))
                repaired = true;
            repairedRow[column] = repaired;
            if (associated && processor != nullptr)
                for (std::size_t channel = 0; channel < 3; ++channel)
                    target[channel] = target[3] > 0.0F ? target[channel] / target[3] : 0.0F;
        }
        if (processor != nullptr && !processor->apply(row))
            return {{}, "Shuffle color conversion failed", false,
                    media::ImageDiagnosticCode::ColorSpaceUnavailable};
        auto outputRow = builder.value()->row(y);
        if (!outputRow)
            return {{}, "Shuffle output row is invalid", false,
                    media::ImageDiagnosticCode::InvalidDataWindow};
        for (std::size_t column = 0; column < width; ++column) {
            auto sample = row[column];
            bool repaired = repairedRow[column];
            for (auto& component : sample)
                if (!std::isfinite(component)) {
                    component = 0.0F;
                    repaired = true;
                }
            if (sample[3] < 0.0F || sample[3] > 1.0F) {
                sample[3] = std::clamp(sample[3], 0.0F, 1.0F);
                repaired = true;
            }
            const bool keep = associated && processor == nullptr;
            auto red = keep ? sample[0] : sample[0] * sample[3];
            auto green = keep ? sample[1] : sample[1] * sample[3];
            auto blue = keep ? sample[2] : sample[2] * sample[3];
            if (sample[3] == 0.0F && (red != 0.0F || green != 0.0F || blue != 0.0F)) {
                red = green = blue = 0.0F;
                repaired = true;
            }
            auto pixel = render::Rgba32f::fromPremultiplied(red, green, blue, sample[3]);
            if (!pixel) {
                pixel = render::Rgba32f::fromPremultiplied(
                    std::isfinite(red) ? red : 0.0F, std::isfinite(green) ? green : 0.0F,
                    std::isfinite(blue) ? blue : 0.0F, sample[3]);
                repaired = true;
            }
            if (!pixel)
                return {{}, "Shuffle produced an invalid pixel", false,
                        media::ImageDiagnosticCode::DecodeFailed};
            if (repaired)
                ++repairedPixels;
            (*outputRow.value())[column] = *pixel.value();
        }
    }
    auto image = std::move(*builder.value()).freeze();
    if (!image)
        return {{}, "Shuffle output validation failed", false,
                media::ImageDiagnosticCode::DecodeFailed};
    Result result{std::make_shared<const render::Rgba32fImage>(std::move(*image.value())), {}};
    result.repairedPixels = repairedPixels;
    return result;
}

media::ImageResult<render::Rgba32fImage>
evaluateChannelShuffle(const ChannelShuffleSelection& selection,
                       const render::Rgba32fImageDescriptor composition,
                       const double horizontalScale, const double verticalScale,
                       const std::size_t budget, const CancellationToken& cancel) {
    auto gathered = gatherChannelShuffle(selection, selection.converts(), budget, cancel);
    if (!gathered.value.has_value())
        return {{}, gathered.diagnostic, gathered.cancelled, gathered.code};
    auto resampled = resampleDecodedImage(**gathered.value, composition, horizontalScale,
                                          verticalScale, budget, cancel);
    resampled.repairedPixels = gathered.repairedPixels;
    return resampled;
}

std::optional<ContentBounds>
channelShuffleFrameBounds(const ChannelShuffleSelection& selection) noexcept {
    if (!selection.available || selection.anchor >= selection.sources.size())
        return std::nullopt;
    return imageSourceFrameBounds(selection.sources[selection.anchor]);
}

} // namespace bloom::runtime::detail
