#include <bloom/ui/ram_preview_pipeline.hpp>

#include <utility>

namespace bloom::ui {

bool RamPreviewPipeline::isInFlight(const PreviewFrameCacheKey& key) const {
    for (const auto& slot : slots_) {
        if (PreviewFrameCacheKey::forIdentity(slot.identity) == key) {
            return true;
        }
    }
    return false;
}

void RamPreviewPipeline::add(InFlight slot) {
    slots_.push_back(std::move(slot));
    if (slots_.size() > peakInFlight_) {
        peakInFlight_ = static_cast<std::uint32_t>(slots_.size());
    }
}

std::vector<RamPreviewPipeline::Ready> RamPreviewPipeline::takeReady() {
    std::vector<Ready> ready;
    for (std::size_t index = 0; index < slots_.size();) {
        auto result = slots_[index].handle.tryTakeResult();
        if (!result.has_value()) {
            ++index;
            continue;
        }
        ready.push_back(Ready{.frameIndex = slots_[index].frameIndex,
                              .identity = slots_[index].identity,
                              .result = std::move(*result)});
        slots_.erase(slots_.begin() + static_cast<std::ptrdiff_t>(index));
    }
    return ready;
}

void RamPreviewPipeline::cancelAllAndDetach() noexcept {
    for (auto& slot : slots_) {
        slot.handle.cancel();
    }
    slots_.clear();
}

} // namespace bloom::ui
