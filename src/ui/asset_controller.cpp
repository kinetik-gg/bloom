#include "network_share_paths.hpp"
#include <QDir>
#include <QFileDialog>
#include <QUrl>
#include <QUuid>
#include <algorithm>
#include <bloom/color/bloom_neutral_builtin.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/media/audio/audio.hpp>
#include <bloom/media/cache/media_disk_cache_decode.hpp>
#include <bloom/media/image.hpp>
#include <bloom/platform/font_catalog.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/value_graph_evaluation.hpp>
#include <bloom/ui/asset_controller.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/project_host.hpp>
#include <bloom/ui/task_ui_bridge.hpp>
#include <cmath>
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
    std::string cacheKey;
    // The same disk-cache key format image_source.cpp's selectImageSource() builds (see
    // media::cache::buildImageCacheKey()): asset digest + member frame + interpretation + Bloom
    // Neutral config digest + decoder identity, so a proxy decode here and a full evaluator decode
    // of the same source share one disk entry (docs/architecture/media-io.md "Disk cache").
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
                                   const runtime::CancellationToken& cancel) {
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
        const auto length = asset.manifest.last - asset.manifest.first + 1;
        auto offset = static_cast<std::int64_t>(elapsed);
        if (offset < 0)
            offset = 0;
        else if (source.loopMode == 1)
            offset %= length;
        else if (source.loopMode == 2 && length > 1) {
            const auto period = 2 * (length - 1);
            offset %= period;
            if (offset >= length)
                offset = period - offset;
        } else
            offset = std::min(offset, length - 1);
        const auto wanted = asset.manifest.first + offset;
        auto found = std::ranges::upper_bound(asset.manifest.members, wanted, {},
                                              &document::AssetSequenceMember::frame);
        if (found != asset.manifest.members.begin())
            --found;
        locator = &found->locator;
        selected.digest = found->contentDigest;
        memberFrame = found->frame;
    }
    selected.path = media::resolveImagePath(locator->path, locator->relinkHint, directory);
    selected.interpretation.colorSpace = static_cast<media::ImageColorSpace>(
        source.colorSpace == 0 ? static_cast<std::int64_t>(asset.interpretation.colorSpace)
                               : source.colorSpace);
    selected.interpretation.alphaAssociation = source.premultiply
                                                   ? media::ImageAlphaAssociation::Straight
                                                   : media::ImageAlphaAssociation::Premultiplied;
    const auto probe =
        media::probeImage(selected.path, [&] { return cancel.isCancellationRequested(); });
    selected.available = probe.value && probe.value->contentDigest == selected.digest;
    const auto hex = selected.digest.toLowercaseHex();
    const auto config = color::kBloomNeutralV1ConfigDigest.toLowercaseHex();
    selected.cacheKey = std::string(hex.begin(), hex.end()) + ":" + std::to_string(memberFrame) +
                        ":" + std::to_string(static_cast<int>(selected.interpretation.colorSpace)) +
                        ":" +
                        std::to_string(static_cast<int>(selected.interpretation.alphaAssociation)) +
                        ":" + std::string(config.begin(), config.end());
    media::cache::ImageCacheKeyInputs diskInputs;
    diskInputs.contentDigest = selected.digest;
    diskInputs.memberFrame = memberFrame;
    diskInputs.colorSpace = selected.interpretation.colorSpace;
    diskInputs.alphaAssociation = selected.interpretation.alphaAssociation;
    diskInputs.configDigest = color::kBloomNeutralV1ConfigDigest;
    selected.diskCacheKey =
        media::cache::buildImageCacheKey(diskInputs, media::cache::kImageDecoderIdentity);
    return selected;
}
unsigned char srgb(float value) {
    const double linear = static_cast<double>(value);
    const double encoded =
        linear <= 0.0031308 ? linear * 12.92 : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
    return static_cast<unsigned char>(std::lround(std::clamp(encoded, 0.0, 1.0) * 255.0));
}
} // namespace
AssetController::AssetController(CompositionSession& session, ProjectHost& host,
                                 runtime::TaskScheduler& scheduler, TaskUiBridge& bridge,
                                 media::cache::MediaDiskCache* mediaDiskCache, QObject* parent)
    : QObject(parent), session_(session), host_(host), scheduler_(scheduler), bridge_(bridge),
      mediaDiskCache_(mediaDiskCache) {
    dragToken_ = QUuid::createUuid().toByteArray();
    session_.setAssetController(this);
    connect(&bridge_, &TaskUiBridge::snapshotsPolled, this, &AssetController::poll);
    connect(&session_, &CompositionSession::snapshotChanged, this, &AssetController::refresh);
    connect(&session_, &CompositionSession::currentTimeChanged, this, &AssetController::refresh);
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
    connect(this, &AssetController::diagnostic, &session_, &CompositionSession::commandRejected);
    refresh();
}
AssetController::~AssetController() {
    cancel();
    session_.setAssetController(nullptr);
}
bool AssetController::acceptsEdits() const { return host_.canSave(); }
void AssetController::cancel() {
    if (busy_)
        import_.cancel();
    if (previewPending_)
        preview_.cancel();
}
std::filesystem::path AssetController::baseDirectory() const {
    const auto path = host_.displayPath();
    return path ? path->parent_path() : nativePath(QDir::currentPath());
}
bool AssetController::missing(document::AssetId id) const {
    const auto found = previews_.find(id);
    return found != previews_.end() && found->second.missing;
}
QImage AssetController::thumbnail(document::AssetId id) const {
    const auto found = previews_.find(id);
    return found == previews_.end() ? QImage{} : found->second.image;
}
QImage AssetController::nodeThumbnail(document::NodeId id) const {
    const auto found = nodePreviews_.find(id);
    return found == nodePreviews_.end() ? QImage{} : found->second.image;
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
    QFileDialog dialog(parent, tr("Import Media"), {},
                       tr("Media (*.png *.jpg *.jpeg *.exr *.tif *.tiff *.wav *.mp3 "
                          "*.PNG *.JPG *.JPEG *.EXR *.TIF *.TIFF *.WAV *.MP3)"));
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
             : tr("Media (*.png *.jpg *.jpeg *.exr *.tif *.tiff *.wav *.mp3 *.PNG *.JPG *.JPEG "
                  "*.EXR *.TIF *.TIFF *.WAV *.MP3)"));
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
            else
                *result =
                    std::make_unique<commands::ImportAssets>(files, directory, cancel, progress);
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
    // A preserved-read-only install retires the document behind CompositionSession. Its hidden
    // projection must not be read until the application rebinds it to decoded content again.
    if (!host_.liveDocumentAndStack().first) {
        if (previewPending_)
            preview_.cancel();
        previewPending_ = false;
        return;
    }
    nodePreviews_.clear();
    emit changed();
    if (previewPending_) {
        previewDirty_ = true;
        preview_.cancel();
        return;
    }
    previewDirty_ = false;
    const auto snapshot = session_.snapshot();
    const auto directory = baseDirectory();
    const auto time = session_.currentTime();
    const auto* composition = session_.composition();
    const auto rate =
        composition ? composition->format().frameRate() : document::FrameRate::framesPerSecond24();
    std::vector<runtime::CompiledImageSource> sources;
    if (composition)
        for (const auto& node : composition->graph().nodes()) {
            if (node.typeId != "bloom.image-source")
                continue;
            runtime::CompiledImageSource source;
            source.sourceNodeId = node.id;
            for (const auto& binding : node.parameters) {
                const auto* parameter = composition->parameters().find(binding.parameterId);
                const auto* constant =
                    parameter ? std::get_if<document::ConstantValueSource>(&parameter->source)
                              : nullptr;
                if (!constant)
                    continue;
                if (binding.role == "asset") {
                    if (const auto* id = std::get_if<std::string>(&constant->value))
                        if (const auto* asset =
                                snapshot.project().findAsset(document::AssetId::fromRaw(
                                    QString::fromStdString(*id).toULongLong())))
                            source.asset = *asset;
                } else if (const auto* integer = std::get_if<std::int64_t>(&constant->value)) {
                    if (binding.role == "startFrame")
                        source.startFrame = *integer;
                    if (binding.role == "loopMode")
                        source.loopMode = *integer;
                    if (binding.role == "colorSpace")
                        source.colorSpace = *integer;
                } else if (binding.role == "premultiply") {
                    if (const auto* value = std::get_if<bool>(&constant->value))
                        source.premultiply = *value;
                }
            }
            sources.push_back(std::move(source));
        }
    auto submission = scheduler_.submit<std::shared_ptr<Thumbnails>>(
        runtime::TaskRequest(
            "Image thumbnails",
            {.kind = runtime::TaskOwnerKind::Application, .id = runtime::TaskOwnerId::fromRaw(1)},
            runtime::TaskPriority::Background, runtime::TaskExecutor::BlockingIo),
        [snapshot, directory, time, rate, sources = std::move(sources), cached = thumbnailCache_,
         diskCache = mediaDiskCache_](runtime::TaskContext& context) mutable {
            auto results = std::make_shared<Thumbnails>();
            results->cache = std::move(cached);
            // The proxy cache carries over from the previous run, so its byte account has to carry
            // over with it: starting from zero here would let the map grow past the capacity by
            // whatever the previous run had already put in it.
            for (const auto& [key, image] : results->cache)
                results->proxyBytes += proxyByteCost(image);
            const auto decode = [&](const runtime::CompiledImageSource& source) {
                Preview preview;
                const auto selected =
                    selectThumbnail(source, time, rate, directory, context.cancellation());
                preview.key = selected.cacheKey;
                preview.missing = !selected.available;
                if (preview.missing)
                    return preview;
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
                    media::kMaxImageStorageBytes);
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
                    for (int y = 0; y < preview.image.height(); ++y)
                        for (int x = 0; x < width; ++x) {
                            const auto sx = static_cast<std::uint32_t>(x) * extent.width() /
                                            static_cast<std::uint32_t>(width);
                            const auto sy = static_cast<std::uint32_t>(y) * extent.height() /
                                            static_cast<std::uint32_t>(preview.image.height());
                            const auto pixel =
                                image.pixels()[static_cast<std::size_t>(sy) * extent.width() + sx];
                            auto* output =
                                preview.image.scanLine(y) + static_cast<std::ptrdiff_t>(x) * 4;
                            const auto alpha = pixel.alpha();
                            output[0] = srgb(alpha > 0 ? pixel.red() / alpha : 0);
                            output[1] = srgb(alpha > 0 ? pixel.green() / alpha : 0);
                            output[2] = srgb(alpha > 0 ? pixel.blue() / alpha : 0);
                            output[3] = static_cast<unsigned char>(std::lround(alpha * 255.0F));
                        }
                }
                if (!preview.image.isNull()) {
                    // CACHEFIX-1: the 512-entry bound was only ever a bound on COUNT, and the
                    // 8 MiB it claimed was an unmeasured inference from "at most 64 x 64 RGBA8".
                    // Charge the real bytes, keep both bounds, and make the figure readable.
                    const auto cost = proxyByteCost(preview.image);
                    while (!results->cache.empty() &&
                           (results->cache.size() >= kProxyCacheEntryLimit ||
                            cost > kProxyCacheByteCapacity -
                                       std::min(results->proxyBytes, kProxyCacheByteCapacity))) {
                        results->proxyBytes -= std::min(
                            results->proxyBytes, proxyByteCost(results->cache.begin()->second));
                        results->cache.erase(results->cache.begin());
                    }
                    if (cost <= kProxyCacheByteCapacity) {
                        results->cache.emplace(preview.key, preview.image);
                        results->proxyBytes += cost;
                    }
                }
                return preview;
            };
            std::uint64_t completed = 0;
            const auto progress = [&] {
                context.reportProgress(
                    {.phase = "Image thumbnails",
                     .subphase = "Decoding proxies",
                     .completed = ++completed,
                     .total = snapshot.project().assets().size() + sources.size()});
            };
            for (const auto& asset : snapshot.project().assets()) {
                if (context.isCancellationRequested())
                    return runtime::TaskResult<std::shared_ptr<Thumbnails>>::cancelled();
                if (asset.kind == document::AssetKind::Audio) {
                    Preview preview;
                    preview.missing = true;
                    auto path = directory / asset.locator.path;
                    if (!std::filesystem::exists(path) && !asset.locator.relinkHint.empty())
                        path = nativePath(QString::fromStdString(asset.locator.relinkHint));
                    auto decoded = media::audio::decodeAudio(path);
                    if (decoded.value() != nullptr) {
                        const auto summary = media::audio::waveformSummary(*decoded.value(), 256);
                        if (summary.value() != nullptr) {
                            preview.missing = false;
                            results->waveforms.emplace(
                                asset.id, std::make_shared<const media::audio::WaveformSummary>(
                                              *summary.value()));
                            // CACHEFIX-1: a decoded buffer is capped per asset by
                            // AudioDecodeLimits::kDefaultSampleBudget (48M samples, ~192 MB), but
                            // the MAP of them had no aggregate bound at all -- a project with
                            // twenty long audio assets could retain multiple gigabytes the memory
                            // ledger never saw. The waveform summary is kept either way, so the
                            // timeline still draws the asset; only the playable buffer is refused.
                            const auto audioCost = audioBufferByteCost(*decoded.value());
                            if (audioCost <=
                                kDecodedAudioByteCapacity -
                                    std::min(results->audioBytes, kDecodedAudioByteCapacity)) {
                                results->audioBuffers.emplace(
                                    asset.id, std::make_shared<const media::audio::AudioBuffer>(
                                                  std::move(*decoded.value())));
                                results->audioBytes += audioCost;
                            }
                        }
                    }
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
                    results->assets.emplace(asset.id, decode(source));
                }
                progress();
            }
            for (const auto& source : sources) {
                if (context.isCancellationRequested())
                    return runtime::TaskResult<std::shared_ptr<Thumbnails>>::cancelled();
                results->nodes.emplace(source.sourceNodeId, decode(source));
                progress();
            }
            return runtime::TaskResult<std::shared_ptr<Thumbnails>>::succeeded(std::move(results));
        });
    if (submission.accepted()) {
        preview_ = std::move(submission.handle);
        previewPending_ = true;
        bridge_.wake();
    }
}
void AssetController::poll() {
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
            if (previewDirty_) {
                refresh();
                return;
            }
            if (result->value().has_value() && *result->value()) {
                previews_ = std::move((*result->value())->assets);
                nodePreviews_ = std::move((*result->value())->nodes);
                thumbnailCache_ = std::move((*result->value())->cache);
                waveforms_ = std::move((*result->value())->waveforms);
                audioBuffers_ = std::move((*result->value())->audioBuffers);
                proxyCacheBytes_ = (*result->value())->proxyBytes;
                decodedAudioBytes_ = (*result->value())->audioBytes;
                emit changed();
            }
        }
    }
}
} // namespace bloom::ui
