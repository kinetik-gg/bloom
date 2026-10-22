#include "asset_thumbnail_display.hpp"
#include "network_share_paths.hpp"
#include <QDir>
#include <QFileDialog>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <algorithm>
#include <bloom/color/bloom_neutral_builtin.hpp>
#include <bloom/color/ocio_cpu_display_processor.hpp>
#include <bloom/color/ocio_cpu_file_transform_processor.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/media/audio/audio.hpp>
#include <bloom/media/cache/media_disk_cache_decode.hpp>
#include <bloom/media/image.hpp>
#include <bloom/media/video/audio.hpp>
#include <bloom/media/video/colour.hpp>
#include <bloom/media/video/session.hpp>
#include <bloom/platform/font_catalog.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/input_color_context.hpp>
#include <bloom/runtime/memory_budget_ledger.hpp>
#include <bloom/runtime/value_graph_evaluation.hpp>
#include <bloom/runtime/video_asset.hpp>
#include <bloom/ui/asset_controller.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/project_host.hpp>
#include <bloom/ui/task_ui_bridge.hpp>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <span>

namespace bloom::ui {
namespace {
// CACHEFIX-1 proxy/audio caps. The proxy cache keeps its historic 512-entry bound and adds the
// byte bound that bound only ever implied; the audio cache gets the aggregate bound it never had.
constexpr std::size_t kProxyCacheEntryLimit = 512;
constexpr std::size_t kProxyCacheByteCapacity = std::size_t{8} * 1024U * 1024U;
constexpr std::size_t kDecodedAudioByteCapacity = std::size_t{2} * 1024U * 1024U * 1024U;

[[nodiscard]] std::size_t proxyByteCost(const QImage& image) {
    // sizeInBytes() is the allocation QImage actually holds, stride padding included.
    return image.isNull() ? 0 : static_cast<std::size_t>(image.sizeInBytes());
}

[[nodiscard]] std::size_t audioBufferByteCost(const media::audio::AudioBuffer& buffer) {
    std::size_t bytes = sizeof(media::audio::AudioBuffer);
    for (const auto& plane : buffer.planes)
        bytes += plane.capacity() * sizeof(float);
    return bytes;
}

// Prune aliasing preview images too: erasing a map entry alone does not release its pixels.
template <typename Cache, typename Assets, typename Nodes>
std::size_t trimProxyStorage(Cache& cache, Assets& assets, Nodes& nodes, const std::size_t limit) {
    std::size_t bytes = 0;
    for (const auto& [key, image] : cache)
        bytes += proxyByteCost(image);
    while (!cache.empty() && (bytes > limit || cache.size() > kProxyCacheEntryLimit)) {
        bytes -= proxyByteCost(cache.begin()->second);
        cache.erase(cache.begin());
    }
    const auto trimAliases = [&cache](auto& previews) {
        for (auto& [id, preview] : previews)
            if (!cache.contains(preview.key))
                preview.image = {};
    };
    trimAliases(assets);
    trimAliases(nodes);
    return bytes;
}

template <typename Buffers>
std::size_t trimAudioStorage(Buffers& buffers, const std::size_t limit) {
    std::size_t bytes = 0;
    for (const auto& [id, buffer] : buffers)
        bytes += audioBufferByteCost(*buffer);
    while (!buffers.empty() && bytes > limit) {
        bytes -= audioBufferByteCost(*buffers.begin()->second);
        buffers.erase(buffers.begin());
    }
    return bytes;
}

std::filesystem::path nativePath(const QString& text) {
#if defined(_WIN32)
    return std::filesystem::path(text.toStdWString());
#else
    return std::filesystem::path(text.toStdString());
#endif
}
struct ThumbnailSelection {
    std::filesystem::path path;
    core::Sha256Digest digest;
    media::ImageInterpretation interpretation;
    std::shared_ptr<const color::CpuColorSpaceProcessor> inputProcessor;
    std::string inputColorSpaceName;
    std::string inputColorSpaceWarning;
    std::string workingColorSpaceId;
    core::Sha256Digest configRevision;
    bool inputColorSpaceAutomatic = false;
    std::string cacheKey;
    // The same disk-cache key format image_source.cpp's selectImageSource() builds (see
    // media::cache::buildImageCacheKey()): asset digest + member frame + interpretation + input/
    // working colour-space ids + config revision + decoder identity, so a proxy decode here and a
    // full evaluator decode of the same source share one disk entry.
    std::string diskCacheKey;
    bool available = false;
};

[[nodiscard]] bool fontFileMatches(const document::AssetRecord& asset) {
    if (asset.locator.portability == "builtin")
        return true;
    constexpr std::uintmax_t kMaximumFontBytes = static_cast<std::uintmax_t>(64) * 1024U * 1024U;
    std::error_code error;
    const auto size = std::filesystem::file_size(asset.locator.path, error);
    if (error || size == 0 || size > kMaximumFontBytes)
        return false;
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    std::ifstream input(asset.locator.path, std::ios::binary);
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input)
        return false;
    const auto digest = core::Sha256Hasher::hash(std::as_bytes(std::span(bytes)));
    return digest.has_value() && *digest == asset.contentDigest;
}

[[nodiscard]] document::AssetRecord fontAssetRecord(const platform::FontFace& face) {
    document::AssetRecord asset;
    asset.kind = document::AssetKind::Font;
    asset.contentDigest = face.contentDigest;
    asset.fontFamily = face.family;
    asset.fontStyle = face.style;
    asset.fontIndex = face.faceIndex;
    if (face.source == platform::FontSource::Embedded) {
        asset.locator = {"font", "builtin", "embedded/" + std::to_string(face.faceIndex),
                         "font:embedded:" + face.family + "|" + face.style};
    } else {
        const auto absolute = std::filesystem::absolute(face.path).lexically_normal();
        const auto path = absolute.generic_string();
        asset.locator = {
            "font", "system", path,
            QUrl::fromLocalFile(QString::fromStdString(path)).toEncoded().toStdString()};
    }
    return asset;
}
// UI-only frame projection of the Image source contract. Use the public exact frame mapping;
// runtime implementation headers are deliberately outside this module's boundary.
ThumbnailSelection selectThumbnail(const runtime::CompiledImageSource& source,
                                   core::RationalTime time, document::FrameRate rate,
                                   const std::filesystem::path& directory,
                                   const runtime::CancellationToken& cancel,
                                   const runtime::EvaluationColorIntent& colorIntent) {
    ThumbnailSelection selected;
    if (!source.asset)
        return selected;
    const auto& asset = *source.asset;
    const auto* locator = &asset.locator;
    selected.digest = asset.contentDigest;
    std::int64_t memberFrame = 0;
    if (asset.kind == document::AssetKind::Sequence) {
        const auto frame = runtime::valueGraphFrameIndex(time, rate);
        if (!frame || asset.manifest.members.empty())
            return selected;
        const auto elapsed =
            static_cast<long double>(*frame) - static_cast<long double>(source.startFrame);
        if (elapsed < static_cast<long double>(std::numeric_limits<std::int64_t>::min()) ||
            elapsed > static_cast<long double>(std::numeric_limits<std::int64_t>::max()))
            return selected;
        const auto length = static_cast<std::uint64_t>(asset.manifest.last) -
                            static_cast<std::uint64_t>(asset.manifest.first) + 1;
        const auto signedOffset = static_cast<std::int64_t>(elapsed);
        std::uint64_t offset = signedOffset < 0 ? 0 : static_cast<std::uint64_t>(signedOffset);
        else if (source.loopMode == 1)
            offset %= length;
        else if (source.loopMode == 2 && length > 1) {
            const auto period = 2 * (length - 1);
            offset %= period;
            if (offset >= length)
                offset = period - offset;
        } else
            offset = std::min(offset, length - 1);
        const auto wanted =
            static_cast<std::int64_t>(static_cast<std::uint64_t>(asset.manifest.first) + offset);
        auto found = std::ranges::upper_bound(asset.manifest.members, wanted, {},
                                              &document::AssetSequenceMember::frame);
        if (found != asset.manifest.members.begin())
            --found;
        locator = &found->locator;
        selected.digest = found->contentDigest;
        memberFrame = found->frame;
    }
    selected.path = media::resolveImagePath(locator->path, locator->relinkHint, directory);
    const auto input = document::effectiveAssetInputColor(asset.interpretation, source.colorSpace,
                                                          source.inputColorSpaceId);
    selected.interpretation.colorSpace = static_cast<media::ImageColorSpace>(input.colorSpace);
    selected.interpretation.inputColorSpaceId = input.inputColorSpaceId;
    selected.interpretation.alphaAssociation = source.premultiply
                                                   ? media::ImageAlphaAssociation::Straight
                                                   : media::ImageAlphaAssociation::Premultiplied;
    const auto probe =
        media::probeImage(selected.path, [&] { return cancel.isCancellationRequested(); });
    selected.available = probe.value && probe.value->contentDigest == selected.digest;
    if (selected.available) {
        const auto config = runtime::detail::resolveInputColorConfig(colorIntent);
        if (!config) {
            selected.available = false;
            selected.inputColorSpaceWarning =
                "The selected OCIO input configuration is unavailable";
        } else {
            const auto resolution = runtime::detail::resolveImageInputColorSpace(
                *config, *probe.value, selected.interpretation.colorSpace,
                selected.interpretation.inputColorSpaceId);
            selected.inputColorSpaceAutomatic = resolution.automatic;
            selected.inputColorSpaceName = resolution.name;
            selected.inputColorSpaceWarning = resolution.warning;
            selected.workingColorSpaceId = std::string(config->processColorSpaceId());
            selected.configRevision = config->expectedRevision();
            if (!resolution.noConversion && resolution.id.empty())
                selected.available = false;
            if (selected.available) {
                selected.interpretation.inputColorSpaceId = resolution.id;
                std::string diagnostic;
                selected.inputProcessor =
                    runtime::detail::prepareInputColorProcessor(*config, resolution, diagnostic);
                if (!resolution.noConversion && !selected.inputProcessor) {
                    selected.available = false;
                    selected.inputColorSpaceWarning = std::move(diagnostic);
                }
            }
        }
    }
    const auto hex = selected.digest.toLowercaseHex();
    const auto config = selected.configRevision.toLowercaseHex();
    selected.cacheKey = std::string(hex.begin(), hex.end()) + ":" + std::to_string(memberFrame) +
                        ":" + std::to_string(static_cast<int>(selected.interpretation.colorSpace)) +
                        ":" + selected.interpretation.inputColorSpaceId + ":" +
                        selected.workingColorSpaceId + ":" +
                        std::to_string(static_cast<int>(selected.interpretation.alphaAssociation)) +
                        ":" + std::string(config.begin(), config.end());
    media::cache::ImageCacheKeyInputs diskInputs;
    diskInputs.contentDigest = selected.digest;
    diskInputs.memberFrame = memberFrame;
    diskInputs.colorSpace = selected.interpretation.colorSpace;
    diskInputs.inputColorSpaceId = selected.interpretation.inputColorSpaceId;
    diskInputs.workingColorSpaceId = selected.workingColorSpaceId;
    diskInputs.alphaAssociation = selected.interpretation.alphaAssociation;
    diskInputs.configDigest = selected.configRevision;
    selected.diskCacheKey =
        media::cache::buildImageCacheKey(diskInputs, media::cache::kImageDecoderIdentity);
    return selected;
}
} // namespace
AssetController::AssetController(CompositionSession& session, ProjectHost& host,
                                 runtime::TaskScheduler& scheduler, TaskUiBridge& bridge,
                                 media::cache::MediaDiskCache* mediaDiskCache, QObject* parent)
    : QObject(parent), session_(session), host_(host), scheduler_(scheduler), bridge_(bridge),
      mediaDiskCache_(mediaDiskCache) {
    // Source freshness is independent of the playhead frequency. A burst of time changes
    // requests one delayed check, without cancelling active work or rebuilding unchanged rows.
    freshnessTimer_.setSingleShot(true);
    freshnessTimer_.setInterval(1000);
    connect(&freshnessTimer_, &QTimer::timeout, this, [this] {
        assetsDirty_ = true;
        previewDirty_ = true;
        if (!previewPending_)
            startThumbnails();
    });
    auto& ledger = runtime::processMemoryBudgetLedger();
    ledger.registerCache(
        &proxyCacheBytes_, kProxyCacheByteCapacity, [this] { return proxyCacheBytes_; },
        [this, previous = kProxyCacheByteCapacity](const std::size_t bytes) mutable {
            if (bytes < previous && previewPending_) {
                previewDirty_ = true;
                assetsDirty_ = assetsDirty_ || previewIncludesAssets_;
                preview_.cancel();
            }
            previous = bytes;
            const auto before = proxyCacheBytes_;
            proxyCacheBytes_ = trimProxyStorage(thumbnailCache_, previews_, nodePreviews_, bytes);
            if (proxyCacheBytes_ != before)
                QTimer::singleShot(0, this, &AssetController::changed);
        });
    try {
        ledger.registerCache(
            &decodedAudioBytes_, kDecodedAudioByteCapacity, [this] { return decodedAudioBytes_; },
            [this, previous = kDecodedAudioByteCapacity](const std::size_t bytes) mutable {
                if (bytes < previous && previewPending_) {
                    previewDirty_ = true;
                    assetsDirty_ = assetsDirty_ || previewIncludesAssets_;
                    preview_.cancel();
                }
                previous = bytes;
                const auto before = decodedAudioBytes_;
                decodedAudioBytes_ = trimAudioStorage(audioBuffers_, bytes);
                if (decodedAudioBytes_ != before)
                    QTimer::singleShot(0, this, &AssetController::changed);
            });
    } catch (...) {
        ledger.unregisterCache(&proxyCacheBytes_);
        throw;
    }
    dragToken_ = QUuid::createUuid().toByteArray();
    session_.setAssetController(this);
    connect(&bridge_, &TaskUiBridge::resultsAvailable, this, &AssetController::poll);
    connect(&session_, &CompositionSession::snapshotChanged, this, &AssetController::refresh);
    connect(&session_, &CompositionSession::currentTimeChanged, this,
            &AssetController::refreshNodeThumbnails);
    connect(&session_, &CompositionSession::compositionChanged, this, &AssetController::refresh);
    connect(&host_, &ProjectHost::sessionReplaced, this, [this] {
        dragToken_ = QUuid::createUuid().toByteArray();
        cancel();
        base_.reset();
        previews_.clear();
        nodePreviews_.clear();
        thumbnailCache_.clear();
        waveforms_.clear();
        audioBuffers_.clear();
        proxyCacheBytes_ = 0;
        decodedAudioBytes_ = 0;
        refresh();
    });
    connect(&host_, &ProjectHost::colorSettingsChanged, this, [this] {
        const bool requested = inputColorCatalogPending_ || inputColorCatalog_ != nullptr;
        if (inputColorCatalogPending_) {
            inputColorCatalogTask_.cancel();
            inputColorCatalogPending_ = false;
            inputColorCatalogRequestedSettings_.reset();
            inputColorCatalogRequestedWorking_.clear();
        }
        inputColorCatalog_.reset();
        if (requested)
            requestInputColorSpaces();
        emit changed();
    });
    connect(this, &AssetController::diagnostic, &session_, &CompositionSession::commandRejected);
    refresh();
}
AssetController::~AssetController() {
    runtime::processMemoryBudgetLedger().unregisterCache(&proxyCacheBytes_);
    runtime::processMemoryBudgetLedger().unregisterCache(&decodedAudioBytes_);
    cancel();
    session_.setAssetController(nullptr);
}
bool AssetController::acceptsEdits() const { return host_.canSave(); }
void AssetController::cancel() {
    freshnessTimer_.stop();
    previewDirty_ = false;
    if (busy_)
        import_.cancel();
    if (previewPending_)
        preview_.cancel();
    if (inputColorCatalogPending_) {
        inputColorCatalogTask_.cancel();
        inputColorCatalogPending_ = false;
        inputColorCatalogRequestedSettings_.reset();
        inputColorCatalogRequestedWorking_.clear();
    }
}
void AssetController::setCachePurgeGate(const bool gated) {
    if (cachePurgeGate_ == gated)
        return;
    cachePurgeGate_ = gated;
    if (gated) {
        freshnessTimer_.stop();
        ++contentGeneration_;
        assetsDirty_ = true;
        previewDirty_ = true;
        if (previewPending_)
            preview_.cancel();
    } else if (previewDirty_ && !previewPending_) {
        startThumbnails();
    }
}
const std::vector<color::OcioColorSpaceInfo>* AssetController::inputColorSpaces() const noexcept {
    return inputColorCatalog_ && inputColorCatalog_->diagnostic.isEmpty()
               ? &inputColorCatalog_->spaces
               : nullptr;
}
QString AssetController::inputColorCatalogDiagnostic() const {
    return inputColorCatalog_ ? inputColorCatalog_->diagnostic : QString{};
}
void AssetController::requestInputColorSpaces() {
    const auto* current = host_.colorSettings();
    if (!current)
        return;
    const auto settings = *current;
    const std::string working(session_.colorIntent().workingColorSpaceId);
    if (inputColorCatalog_ && inputColorCatalog_->diagnostic.isEmpty() &&
        inputColorCatalog_->settings == settings &&
        inputColorCatalog_->workingColorSpaceId == working)
        return;
    if (inputColorCatalogPending_ && inputColorCatalogRequestedSettings_ == settings &&
        inputColorCatalogRequestedWorking_ == working)
        return;
    if (inputColorCatalogPending_) {
        inputColorCatalogTask_.cancel();
        inputColorCatalogPending_ = false;
        inputColorCatalogRequestedSettings_.reset();
        inputColorCatalogRequestedWorking_.clear();
    }
    inputColorCatalog_.reset();
    auto submission = scheduler_.submit<std::shared_ptr<InputColorCatalog>>(
        runtime::TaskRequest(
            "Prepare input colour choices",
            {.kind = runtime::TaskOwnerKind::Application, .id = runtime::TaskOwnerId::fromRaw(1)},
            runtime::TaskPriority::Foreground, runtime::TaskExecutor::Cpu),
        [settings, working](runtime::TaskContext& context) {
            if (context.isCancellationRequested())
                return runtime::TaskResult<std::shared_ptr<InputColorCatalog>>::cancelled();
            auto result = std::make_shared<InputColorCatalog>();
            result->settings = settings;
            result->workingColorSpaceId = working;
            const auto* builtIn =
                std::get_if<document::BuiltInOcioConfigLocator>(&settings.ocioConfig.locator);
            if (!builtIn || settings.ocioConfig.expectedRevision.algorithm !=
                                document::OcioRevisionAlgorithm::Sha256) {
                result->diagnostic = QObject::tr("This OCIO configuration is not available in the "
                                                 "current runtime.");
            } else {
                auto resolved = color::resolveOcioBuiltIn(
                    color::OcioConfigLocatorKind::BloomBuiltIn, builtIn->uri,
                    settings.ocioConfig.expectedRevision.digest, working);
                if (resolved.ready()) {
                    auto config = std::move(resolved).takeResolved();
                    if (config)
                        result->spaces = config->colorSpaces();
                    else
                        result->diagnostic = QObject::tr("The selected OCIO configuration could "
                                                         "not be resolved.");
                } else
                    result->diagnostic = QObject::tr("The selected OCIO configuration could not "
                                                     "be resolved.");
            }
            if (context.isCancellationRequested())
                return runtime::TaskResult<std::shared_ptr<InputColorCatalog>>::cancelled();
            return runtime::TaskResult<std::shared_ptr<InputColorCatalog>>::succeeded(
                std::move(result));
        });
    if (submission.accepted()) {
        inputColorCatalogTask_ = std::move(submission.handle);
        inputColorCatalogPending_ = true;
        inputColorCatalogRequestedSettings_ = settings;
        inputColorCatalogRequestedWorking_ = working;
        bridge_.wake();
    } else {
        inputColorCatalog_ = std::make_shared<InputColorCatalog>();
        inputColorCatalog_->settings = settings;
        inputColorCatalog_->workingColorSpaceId = working;
        inputColorCatalog_->diagnostic = tr("Colour choices are unavailable while processing "
                                            "capacity is full.");
        emit changed();
    }
}
std::filesystem::path AssetController::baseDirectory() const {
    const auto path = host_.displayPath();
    return path ? path->parent_path() : nativePath(QDir::currentPath());
}
bool AssetController::missing(document::AssetId id) const {
    const auto found = previews_.find(id);
    return found != previews_.end() && found->second.missing;
}
QString AssetController::audioDiagnostic(document::AssetId id) const {
    const auto found = previews_.find(id);
    return found == previews_.end() ? QString{}
                                    : QString::fromStdString(found->second.audioDiagnostic);
}
QImage AssetController::thumbnail(document::AssetId id) const {
    const auto found = previews_.find(id);
    return found == previews_.end() ? QImage{} : found->second.image;
}
QImage AssetController::nodeThumbnail(document::NodeId id) const {
    const auto found = nodePreviews_.find(id);
    return found == nodePreviews_.end() ? QImage{} : found->second.image;
}
QString AssetController::inputColorSpaceDisplay(const document::AssetId id) const {
    const auto found = previews_.find(id);
    if (found != previews_.end() && !found->second.inputColorSpaceName.empty()) {
        const auto name = QString::fromStdString(found->second.inputColorSpaceName);
        return found->second.inputColorSpaceAutomatic ? tr("Auto (resolved: %1)").arg(name) : name;
    }
    const auto* asset = session_.snapshot().project().findAsset(id);
    if (!asset)
        return {};
    if (!asset->interpretation.inputColorSpaceId.empty())
        return QString::fromStdString(asset->interpretation.inputColorSpaceId);
    switch (asset->interpretation.colorSpace) {
    case document::AssetColorSpace::Raw:
        return tr("Raw data (no colour conversion)");
    case document::AssetColorSpace::Srgb:
        return tr("Legacy sRGB");
    case document::AssetColorSpace::Linear:
        return tr("Legacy Linear");
    case document::AssetColorSpace::Auto:
        return tr("Auto (from media tags)");
    }
    return {};
}
QString AssetController::inputColorSpaceWarning(const document::AssetId id) const {
    const auto found = previews_.find(id);
    return found == previews_.end() ? QString{}
                                    : QString::fromStdString(found->second.inputColorSpaceWarning);
}
std::shared_ptr<const media::audio::WaveformSummary>
AssetController::waveform(const document::AssetId id) const {
    const auto found = waveforms_.find(id);
    return found == waveforms_.end() ? nullptr : found->second;
}
std::shared_ptr<const media::audio::AudioBuffer>
AssetController::audioBuffer(const document::AssetId id) const {
    const auto found = audioBuffers_.find(id);
    return found == audioBuffers_.end() ? nullptr : found->second;
}
void AssetController::requestImport(QWidget* parent) {
    if (!host_.canSave())
        return;
    // A real QFileDialog instance rather than the static getOpenFileNames() convenience: it is
    // still native when the platform theme provides one (unchanged behaviour), but constructing
    // it lets configureFileDialogSidebar() add the mounted network shares before exec(), which
    // the static function gives no opportunity to do.
    QFileDialog dialog(
        parent, tr("Import Media"), {},
        tr("Media (*.cube *.clf *.spi1d *.spi3d *.png *.jpg *.jpeg *.exr *.tif *.tiff *.wav *.mp3 "
           "*.mp4 *.mov *.mkv *.mxf "
           "*.m4v *.mts *.m2ts *.ts *.PNG *.JPG *.JPEG *.EXR *.TIF *.TIFF *.WAV *.MP3 "
           "*.MP4 *.MOV *.MKV *.MXF)"));
    dialog.setAcceptMode(QFileDialog::AcceptOpen);
    dialog.setFileMode(QFileDialog::ExistingFiles);
    configureFileDialogSidebar(dialog);
    if (dialog.exec() == QDialog::Accepted) {
        const auto paths = dialog.selectedFiles();
        if (!paths.empty())
            importFiles(paths);
    }
}
void AssetController::importFiles(const QStringList& paths) { prepare(paths); }
void AssetController::relink(document::AssetId id, QWidget* parent) {
    if (!host_.canSave())
        return;
    const auto* asset = session_.snapshot().project().findAsset(id);
    const bool font = asset != nullptr && asset->kind == document::AssetKind::Font;
    QFileDialog dialog(
        parent, font ? tr("Relink Font") : tr("Relink Media"), {},
        font ? tr("Fonts (*.ttf *.otf *.ttc *.TTF *.OTF *.TTC)")
             : tr("Media (*.cube *.clf *.spi1d *.spi3d *.png *.jpg *.jpeg *.exr *.tif *.tiff *.wav "
                  "*.mp3 *.mp4 *.mov *.mkv *.mxf "
                  "*.m4v *.mts *.m2ts *.ts *.PNG *.JPG *.JPEG *.EXR *.TIF *.TIFF *.WAV *.MP3 "
                  "*.MP4 *.MOV *.MKV *.MXF)"));
    dialog.setAcceptMode(QFileDialog::AcceptOpen);
    dialog.setFileMode(QFileDialog::ExistingFile);
    configureFileDialogSidebar(dialog);
    if (dialog.exec() == QDialog::Accepted && !dialog.selectedFiles().isEmpty())
        prepare({dialog.selectedFiles().front()}, id);
}
void AssetController::remove(document::AssetId id) {
    if (!host_.canSave())
        return;
    commands::Transaction transaction("Remove Asset", session_.snapshot().revision());
    transaction.emplace<commands::RemoveAsset>(id);
    static_cast<void>(session_.executeTransaction(std::move(transaction)));
}
void AssetController::prepare(const QStringList& paths, document::AssetId relinkId) {
    if (!host_.canSave()) {
        emit diagnostic(tr("Media import requires an idle, editable project"));
        return;
    }
    if (busy_ || paths.empty())
        return;
    std::vector<std::filesystem::path> files;
    for (const auto& path : paths)
        files.push_back(nativePath(path));
    const auto directory = baseDirectory();
    base_ = session_.snapshot();
    const bool relinkFont = relinkId.isValid() && base_->project().findAsset(relinkId) != nullptr &&
                            base_->project().findAsset(relinkId)->kind == document::AssetKind::Font;
    auto submission = scheduler_.submit<OperationHandle>(
        runtime::TaskRequest(
            "Import media",
            {.kind = runtime::TaskOwnerKind::Application, .id = runtime::TaskOwnerId::fromRaw(1)},
            runtime::TaskPriority::Foreground, runtime::TaskExecutor::BlockingIo),
        [files = std::move(files), directory, relinkId, relinkFont](runtime::TaskContext& context) {
            auto result = std::make_shared<std::unique_ptr<commands::Operation>>();
            auto cancel = [&] { return context.isCancellationRequested(); };
            auto progress = [&](std::uint64_t done, std::uint64_t total) {
                context.reportProgress({.phase = "Importing media",
                                        .subphase = "Probing and scanning",
                                        .completed = done,
                                        .total = total});
            };
            if (relinkFont) {
                const auto catalogue = platform::FontCatalogueProvider{}.enumerate(cancel);
                const auto found = std::ranges::find_if(
                    catalogue.faces, [&](const auto& face) { return face.path == files.front(); });
                if (found == catalogue.faces.end())
                    return runtime::TaskResult<OperationHandle>::failed(
                        {.code = "bloom.font.relink-not-found",
                         .severity = runtime::DiagnosticSeverity::Error,
                         .summary = "The selected file is not in the system font catalogue",
                         .detail = "Choose an installed font face file.",
                         .suggestedAction = "Select a .ttf, .otf, or .ttc file reported by the "
                                            "system catalogue."});
                *result =
                    std::make_unique<commands::RelinkFontAsset>(relinkId, fontAssetRecord(*found));
            } else if (relinkId.isValid())
                *result = std::make_unique<commands::RelinkAsset>(relinkId, files.front(),
                                                                  directory, cancel);
            else {
                auto prepared =
                    std::make_unique<commands::ImportAssets>(files, directory, cancel, progress);
                if (context.isCancellationRequested())
                    return runtime::TaskResult<OperationHandle>::cancelled();
                if (!prepared->diagnostic().empty())
                    return runtime::TaskResult<OperationHandle>::failed(
                        {.code = "bloom.media.import-failed",
                         .severity = runtime::DiagnosticSeverity::Error,
                         .summary = prepared->diagnostic(),
                         .detail = prepared->diagnostic(),
                         .suggestedAction = "Check the file and its reported limit."});
                *result = std::move(prepared);
            }
            if (context.isCancellationRequested())
                return runtime::TaskResult<OperationHandle>::cancelled();
            return runtime::TaskResult<OperationHandle>::succeeded(std::move(result));
        });
    if (!submission.accepted()) {
        emit diagnostic(tr("Media import could not be scheduled"));
        return;
    }
    import_ = std::move(submission.handle);
    busy_ = true;
    bridge_.wake();
    emit activityChanged();
}
void AssetController::refresh() {
    // The picker requests its catalogue lazily. Once requested, a composition working-space edit
    // must retire the old choices even if the previous worker is still running; unrelated document
    // revisions leave the ready catalogue and its worker alone.
    if (const auto* settings = host_.colorSettings()) {
        const std::string_view working = session_.colorIntent().workingColorSpaceId;
        const bool pendingChanged =
            inputColorCatalogPending_ && (inputColorCatalogRequestedSettings_ != *settings ||
                                          inputColorCatalogRequestedWorking_ != working);
        const bool cachedChanged =
            inputColorCatalog_ && (inputColorCatalog_->settings != *settings ||
                                   inputColorCatalog_->workingColorSpaceId != working);
        if (pendingChanged || cachedChanged) {
            requestInputColorSpaces();
            emit changed();
        }
    }
    freshnessTimer_.stop();
    ++contentGeneration_;
    assetsDirty_ = true;
    previewDirty_ = true;
    nodePreviews_.clear();
    emit nodeThumbnailsChanged();
    if (previewPending_) {
        preview_.cancel();
        return;
    }
    startThumbnails();
}

