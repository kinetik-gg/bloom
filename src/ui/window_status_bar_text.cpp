// The status bar's text and colour rules, kept free of widgets so tests and the segments read the
// same words (docs/ux/ui-grammar.md, "Status bar").
#include <bloom/ui/window_status_bar.hpp>

#include "window_status_bar_segments.hpp"

#include <bloom/media/cache/media_disk_cache.hpp>
#include <bloom/runtime/operation_cache.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/preview_frame_cache.hpp>

#include <cmath>

namespace bloom::ui {

using statusbar::formatBytes;

PreviewColorState previewColorState(const CompositionPreviewState& preview) {
    if (preview.activity == PreviewActivity::Failed &&
        (!preview.viewerDisplayState || !preview.frame)) {
        return {preview.message.isEmpty() ? WindowStatusBar::tr("Color state unavailable")
                                          : preview.message,
                kit::Color::Error, WindowStatusBar::tr("None")};
    }
    const auto bufferView =
        preview.frame != nullptr ? preview.frame->displayBufferView() : std::nullopt;
    if (preview.frame != nullptr) {
        const auto provider = preview.frame->provenance().provider;
        // The resident arm keeps no CPU display buffer by construction (its pixels never crossed
        // the host boundary), so `bufferView` can never classify it and the reference fallback
        // below would falsely report the CPU "Reference (unqualified)" state. Read the immutable
        // provenance instead. A valid admission lease or an exact known owner-applied descriptor
        // identifies GPU pixels. Lease invalidation alone does not erase that applied metadata;
        // uncorrelated/blank displays remain unavailable and can never become CPU reference.
        if (provider == runtime::PreviewDisplayProvider::GpuResident) {
            const auto& resident = preview.frame->residentFrame();
            if (preview.ownerAppliedDisplay ||
                (resident != nullptr && resident->isDisplayValid())) {
                return {WindowStatusBar::tr("GPU resident"), kit::Color::Ok,
                        WindowStatusBar::tr("GPU")};
            }
            return {WindowStatusBar::tr("Color state unavailable"), kit::Color::Warn,
                    WindowStatusBar::tr("None")};
        }
        // GPU provenance is reported honestly on the existing color chip: a display-only frame the
        // fixed GPU Neutral operation produced says so, and the qualification report it carries is
        // PreviewOnly (RGB tolerance), so this never claims reference parity. CPU reference and
        // qualified-CPU frames keep their exact existing text.
        if (provider == runtime::PreviewDisplayProvider::GpuNeutral) {
            return {WindowStatusBar::tr("GPU display"), kit::Color::Ok, WindowStatusBar::tr("GPU")};
        }
        if (provider == runtime::PreviewDisplayProvider::GpuPreparedPacked) {
            return {WindowStatusBar::tr("GPU-prepared / CPU-painted"), kit::Color::Ok,
                    WindowStatusBar::tr("GPU")};
        }
    }
    if (bufferView.has_value() && bufferView->isOcioQualified) {
        return {WindowStatusBar::tr("Qualified · Bloom Neutral"), kit::Color::Ok,
                WindowStatusBar::tr("CPU")};
    }
    return {preview.frame ? WindowStatusBar::tr("Reference (unqualified)")
                          : WindowStatusBar::tr("No displayed frame"),
            kit::Color::Warn,
            preview.frame ? WindowStatusBar::tr("CPU") : WindowStatusBar::tr("None")};
}

QString previewActivityText(const CompositionPreviewState& preview) {
    if (preview.viewerDisplayState && !preview.viewerStatusText.isEmpty())
        return preview.viewerStatusText;
    if (preview.viewerDisplayState && !preview.message.isEmpty())
        return preview.message;
    if (!preview.message.isEmpty() && (preview.activity == PreviewActivity::Failed ||
                                       preview.freshness != FrameFreshness::Current))
        return preview.message;
    const bool stale = preview.freshness == FrameFreshness::Stale;
    switch (preview.activity) {
    case PreviewActivity::Rendering:
        return stale ? WindowStatusBar::tr("Rendering current frame · Previous frame shown")
                     : WindowStatusBar::tr("Rendering current frame");
    case PreviewActivity::Ready:
        if (!preview.diagnostics.empty() && !preview.diagnostics.front().summary.empty())
            return QString::fromStdString(preview.diagnostics.front().summary);
        return WindowStatusBar::tr("Current frame ready");
    case PreviewActivity::Unsupported:
        return stale ? WindowStatusBar::tr("Preview unsupported · Previous frame shown")
                     : WindowStatusBar::tr("Preview unsupported");
    case PreviewActivity::Cancelled:
        return stale ? WindowStatusBar::tr("Preview cancelled · Previous frame shown")
                     : WindowStatusBar::tr("Preview cancelled");
    case PreviewActivity::Failed:
        return stale ? WindowStatusBar::tr("Preview failed · Previous frame shown")
                     : WindowStatusBar::tr("Preview failed");
    }
    return WindowStatusBar::tr("Preview unavailable");
}

kit::Color previewActivityColor(const CompositionPreviewState& preview) {
    switch (preview.activity) {
    case PreviewActivity::Rendering:
        return kit::Color::Accent;
    case PreviewActivity::Ready:
        return kit::Color::Ok;
    case PreviewActivity::Unsupported:
        return kit::Color::Warn;
    case PreviewActivity::Cancelled:
        return kit::Color::Muted;
    case PreviewActivity::Failed:
        return kit::Color::Error;
    }
    return kit::Color::Muted;
}

QString droppedFrameText(const CompositionPreviewController& previewController) {
    if (!previewController.isCountingDroppedFrames()) {
        return {};
    }
    return WindowStatusBar::tr("%1 dropped").arg(previewController.droppedFrameCount());
}

QString previewCacheText(const CompositionPreviewController& previewController) {
    const auto& progress = previewController.ramPreviewProgress();
    if (progress.has_value()) {
        return WindowStatusBar::tr("Caching %1/%2")
            .arg(progress->cachedFrames)
            .arg(progress->totalFrames);
    }
    // No run is caching. What is left to report is what the cache actually holds -- which is also
    // the only honest account of BACKGROUND cache fill available: BackgroundPreviewController
    // publishes no progress signal, so a "filling in the background" claim would be invented.
    const auto& cache = previewController.frameCache();
    if (cache.size() == 0) {
        return {};
    }
    return WindowStatusBar::tr("Cache %1 frames · %2 / %3")
        .arg(cache.size())
        .arg(formatBytes(cache.residentBytes()), formatBytes(cache.byteBudget()));
}

QString operationCacheText(const runtime::OperationCache& operationCache) {
    const auto statistics = operationCache.statistics();
    if (statistics.hits == 0 && statistics.misses == 0 && operationCache.retainedBytes() == 0)
        return {};
    return WindowStatusBar::tr("Operation cache: %1 hits · %2 misses · %3 / %4")
        .arg(statistics.hits)
        .arg(statistics.misses)
        .arg(formatBytes(operationCache.retainedBytes()), formatBytes(operationCache.byteBudget()));
}

// CACHE-2 (docs/architecture/media-io.md "Disk cache"): kept as its own free function, separate
// hunk and separate cell from previewCacheText() above -- the RAM preview cell reports the
// in-memory frame cache, this one the on-disk decoded-frame store, and the two are independent
// budgets an artist can reason about separately.
QString mediaDiskCacheStatusText(const media::cache::MediaDiskCacheStatistics& stats,
                                 const bool enabled) {
    if (!enabled) {
        return WindowStatusBar::tr("Disk cache off");
    }
    const auto total = stats.hits + stats.misses;
    if (total == 0 && stats.entryCount == 0) {
        return {};
    }
    return WindowStatusBar::tr("Disk %1% hit · %2")
        .arg(static_cast<int>(std::lround(stats.hitRate() * 100.0)))
        .arg(formatBytes(stats.storedBytes));
}

} // namespace bloom::ui