void AssetController::refreshNodeThumbnails() {
    if (!freshnessTimer_.isActive())
        freshnessTimer_.start();
    // Preserve the visible picture while one bounded job finishes. Time changes replace a single
    // newest request; they neither cancel useful asset preparation nor rebuild Assets rows.
    if (!timeDependentNodes_ && !assetsDirty_ && !previewPending_)
        return;
    previewDirty_ = true;
    if (!previewPending_)
        startThumbnails();
}

void AssetController::startThumbnails() {
    if (cachePurgeGate_)
        return;
    // A preserved-read-only install retires the document behind CompositionSession. Its hidden
    // projection must not be read until the application rebinds it to decoded content again.
    if (!host_.liveDocumentAndStack().first) {
        if (previewPending_)
            preview_.cancel();
        previewPending_ = false;
        previewDirty_ = false;
        return;
    }
    if (previewPending_)
        return;
    const bool includeAssets = assetsDirty_;
    const auto snapshot = session_.snapshot();
    const auto directory = baseDirectory();
    const auto time = session_.currentTime();
    const auto capturedColor = runtime::captureEvaluationColorIntent(session_.colorIntent());
    if (!capturedColor) {
        emit diagnostic(tr("Media thumbnail colour intent could not be captured"));
        return;
    }
    const auto& colorIntent = *capturedColor;
    const auto* currentComposition = session_.composition();
    const auto rate = currentComposition ? currentComposition->format().frameRate()
                                         : document::FrameRate::framesPerSecond24();
    runtime::TaskRequest request(
        "Media thumbnails",
        {.kind = runtime::TaskOwnerKind::Application, .id = runtime::TaskOwnerId::fromRaw(1)},
        runtime::TaskPriority::Background, runtime::TaskExecutor::BlockingIo);
    request.usesPurgeableCaches = true;
    auto submission = scheduler_.submit<std::shared_ptr<Thumbnails>>(
        std::move(request),
        [snapshot, directory, time, rate, colorIntent, includeAssets,
         compositionId = session_.compositionId(), cached = thumbnailCache_,
         cachedNodes = nodePreviews_, cachedAudio = includeAssets ? audioBuffers_ : AudioBuffers{},
         cachedWaveforms = includeAssets ? waveforms_ : Waveforms{},
         cachedPreviews = includeAssets ? previews_ : Previews{}, diskCache = mediaDiskCache_,
         proxyOwner = &proxyCacheBytes_,
         audioOwner = &decodedAudioBytes_](runtime::TaskContext& context) mutable {
            const auto* composition = snapshot.project().findComposition(compositionId);
            std::vector<runtime::CompiledImageSource> sources;
            if (composition)
                for (const auto& node : composition->graph().nodes()) {
                    if (context.isCancellationRequested())
                        return runtime::TaskResult<std::shared_ptr<Thumbnails>>::cancelled();
                    if (node.typeId != "bloom.image-source" && node.typeId != "bloom.video-source")
                        continue;
                    runtime::CompiledImageSource source;
                    source.sourceNodeId = node.id;
                    for (const auto& binding : node.parameters) {
                        const auto* parameter = composition->parameters().find(binding.parameterId);
                        const auto* constant =
                            parameter
                                ? std::get_if<document::ConstantValueSource>(&parameter->source)
                                : nullptr;
                        if (!constant)
                            continue;
                        if (binding.role == "asset") {
                            if (const auto* id = std::get_if<std::string>(&constant->value))
                                if (const auto* asset =
                                        snapshot.project().findAsset(document::AssetId::fromRaw(
                                            QString::fromStdString(*id).toULongLong())))
                                    source.asset = *asset;
                        } else if (const auto* integer =
                                       std::get_if<std::int64_t>(&constant->value)) {
                            if (binding.role == "startFrame")
                                source.startFrame = *integer;
                            if (binding.role == "loopMode")
                                source.loopMode = *integer;
                            if (binding.role == "colorSpace")
                                source.colorSpace = *integer;
                        } else if (binding.role == "inputColorSpaceId") {
                            if (const auto* value = std::get_if<std::string>(&constant->value))
                                source.inputColorSpaceId = *value;
                        } else if (binding.role == "premultiply") {
                            if (const auto* value = std::get_if<bool>(&constant->value))
                                source.premultiply = *value;
                        }
                    }
                    if (includeAssets ||
                        (source.asset && source.asset->kind == document::AssetKind::Sequence))
                        sources.push_back(std::move(source));
                }
            auto results = std::make_shared<Thumbnails>();
            std::shared_ptr<const color::PreparedCpuDisplayProcessorHandle> thumbnailDisplay;
            std::string thumbnailDisplayIdentity = "unavailable";
            if (auto config = runtime::detail::resolveInputColorConfig(colorIntent)) {
                auto built = color::buildBloomNeutralCpuDisplayProcessor(*config);
                auto handle = std::move(built).takeHandle();
                if (handle) {
                    const auto identity = detail::assetThumbnailDisplayCacheIdentity(*handle);
                    if (identity) {
                        thumbnailDisplayIdentity = *identity;
                        thumbnailDisplay =
                            std::make_shared<const color::PreparedCpuDisplayProcessorHandle>(
                                std::move(*handle));
                    }
                }
            }
            results->timeDependentNodes = std::ranges::any_of(sources, [](const auto& source) {
                return source.asset && source.asset->kind == document::AssetKind::Sequence;
            });
            results->nodes = std::move(cachedNodes);
            results->cache = std::move(cached);
            // The proxy cache carries over from the previous run, so its byte account has to carry
            // over with it: starting from zero here would let the map grow past the capacity by
            // whatever the previous run had already put in it.
            for (const auto& [key, image] : results->cache)
                results->proxyBytes += proxyByteCost(image);
            const auto proxyLimit = [proxyOwner] {
                return runtime::processMemoryBudgetLedger().cacheByteBudget(proxyOwner);
            };
            const auto audioLimit = [audioOwner] {
                return runtime::processMemoryBudgetLedger().cacheByteBudget(audioOwner);
            };
            const auto keepAudio =
                [&](const document::AssetId id,
                    const std::shared_ptr<const media::audio::AudioBuffer>& buffer) {
                    const auto cost = audioBufferByteCost(*buffer);
                    const auto limit = audioLimit();
                    if (cost <= limit - std::min(results->audioBytes, limit)) {
                        results->audioBuffers.emplace(id, buffer);
                        results->audioBytes += cost;
                    }
                };
            const auto keepProxy = [&](const std::string& key, const QImage& image) {
                const auto limit = proxyLimit();
                const auto cost = proxyByteCost(image);
                if (cost <= limit)
                    results->cache.insert_or_assign(key, image);
                results->proxyBytes =
                    trimProxyStorage(results->cache, results->assets, results->nodes, limit);
            };
            const auto decode = [&](const runtime::CompiledImageSource& source,
                                    const core::RationalTime sampleTime) {
                Preview preview;
                const auto selected = selectThumbnail(source, sampleTime, rate, directory,
                                                      context.cancellation(), colorIntent);
                preview.key = selected.cacheKey + ':' + thumbnailDisplayIdentity;
                preview.inputColorSpaceName = selected.inputColorSpaceName;
                preview.inputColorSpaceWarning = selected.inputColorSpaceWarning;
                preview.inputColorSpaceAutomatic = selected.inputColorSpaceAutomatic;
                preview.missing = !selected.available;
                if (preview.missing)
                    return preview;
                if (!thumbnailDisplay) {
                    preview.missing = true;
                    preview.inputColorSpaceWarning =
                        "The selected OCIO thumbnail display processor is unavailable";
                    return preview;
                }
                const auto found = results->cache.find(preview.key);
                if (found != results->cache.end()) {
                    preview.image = found->second;
                    return preview;
                }
                // The same disk cache the evaluator writes to (media-io.md "Disk cache"): this
                // whole lambda already runs on a BlockingIo worker thread, so a disk-cache write
                // here is synchronous (writeAsync = false) -- there is no evaluation thread to
                // avoid blocking.
                auto decoded = media::cache::decodeThroughDiskCache(
                    selected.path, selected.interpretation, selected.digest, selected.diskCacheKey,
                    diskCache, /*writeAsync=*/false,
                    [&] { return context.isCancellationRequested(); }, {},
                    media::kMaxImageStorageBytes, selected.inputProcessor);
                preview.missing = !decoded.value.has_value();
                if (decoded.value.has_value()) {
                    const auto& image = **decoded.value;
                    const auto extent = image.descriptor()->dataWindow().extent();
                    const auto scale =
                        std::min(1.0, 64.0 / std::max(extent.width(), extent.height()));
                    const auto width =
                        std::max(1, static_cast<int>(std::lround(extent.width() * scale)));
                    const auto height = std::max(
                        1, static_cast<int>(std::lround(static_cast<double>(extent.height()) *
                                                        width / extent.width())));
                    preview.image = QImage(width, std::min(64, height), QImage::Format_RGBA8888);
                    std::vector<render::Rgba32f> sourceRow;
                    sourceRow.reserve(static_cast<std::size_t>(width));
                    std::vector<float> scratch(static_cast<std::size_t>(width) * 4U);
                    std::vector<render::Rgba8> packed(static_cast<std::size_t>(width));
                    bool displayValid = true;
                    for (int y = 0; y < preview.image.height() && displayValid; ++y) {
                        sourceRow.clear();
                        for (int x = 0; x < width; ++x) {
                            const auto sx = static_cast<std::uint32_t>(x) * extent.width() /
                                            static_cast<std::uint32_t>(width);
                            const auto sy = static_cast<std::uint32_t>(y) * extent.height() /
                                            static_cast<std::uint32_t>(preview.image.height());
                            sourceRow.push_back(
                                image.pixels()[static_cast<std::size_t>(sy) * extent.width() + sx]);
                        }
                        displayValid =
                            detail::packAssetThumbnailRow(*thumbnailDisplay, std::span(sourceRow),
                                                          std::span(scratch), std::span(packed));
                        if (displayValid)
                            std::memcpy(preview.image.scanLine(y), packed.data(),
                                        packed.size() * sizeof(render::Rgba8));
                    }
                    if (!displayValid) {
                        preview.image = {};
                        preview.missing = true;
                        preview.inputColorSpaceWarning =
                            "The thumbnail display transform could not process this image";
                    }
                }
                if (!preview.image.isNull()) {
                    // CACHEFIX-1: the 512-entry bound was only ever a bound on COUNT, and the
                    // 8 MiB it claimed was an unmeasured inference from "at most 64 x 64 RGBA8".
                    // Charge the real bytes, keep both bounds, and make the figure readable.
                    keepProxy(preview.key, preview.image);
                }
                return preview;
            };
            std::uint64_t completed = 0;
            const auto progress = [&] {
                context.reportProgress(
                    {.phase = "Media thumbnails",
                     .subphase = "Decoding proxies",
                     .completed = ++completed,
                     .total = (includeAssets ? snapshot.project().assets().size() : 0) +
                              sources.size()});
            };
            if (includeAssets)
                for (const auto& asset : snapshot.project().assets()) {
                    if (context.isCancellationRequested())
                        return runtime::TaskResult<std::shared_ptr<Thumbnails>>::cancelled();
                    if (asset.kind == document::AssetKind::Video) {
                        Preview preview;
                        preview.missing = true;
                        const auto digest = asset.contentDigest.toLowercaseHex();
                        const auto path = media::resolveImagePath(
                            asset.locator.path, asset.locator.relinkHint, directory);
                        media::video::VideoDecodeSession video(path);
                        const auto probe = runtime::video::probeMetadata(asset);
                        const bool available = !video.verifySource(
                            asset.contentDigest, [&] { return context.isCancellationRequested(); });
                        const auto stream =
                            std::ranges::find(probe.streams, media::provider::MediaKind::Video,
                                              &media::provider::StreamDescriptor::kind);
                        std::shared_ptr<const color::CpuColorSpaceProcessor> inputProcessor;
                        std::string inputId;
                        std::string workingId;
                        core::Sha256Digest configRevision;
                        bool automatic = false;
                        bool colorAvailable = false;
                        if (stream != probe.streams.end()) {
                            const auto config =
                                runtime::detail::resolveInputColorConfig(colorIntent);
                            if (config) {
                                const media::provider::ColourTags tags{
                                    stream->colour.primaries, stream->colour.transfer,
                                    stream->colour.matrix, stream->colour.range};
                                const auto resolution = media::video::resolveVideoInputColorSpace(
                                    *config, tags,
                                    static_cast<std::uint32_t>(asset.interpretation.colorSpace),
                                    asset.interpretation.inputColorSpaceId);
                                inputId = resolution.id;
                                workingId = std::string(config->processColorSpaceId());
                                configRevision = config->expectedRevision();
                                automatic = resolution.automatic;
                                preview.inputColorSpaceName = resolution.name;
                                preview.inputColorSpaceWarning = resolution.warning;
                                if (resolution.noConversion) {
                                    colorAvailable = true;
                                } else if (!resolution.id.empty()) {
                                    std::string diagnostic;
                                    inputProcessor = runtime::detail::prepareInputColorProcessor(
                                        *config,
                                        runtime::detail::InputColorSpaceResolution{
                                            resolution.id, resolution.name, resolution.warning,
                                            resolution.noConversion, resolution.automatic},
                                        diagnostic);
                                    if (!resolution.noConversion && !inputProcessor) {
                                        preview.inputColorSpaceWarning = std::move(diagnostic);
                                        inputId.clear();
                                    } else
                                        colorAvailable = true;
                                }
                            } else {
                                preview.inputColorSpaceWarning =
                                    "The selected OCIO input configuration is unavailable";
                            }
                        }
                        preview.inputColorSpaceAutomatic = automatic;
                        const auto revision = configRevision.toLowercaseHex();
                        preview.key = std::string(digest.begin(), digest.end());
                        preview.key += ":video:";
                        preview.key +=
                            std::to_string(static_cast<unsigned>(asset.interpretation.colorSpace));
                        preview.key += ':';
                        preview.key += inputId;
                        preview.key += ':';
                        preview.key += workingId;
                        preview.key += ':';
                        preview.key.append(revision.begin(), revision.end());
                        preview.key += ':';
                        preview.key += thumbnailDisplayIdentity;
                        const auto cachedPreview = results->cache.find(preview.key);
                        if (available && colorAvailable && thumbnailDisplay &&
                            cachedPreview != results->cache.end()) {
                            preview.image = cachedPreview->second;
                            preview.missing = false;
                        }
                        if (available && colorAvailable && thumbnailDisplay &&
                            preview.image.isNull() && stream != probe.streams.end()) {
                            const auto frame = video.frame(
                                probe, stream->id, 0, 0, nullptr, inputId, workingId,
                                configRevision, [&] { return context.isCancellationRequested(); });
                            if (const auto* product = std::get_if<
                                    std::shared_ptr<const media::provider::FrameProduct>>(&frame)) {
                                const auto window =
                                    render::ImageWindow::create(0, 0, asset.width, asset.height);
                                if (window) {
                                    const auto descriptor = render::Rgba32fImageDescriptor::create(
                                        *window.value(), *window.value(),
                                        core::PixelAspectRatio::square());
                                    if (descriptor) {
                                        const auto scale =
                                            std::min(1.0, 64.0 / static_cast<double>(std::max(
                                                                     asset.width, asset.height)));
                                        const auto pixels = media::video::videoToSceneLinear(
                                            **product,
                                            static_cast<std::uint32_t>(
                                                asset.interpretation.colorSpace),
                                            inputId, inputProcessor, *descriptor.value(), scale,
                                            scale, 65536,
                                            [&] { return context.isCancellationRequested(); });
                                        if (const auto* image =
                                                std::get_if<render::Rgba32fImage>(&pixels)) {
                                            const auto extent =
                                                image->descriptor()->dataWindow().extent();
                                            preview.image =
                                                QImage(static_cast<int>(extent.width()),
                                                       static_cast<int>(extent.height()),
                                                       QImage::Format_RGBA8888);
                                            std::vector<float> scratch(
                                                static_cast<std::size_t>(extent.width()) * 4U);
                                            std::vector<render::Rgba8> packed(extent.width());
                                            bool displayValid = true;
                                            for (std::uint32_t y = 0;
                                                 y < extent.height() && displayValid; ++y) {
                                                const auto offset =
                                                    static_cast<std::size_t>(y) * extent.width();
                                                const std::span<const render::Rgba32f> row(
                                                    image->pixels().data() + offset,
                                                    extent.width());
                                                displayValid = detail::packAssetThumbnailRow(
                                                    *thumbnailDisplay, row, std::span(scratch),
                                                    std::span(packed));
                                                if (displayValid)
                                                    std::memcpy(
                                                        preview.image.scanLine(static_cast<int>(y)),
                                                        packed.data(),
                                                        packed.size() * sizeof(render::Rgba8));
                                            }
                                            if (displayValid)
                                                preview.missing = false;
                                            else {
                                                preview.image = {};
                                                preview.inputColorSpaceWarning =
                                                    "The thumbnail display transform could not "
                                                    "process this video frame";
                                            }
                                        }
                                    }
                                }
                            }
                        }
                        if (!thumbnailDisplay && preview.inputColorSpaceWarning.empty())
                            preview.inputColorSpaceWarning =
                                "The selected OCIO thumbnail display processor is unavailable";
                        if (!preview.image.isNull()) {
                            keepProxy(preview.key, preview.image);
                        }
                        const auto previous = cachedPreviews.find(asset.id);
                        const auto cachedBuffer = cachedAudio.find(asset.id);
                        const auto waveform = cachedWaveforms.find(asset.id);
                        if (available && previous != cachedPreviews.end() &&
                            previous->second.key == preview.key &&
                            cachedBuffer != cachedAudio.end() &&
                            waveform != cachedWaveforms.end()) {
                            keepAudio(asset.id, cachedBuffer->second);
                            results->waveforms.emplace(asset.id, waveform->second);
                        } else if (available && asset.channels > 0) {
                            auto decoded = media::video::decodeAudioClip(
                                video, probe, [&] { return context.isCancellationRequested(); });
                            if (auto* audio = std::get_if<media::audio::AudioBuffer>(&decoded)) {
                                const auto summary = media::audio::waveformSummary(*audio, 256);
                                if (summary.value())
                                    results->waveforms.emplace(
                                        asset.id,
                                        std::make_shared<const media::audio::WaveformSummary>(
                                            *summary.value()));
                                keepAudio(asset.id,
                                          std::make_shared<const media::audio::AudioBuffer>(
                                              std::move(*audio)));
                            }
                        }
                        results->assets.emplace(asset.id, std::move(preview));
                    } else if (asset.kind == document::AssetKind::Audio) {
                        Preview preview;
                        preview.missing = true;
                        const auto path = media::resolveImagePath(
                            asset.locator.path, asset.locator.relinkHint, directory);
                        auto decoded = media::audio::decodeAudio(path);
                        if (!decoded)
                            preview.audioDiagnostic =
                                media::audio::describeAudioError(*decoded.error());
                        if (decoded.value() != nullptr) {
                            const auto summary =
                                media::audio::waveformSummary(*decoded.value(), 256);
                            if (summary.value() != nullptr) {
                                preview.missing = false;
                                results->waveforms.emplace(
                                    asset.id, std::make_shared<const media::audio::WaveformSummary>(
                                                  *summary.value()));
                                keepAudio(asset.id,
                                          std::make_shared<const media::audio::AudioBuffer>(
                                              std::move(*decoded.value())));
                            } else
                                preview.audioDiagnostic =
                                    media::audio::describeAudioError(*summary.error());
                        }
                        results->assets.emplace(asset.id, std::move(preview));
                    } else if (asset.kind == document::AssetKind::Lut) {
                        Preview preview;
                        const auto path = media::resolveImagePath(
                            asset.locator.path, asset.locator.relinkHint, directory);
                        const auto lut = color::readLutFile(
                            path, [&context] { return context.isCancellationRequested(); });
                        preview.missing =
                            lut.error != color::LutError::None || lut.digest != asset.contentDigest;
                        results->assets.emplace(asset.id, std::move(preview));
                    } else if (asset.kind == document::AssetKind::Font) {
                        Preview preview;
                        preview.missing = !fontFileMatches(asset);
                        results->assets.emplace(asset.id, std::move(preview));
                    } else {
                        runtime::CompiledImageSource source;
                        source.asset = asset;
                        source.premultiply = asset.interpretation.alphaAssociation ==
                                             document::AssetAlphaAssociation::Straight;
                        results->assets.emplace(asset.id, decode(source, core::RationalTime{}));
                    }
                    results->proxyBytes = trimProxyStorage(results->cache, results->assets,
                                                           results->nodes, proxyLimit());
                    progress();
                }
            for (const auto& source : sources) {
                if (context.isCancellationRequested())
                    return runtime::TaskResult<std::shared_ptr<Thumbnails>>::cancelled();
                if (source.asset && source.asset->kind == document::AssetKind::Video) {
                    const auto asset = results->assets.find(source.asset->id);
                    if (asset != results->assets.end())
                        results->nodes.insert_or_assign(source.sourceNodeId, asset->second);
                } else
                    results->nodes.insert_or_assign(source.sourceNodeId, decode(source, time));
                results->proxyBytes =
                    trimProxyStorage(results->cache, results->assets, results->nodes, proxyLimit());
                progress();
            }
            return runtime::TaskResult<std::shared_ptr<Thumbnails>>::succeeded(std::move(results));
        });
    if (submission.accepted()) {
        preview_ = std::move(submission.handle);
        previewPending_ = true;
        previewDirty_ = false;
        assetsDirty_ = false;
        previewIncludesAssets_ = includeAssets;
        previewContentGeneration_ = contentGeneration_;
        bridge_.wake();
    }
}
void AssetController::poll() {
    if (inputColorCatalogPending_) {
        if (auto result = inputColorCatalogTask_.tryTakeResult()) {
            inputColorCatalogPending_ = false;
            if (result->state() == runtime::TaskState::Succeeded && result->value().has_value() &&
                *result->value() && host_.colorSettings() &&
                (*result->value())->settings == *host_.colorSettings() &&
                (*result->value())->workingColorSpaceId ==
                    session_.colorIntent().workingColorSpaceId)
                inputColorCatalog_ = *result->value();
            else if (result->state() == runtime::TaskState::Failed &&
                     inputColorCatalogRequestedSettings_ && host_.colorSettings() &&
                     *inputColorCatalogRequestedSettings_ == *host_.colorSettings()) {
                inputColorCatalog_ = std::make_shared<InputColorCatalog>();
                inputColorCatalog_->settings = *inputColorCatalogRequestedSettings_;
                inputColorCatalog_->workingColorSpaceId = inputColorCatalogRequestedWorking_;
                inputColorCatalog_->diagnostic =
                    tr("OCIO colour choices could not be prepared. Reopen the picker to retry.");
            }
            inputColorCatalogRequestedSettings_.reset();
            emit changed();
        }
    }
    if (busy_) {
        auto result = import_.tryTakeResult();
        if (result) {
            busy_ = false;
            emit activityChanged();
            if (result->state() == runtime::TaskState::Succeeded && result->value().has_value() &&
                *result->value()) {
                if (!host_.liveDocumentAndStack().first || !base_ ||
                    base_->project().id() != session_.snapshot().project().id() ||
                    base_->revision() != session_.snapshot().revision()) {
                    emit diagnostic(tr("Project changed during import; import the files again"));
                } else {
                    commands::Transaction transaction("Import Assets", base_->revision());
                    if (transaction.add(std::move(**result->value())))
                        static_cast<void>(session_.executeTransaction(std::move(transaction)));
                }
            }
            if (result->state() == runtime::TaskState::Failed)
                for (const auto& issue : result->diagnostics())
                    emit diagnostic(QString::fromStdString(issue.summary));
            base_.reset();
        }
    }
    if (previewPending_) {
        auto result = preview_.tryTakeResult();
        if (result) {
            previewPending_ = false;
            const bool currentContent = previewContentGeneration_ == contentGeneration_;
            if (currentContent && result->value().has_value() && *result->value()) {
                timeDependentNodes_ = (*result->value())->timeDependentNodes;
                const auto samePreview = [](const auto& left, const auto& right) {
                    return left.first == right.first && left.second.key == right.second.key &&
                           left.second.image.cacheKey() == right.second.image.cacheKey() &&
                           left.second.missing == right.second.missing &&
                           left.second.inputColorSpaceName == right.second.inputColorSpaceName &&
                           left.second.inputColorSpaceWarning ==
                               right.second.inputColorSpaceWarning &&
                           left.second.audioDiagnostic == right.second.audioDiagnostic &&
                           left.second.inputColorSpaceAutomatic ==
                               right.second.inputColorSpaceAutomatic;
                };
                const auto& prepared = **result->value();
                const bool assetsChanged =
                    previewIncludesAssets_ &&
                    (previews_.size() != prepared.assets.size() ||
                     !std::equal(previews_.begin(), previews_.end(), prepared.assets.begin(),
                                 samePreview) ||
                     waveforms_ != prepared.waveforms || audioBuffers_ != prepared.audioBuffers);
                if (previewIncludesAssets_) {
                    previews_ = std::move((*result->value())->assets);
                    waveforms_ = std::move((*result->value())->waveforms);
                    audioBuffers_ = std::move((*result->value())->audioBuffers);
                }
                nodePreviews_ = std::move((*result->value())->nodes);
                thumbnailCache_ = std::move((*result->value())->cache);
                auto& ledger = runtime::processMemoryBudgetLedger();
                proxyCacheBytes_ = trimProxyStorage(thumbnailCache_, previews_, nodePreviews_,
                                                    ledger.cacheByteBudget(&proxyCacheBytes_));
                decodedAudioBytes_ =
                    trimAudioStorage(audioBuffers_, ledger.cacheByteBudget(&decodedAudioBytes_));
                if (assetsChanged)
                    emit changed();
                emit nodeThumbnailsChanged();
            }
            if (previewDirty_ && !assetsDirty_ && !timeDependentNodes_)
                previewDirty_ = false;
            if (previewDirty_)
                startThumbnails();
        }
    }
}
} // namespace bloom::ui
