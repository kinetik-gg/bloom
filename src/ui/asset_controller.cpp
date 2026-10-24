#include "asset_data_thumbnail.hpp"
#include "asset_thumbnail_display.hpp"
#include "network_share_paths.hpp"
#include <QDir>
#include <QFileDialog>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <algorithm>
#include <array>
#include <bloom/color/bloom_neutral_builtin.hpp>
#include <bloom/color/ocio_builtin_registry.hpp>
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
#include <bloom/platform/source_identity.hpp>
#include <bloom/render/processing_host_storage.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/gpu_processing_phase_policy.hpp>
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
#include <limits>
#include <optional>
#include <span>

namespace bloom::ui {
namespace {
// CACHEFIX-1 proxy/audio caps. The proxy cache keeps its historic 512-entry bound and adds the
// byte bound that bound only ever implied; the audio cache gets the aggregate bound it never had.
constexpr std::size_t kProxyCacheEntryLimit = 512;
constexpr std::size_t kProxyCacheByteCapacity = std::size_t{8} * 1024U * 1024U;

[[nodiscard]] std::size_t proxyByteCost(const QImage& image) {
    // sizeInBytes() is the allocation QImage actually holds, stride padding included.
    return image.isNull() ? 0 : static_cast<std::size_t>(image.sizeInBytes());
}

struct AudioBufferBackingBytes final {
    std::uint64_t allocated = 0;
    std::uint64_t initialized = 0;
};

[[nodiscard]] std::optional<AudioBufferBackingBytes>
measuredAudioBufferBytes(const media::audio::AudioBuffer& buffer) noexcept {
    constexpr auto max = std::numeric_limits<std::uint64_t>::max();
    AudioBufferBackingBytes bytes{sizeof(media::audio::AudioBuffer),
                                  sizeof(media::audio::AudioBuffer)};
    const auto add = [](const std::size_t count, const std::size_t width, std::uint64_t& total) {
        if (count > (max - total) / width)
            return false;
        total += static_cast<std::uint64_t>(count) * static_cast<std::uint64_t>(width);
        return true;
    };
    if (!add(buffer.planes.capacity(), sizeof(std::vector<float>), bytes.allocated) ||
        !add(buffer.planes.size(), sizeof(std::vector<float>), bytes.initialized))
        return {};
    for (const auto& plane : buffer.planes) {
        if (!add(plane.capacity(), sizeof(float), bytes.allocated) ||
            !add(plane.size(), sizeof(float), bytes.initialized))
            return {};
    }
    if (buffer.stream) {
        const auto retained = buffer.stream->retainedBytes();
        if (retained > max - bytes.allocated)
            return {};
        bytes.allocated += retained;
        // Decoder allowance is a conservative charge, never fictitious sampled-memory credit.
        bytes.initialized += retained - 256U * 1024U;
    }
    return bytes;
}

[[nodiscard]] std::size_t audioBufferByteCost(const media::audio::AudioBuffer& buffer) {
    const auto bytes = measuredAudioBufferBytes(buffer);
    return bytes && bytes->allocated <= std::numeric_limits<std::size_t>::max()
               ? static_cast<std::size_t>(bytes->allocated)
               : std::numeric_limits<std::size_t>::max();
}

[[nodiscard]] std::shared_ptr<const media::audio::AudioBuffer>
adoptAudioBuffer(media::audio::AudioBuffer&& decoded,
                 const std::shared_ptr<render::GpuProcessingPhaseLease>& phase) {
    // Measure after the final move. The full capacity stays claimed; only initialized elements
    // earn credit against a later physical sample. A phase estimate alone earns no credit.
    auto backing = std::make_unique<media::audio::AudioBuffer>(std::move(decoded));
    const auto bytes = measuredAudioBufferBytes(*backing);
    if (!bytes)
        return {};
    if (phase)
        return render::adoptProcessingHostBacking(std::move(backing), phase, bytes->allocated,
                                                  bytes->initialized);
    return std::shared_ptr<const media::audio::AudioBuffer>(std::move(backing));
}

struct AudioStorageClaim final {
    std::uint64_t samples = 0;
    std::uint64_t retainedBytes = 0;
    std::uint64_t scratchBytes = 0;
};

[[nodiscard]] std::optional<AudioStorageClaim>
audioStorageClaim(const media::audio::AudioProbe& probe,
                  const std::uint64_t chunkFrames = 4096) noexcept {
    constexpr auto max = std::numeric_limits<std::uint64_t>::max();
    if (probe.frames == 0 || probe.channels == 0 || probe.frames > max / probe.channels ||
        probe.frames > std::numeric_limits<std::size_t>::max())
        return {};
    const auto samples = probe.frames * probe.channels;
    if (samples > std::numeric_limits<std::size_t>::max() / sizeof(float) ||
        samples > max / sizeof(float))
        return {};
    const auto planes = static_cast<std::uint64_t>(probe.channels) * sizeof(std::vector<float>);
    const auto payload = samples * sizeof(float);
    if (payload > max - sizeof(media::audio::AudioBuffer) ||
        planes > max - sizeof(media::audio::AudioBuffer) - payload)
        return {};
    const auto scratchSamples = std::min(chunkFrames, probe.frames) * probe.channels;
    const auto retained = sizeof(media::audio::AudioBuffer) + planes + payload;
    if (scratchSamples > (max - retained) / sizeof(float))
        return {};
    if (retained > std::numeric_limits<std::size_t>::max() - scratchSamples * sizeof(float))
        return {};
    return AudioStorageClaim{samples, retained, scratchSamples * sizeof(float)};
}

[[nodiscard]] std::optional<AudioStorageClaim>
windowedAudioStorageClaim(const media::audio::AudioProbe& probe) noexcept {
    constexpr auto window = media::audio::AudioStream::kWindowFrames;
    constexpr std::uint64_t decoderAllowance = 256U * 1024U;
    const auto channels = static_cast<std::uint64_t>(probe.channels);
    if (probe.frames == 0 || channels == 0 ||
        channels > std::numeric_limits<std::size_t>::max() / (window * sizeof(float)) ||
        channels > std::numeric_limits<std::uint64_t>::max() / (window * sizeof(float)))
        return {};
    const auto samples = static_cast<std::uint64_t>(window) * probe.channels;
    const auto retained = sizeof(media::audio::AudioBuffer) + sizeof(media::audio::AudioStream) +
                          samples * sizeof(float) + decoderAllowance;
    const auto scratch = samples * sizeof(float) + static_cast<std::uint64_t>(256) *
                                                       probe.channels *
                                                       sizeof(media::audio::WaveformRange);
    if (retained > std::numeric_limits<std::uint64_t>::max() - scratch)
        return {};
    return AudioStorageClaim{samples, retained, scratch};
}

enum class AudioSourceCheck { Matches, MissingOrChanged, Cancelled };

// Identity-verified: one stat while the file matches its last full hash, a rehash otherwise.
[[nodiscard]] AudioSourceCheck verifyAudioSource(const std::filesystem::path& path,
                                                 const core::Sha256Digest& expected,
                                                 const runtime::CancellationToken& cancellation) {
    switch (platform::verifySourceDigest(path, expected,
                                         [&] { return cancellation.isCancellationRequested(); })) {
    case platform::SourceCheck::Matches:
        return AudioSourceCheck::Matches;
    case platform::SourceCheck::Cancelled:
        return AudioSourceCheck::Cancelled;
    case platform::SourceCheck::Changed:
    case platform::SourceCheck::Missing:
        break;
    }
    return AudioSourceCheck::MissingOrChanged;
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

[[nodiscard]] std::string importFailureText(const commands::ImportFailure& failure) {
    const auto name = failure.path.filename().u8string();
    return "Could not import " + std::string(name.begin(), name.end()) + ": " + failure.diagnostic;
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
    // The verified source bytes are present (a false `available` beside it is a colour or
    // configuration refusal, not a missing file).
    bool sourcePresent = false;
    // The selected part/layer carries no colour channels (a data-only image).
    bool dataOnly = false;
    std::string part;
};

[[nodiscard]] bool fontFileMatches(const document::AssetRecord& asset) {
    if (asset.locator.portability == "builtin")
        return true;
    constexpr std::uint64_t kMaximumFontBytes = std::uint64_t{64} * 1024U * 1024U;
    return platform::verifySourceDigest(asset.locator.path, asset.contentDigest, {},
                                        kMaximumFontBytes) == platform::SourceCheck::Matches;
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
        if (elapsed < -9223372036854775808.0L || elapsed >= 9223372036854775808.0L)
            return selected;
        const auto length = static_cast<std::uint64_t>(asset.manifest.last) -
                            static_cast<std::uint64_t>(asset.manifest.first) + 1;
        const auto signedOffset = static_cast<std::int64_t>(elapsed);
        std::uint64_t offset = 0;
        if (signedOffset >= 0) {
            offset = static_cast<std::uint64_t>(signedOffset);
            if (source.loopMode == 1)
                offset %= length;
            else if (source.loopMode == 2 && length > 1) {
                const auto period = 2 * (length - 1);
                offset %= period;
                if (offset >= length)
                    offset = period - offset;
            } else
                offset = std::min(offset, length - 1);
        }
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
    selected.sourcePresent = selected.available;
    selected.part = source.part;
    if (selected.available) {
        const auto* part = probe.value->findPart(source.part);
        selected.dataOnly = part != nullptr && std::ranges::find(part->colourLayers, source.layer) ==
                                                   part->colourLayers.end();
    }
    if (selected.available && !selected.dataOnly) {
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
    const auto synthesized =
        color::synthesizedColorSpaceIdentity(selected.interpretation.inputColorSpaceId);
    selected.cacheKey += ":" + synthesized;
    media::cache::ImageCacheKeyInputs diskInputs;
    diskInputs.contentDigest = selected.digest;
    diskInputs.memberFrame = memberFrame;
    diskInputs.colorSpace = selected.interpretation.colorSpace;
    diskInputs.inputColorSpaceId = selected.interpretation.inputColorSpaceId;
    diskInputs.workingColorSpaceId = selected.workingColorSpaceId;
    diskInputs.alphaAssociation = selected.interpretation.alphaAssociation;
    diskInputs.configDigest = selected.configRevision;
    diskInputs.synthesizedColorSpaceIdentity = synthesized;
    selected.diskCacheKey =
        media::cache::buildImageCacheKey(diskInputs, media::cache::kImageDecoderIdentity);
    return selected;
}
} // namespace
AssetController::AssetController(CompositionSession& session, ProjectHost& host,
                                 runtime::TaskScheduler& scheduler, TaskUiBridge& bridge,
                                 media::cache::MediaDiskCache* mediaDiskCache, QObject* parent,
                                 AudioProcessingAdmission audioAdmission)
    : QObject(parent), session_(session), host_(host), scheduler_(scheduler), bridge_(bridge),
      mediaDiskCache_(mediaDiskCache), audioAdmission_(std::move(audioAdmission)) {
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
            &decodedAudioBytes_, audioAdmission_.cacheByteCapacity,
            [this] { return decodedAudioBytes_; },
            [this, previous = audioAdmission_.cacheByteCapacity](const std::size_t bytes) mutable {
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
        audioDeliveryBuffers_.clear();
        adoptedAudioBuffers_.clear();
        retryAudioAfterCapacitySample_ = false;
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
void AssetController::requestImageParts(const document::AssetId id) {
    const auto* asset = session_.snapshot().project().findAsset(id);
    if (asset == nullptr || imagePartTasks_.contains(id))
        return;
    if (const auto known = imageParts_.find(id);
        known != imageParts_.end() && known->second.digest == asset->contentDigest)
        return;
    auto submission = scheduler_.submit<std::shared_ptr<ImageParts>>(
        runtime::TaskRequest(
            "Read image parts",
            {.kind = runtime::TaskOwnerKind::Application, .id = runtime::TaskOwnerId::fromRaw(1)},
            runtime::TaskPriority::Visible, runtime::TaskExecutor::Cpu),
        // A sequence is read through its first member (its own verified bytes); the asset digest
        // is a manifest digest that no single file carries.
        [locator = asset->kind == document::AssetKind::Sequence && !asset->manifest.members.empty()
                       ? asset->manifest.members.front().locator
                       : asset->locator,
         fileDigest = asset->kind == document::AssetKind::Sequence &&
                              !asset->manifest.members.empty()
                          ? asset->manifest.members.front().contentDigest
                          : asset->contentDigest,
         digest = asset->contentDigest, base = baseDirectory()](runtime::TaskContext& context) {
            using Result = runtime::TaskResult<std::shared_ptr<ImageParts>>;
            if (context.isCancellationRequested())
                return Result::cancelled();
            const auto path = media::resolveImagePath(locator.path, locator.relinkHint, base);
            auto probe = media::probeImage(path, [&context] {
                return context.isCancellationRequested();
            });
            if (context.isCancellationRequested())
                return Result::cancelled();
            auto result = std::make_shared<ImageParts>();
            result->digest = digest;
            // A changed file is not listed: the pickers show the parts of the verified bytes.
            if (probe.value && probe.value->contentDigest == fileDigest)
                result->parts = std::move(probe.value->parts);

            return Result::succeeded(std::move(result));
        });
    if (submission.accepted()) {
        imagePartTasks_.emplace(id, std::move(submission.handle));
        bridge_.wake();
    }
}
const std::vector<media::ImagePartInfo>*
AssetController::imageParts(const document::AssetId id) const {
    const auto found = imageParts_.find(id);
    const auto* asset = session_.snapshot().project().findAsset(id);
    return found != imageParts_.end() && asset != nullptr &&
                   found->second.digest == asset->contentDigest
               ? &found->second.parts
               : nullptr;
}
std::filesystem::path AssetController::baseDirectory() const {
    const auto path = host_.displayPath();
    return path ? path->parent_path() : nativePath(QDir::currentPath());
}
bool AssetController::missing(document::AssetId id) const {
    const auto found = previews_.find(id);
    return found != previews_.end() && found->second.missing;
}
QString AssetController::problem(document::AssetId id) const {
    const auto found = previews_.find(id);
    return found == previews_.end() || found->second.missing
               ? QString{}
               : QString::fromStdString(found->second.problem);
}
bool AssetController::dataOnly(document::AssetId id) const {
    const auto found = previews_.find(id);
    return found != previews_.end() && found->second.dataOnly;
}
QString AssetController::audioDiagnostic(document::AssetId id) const {
    const auto found = previews_.find(id);
    if (found == previews_.end())
        return {};
    if (!found->second.audioDiagnostic.empty())
        return QString::fromStdString(found->second.audioDiagnostic);
    const auto buffer = audioBuffer(id);
    if (buffer && buffer->stream)
        if (const auto error = buffer->stream->diagnostic())
            return QString::fromStdString(media::audio::describeAudioError(*error));
    return {};
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
    const auto* asset = session_.snapshot().project().findAsset(id);
    const auto preview = previews_.find(id);
    if (!asset || preview == previews_.end() || preview->second.missing)
        return nullptr;
    const auto digest = asset->contentDigest.toLowercaseHex();
    if (preview->second.key.compare(0, digest.size(), digest.data(), digest.size()) != 0)
        return nullptr;
    const auto found = audioBuffers_.find(id);
    if (found != audioBuffers_.end())
        return found->second;
    const auto delivery = audioDeliveryBuffers_.find(id);
    if (delivery != audioDeliveryBuffers_.end())
        return delivery->second;
    const auto adopted = adoptedAudioBuffers_.find(id);
    return adopted == adoptedAudioBuffers_.end() ? nullptr : adopted->second.lock();
}
void AssetController::releaseAudioDelivery() {
    for (const auto& [id, buffer] : audioDeliveryBuffers_)
        adoptedAudioBuffers_.insert_or_assign(id, buffer);
    audioDeliveryBuffers_.clear();
}
void AssetController::retryAudioAfterCapacitySample() {
    if (!retryAudioAfterCapacitySample_)
        return;
    retryAudioAfterCapacitySample_ = false;
    assetsDirty_ = true;
    previewDirty_ = true;
    if (!previewPending_)
        startThumbnails();
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
            std::vector<runtime::TaskDiagnostic> warnings;
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
                if (!prepared->diagnostic().empty()) {
                    // Nothing importable: report every file's own reason, not only the first.
                    std::vector<runtime::TaskDiagnostic> issues;
                    for (const auto& failure : prepared->failures())
                        issues.push_back(
                            {.code = "bloom.media.import-failed",
                             .severity = runtime::DiagnosticSeverity::Error,
                             .summary = importFailureText(failure),
                             .detail = failure.diagnostic,
                             .suggestedAction = "Check the file and its reported limit."});
                    if (issues.empty())
                        issues.push_back(
                            {.code = "bloom.media.import-failed",
                             .severity = runtime::DiagnosticSeverity::Error,
                             .summary = prepared->diagnostic(),
                             .detail = prepared->diagnostic(),
                             .suggestedAction = "Check the file and its reported limit."});
                    return runtime::TaskResult<OperationHandle>::failed(std::move(issues));
                }
                // Skipped files stay listed individually on the job (Jobs panel) as warnings.
                for (const auto& failure : prepared->failures())
                    warnings.push_back({.code = "bloom.media.import-skipped",
                                        .severity = runtime::DiagnosticSeverity::Warning,
                                        .summary = importFailureText(failure),
                                        .detail = failure.diagnostic,
                                        .suggestedAction =
                                            "Check the file and its reported limit, then import "
                                            "it again."});
                *result = std::move(prepared);
            }
            if (context.isCancellationRequested())
                return runtime::TaskResult<OperationHandle>::cancelled();
            return runtime::TaskResult<OperationHandle>::succeeded(std::move(result),
                                                                   std::move(warnings));
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
         cachedNodes = nodePreviews_,
         cachedAudio = includeAssets ? [this] {
             auto buffers = audioBuffers_;
             buffers.insert(audioDeliveryBuffers_.begin(), audioDeliveryBuffers_.end());
             for (const auto& [id, weak] : adoptedAudioBuffers_)
                 if (const auto held = weak.lock())
                     buffers.emplace(id, held);
             return buffers;
         }() : AudioBuffers{},
         cachedWaveforms = includeAssets ? waveforms_ : Waveforms{},
         cachedPreviews = includeAssets ? previews_ : Previews{}, diskCache = mediaDiskCache_,
         proxyOwner = &proxyCacheBytes_,
         audioOwner = &decodedAudioBytes_,
         audioAdmission = audioAdmission_](runtime::TaskContext& context) mutable {
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
                    } else
                        results->audioDeliveryBuffers.emplace(id, buffer);
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
                // Missing means the verified bytes are gone; everything else is a present source
                // with a stated problem, or a data-only image shown through its channels.
                preview.missing = !selected.sourcePresent;
                if (preview.missing)
                    return preview;
                if (selected.dataOnly) {
                    preview.dataOnly = true;
                    preview.inputColorSpaceWarning.clear();
                    preview.key = selected.cacheKey + ":data";
                    if (const auto found = results->cache.find(preview.key);
                        found != results->cache.end()) {
                        preview.image = found->second;
                        return preview;
                    }
                    preview.image = detail::dataChannelThumbnail(
                        selected.path, selected.digest, selected.part,
                        [&] { return context.isCancellationRequested(); });
                    if (!preview.image.isNull())
                        keepProxy(preview.key, preview.image);
                    return preview;
                }
                if (!selected.available) {
                    preview.problem = selected.inputColorSpaceWarning.empty()
                                          ? "The image's colour cannot be interpreted"
                                          : selected.inputColorSpaceWarning;
                    return preview;
                }
                if (!thumbnailDisplay) {
                    preview.problem = "The selected OCIO thumbnail display processor is unavailable";
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
                if (!decoded.value.has_value())
                    preview.problem = decoded.diagnostic.empty() ? "The image could not be decoded"
                                                                 : decoded.diagnostic;
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
                        preview.problem =
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
                        // The decode session (and its worker) exists only for a thumbnail or
                        // audio miss. A known source identity verifies with one stat.
                        std::optional<media::video::VideoDecodeSession> decoder;
                        const auto video = [&]() -> media::video::VideoDecodeSession& {
                            if (!decoder)
                                decoder.emplace(path);
                            return *decoder;
                        };
                        const auto probe = runtime::video::probeMetadata(asset);
                        const auto observed = platform::observeSource(path);
                        const bool available =
                            observed.digest ? *observed.digest == asset.contentDigest
                                            : observed.identity &&
                                                  !video().verifySource(asset.contentDigest, [&] {
                                                      return context.isCancellationRequested();
                                                  });
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
                            const auto frame = video().frame(
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
                        // A present, verified video is never "missing": a colour refusal or an
                        // undecodable first frame is a stated problem, and its audio still plays.
                        preview.missing = !available;
                        if (available && preview.image.isNull())
                            preview.problem = !preview.inputColorSpaceWarning.empty()
                                                  ? preview.inputColorSpaceWarning
                                                  : "The first video frame could not be decoded";
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
                            const auto audioStream = std::ranges::find(
                                probe.streams, media::provider::MediaKind::Audio,
                                &media::provider::StreamDescriptor::kind);
                            const auto frames = audioStream == probe.streams.end()
                                                    ? std::nullopt
                                                    : media::video::audioClipFrameCount(*audioStream);
                            const auto claim = frames
                                                   ? audioStorageClaim(
                                                         {.rate = audioStream->sampleRate,
                                                          .channels = static_cast<std::uint32_t>(
                                                              audioStream->channelLayout.size()),
                                                          .frames = *frames},
                                                         media::provider::Limits::audioSamples)
                                                   : std::nullopt;
                            if (claim) {
                                std::shared_ptr<render::GpuProcessingPhaseLease> phase;
                                std::optional<render::ProcessingHostStorageToken> scratch;
                                if (audioAdmission.policy) {
                                    auto granted = audioAdmission.policy->reserve(
                                        {.output = runtime::GpuProcessingPhaseOutput::FloatFinal,
                                         .pixels = 0,
                                         .metadataBytes = claim->retainedBytes,
                                         .sourceFree = true,
                                         .native = false});
                                    if (granted.status ==
                                        render::GpuProcessingPhaseAdmission::AwaitingCapacity)
                                        results->retryAudioAfterCapacitySample = true;
                                    if (granted.lease) {
                                        phase = std::move(granted.lease);
                                        auto scratchGrant = render::ProcessingHostStorageToken::reserve(
                                            phase, claim->scratchBytes);
                                        scratch = std::move(scratchGrant.token);
                                    }
                                }
                                const auto enoughStandalone = !audioAdmission.policy &&
                                    claim->retainedBytes + claim->scratchBytes <=
                                        audioAdmission.standaloneByteAllowance;
                                if ((phase && scratch) || enoughStandalone) {
                                    auto decoded = media::video::decodeAudioClip(
                                        video(), probe, claim->samples,
                                        [&] { return context.isCancellationRequested(); });
                                    if (auto* audio = std::get_if<media::audio::AudioBuffer>(&decoded)) {
                                        const auto summary = media::audio::waveformSummary(*audio, 256);
                                        if (!summary.value())
                                            preview.audioDiagnostic =
                                                media::audio::describeAudioError(*summary.error());
                                        else if (auto buffer =
                                                     adoptAudioBuffer(std::move(*audio), phase)) {
                                            results->waveforms.emplace(
                                                asset.id,
                                                std::make_shared<const media::audio::WaveformSummary>(
                                                    *summary.value()));
                                            keepAudio(asset.id, buffer);
                                        } else
                                            preview.audioDiagnostic =
                                                "Video audio storage exceeded its admitted phase";
                                    } else
                                        preview.audioDiagnostic =
                                            std::get<media::provider::Unavailable>(decoded).detail;
                                } else {
                                    preview.audioDiagnostic =
                                        "Video audio requests " +
                                        std::to_string(claim->retainedBytes + claim->scratchBytes) +
                                        " processing bytes; admission was refused";
                                }
                            } else {
                                preview.audioDiagnostic =
                                    "Video audio sample count exceeds addressable storage";
                            }
                        }
                        results->assets.emplace(asset.id, std::move(preview));
                    } else if (asset.kind == document::AssetKind::Audio) {
                        Preview preview;
                        preview.missing = true;
                        const auto path = media::resolveImagePath(
                            asset.locator.path, asset.locator.relinkHint, directory);
                        const auto source = verifyAudioSource(path, asset.contentDigest,
                                                              context.cancellation());
                        if (source == AudioSourceCheck::Cancelled)
                            return runtime::TaskResult<std::shared_ptr<Thumbnails>>::cancelled();
                        if (source != AudioSourceCheck::Matches) {
                            preview.audioDiagnostic = "Audio source is unavailable or has changed";
                        } else {
                            const auto digest = asset.contentDigest.toLowercaseHex();
                            preview.key.assign(digest.begin(), digest.end());
                            const auto oldPreview = cachedPreviews.find(asset.id);
                            const auto oldBuffer = cachedAudio.find(asset.id);
                            const auto oldWaveform = cachedWaveforms.find(asset.id);
                            if (oldPreview != cachedPreviews.end() &&
                                oldPreview->second.key == preview.key &&
                                oldBuffer != cachedAudio.end() &&
                                oldWaveform != cachedWaveforms.end()) {
                                keepAudio(asset.id, oldBuffer->second);
                                results->waveforms.emplace(asset.id, oldWaveform->second);
                                preview.missing = false;
                            } else {
                                const auto cancelAudio = [&context] {
                                    return context.isCancellationRequested();
                                };
                                const auto probe = media::audio::probeAudio(path, {}, cancelAudio);
                                if (!probe) {
                                    preview.audioDiagnostic =
                                        media::audio::describeAudioError(*probe.error());
                                } else if (const auto claim = windowedAudioStorageClaim(*probe.value())) {
                                    std::shared_ptr<render::GpuProcessingPhaseLease> phase;
                                    std::optional<render::ProcessingHostStorageToken> scratch;
                                    if (audioAdmission.policy) {
                                        auto granted = audioAdmission.policy->reserve(
                                            {.output = runtime::GpuProcessingPhaseOutput::FloatFinal,
                                             .pixels = 0,
                                             .metadataBytes = claim->retainedBytes,
                                             .sourceFree = true,
                                             .native = false});
                                        if (granted.status ==
                                            render::GpuProcessingPhaseAdmission::AwaitingCapacity)
                                            results->retryAudioAfterCapacitySample = true;
                                        if (granted.lease) {
                                            phase = std::move(granted.lease);
                                            auto scratchGrant = render::ProcessingHostStorageToken::reserve(
                                                phase, claim->scratchBytes);
                                            scratch = std::move(scratchGrant.token);
                                        }
                                        if (!phase || !scratch) {
                                            const auto reservation =
                                                audioAdmission.policy->snapshot().reservations;
                                            const auto capacity = reservation.hostCapacityBytes;
                                            preview.audioDiagnostic =
                                                "Audio decode requests " +
                                                std::to_string(claim->retainedBytes +
                                                               claim->scratchBytes) +
                                                " bytes; processing host capacity is " +
                                                (capacity ? std::to_string(*capacity) :
                                                            std::string("unavailable")) +
                                                ", reserved " +
                                                std::to_string(reservation.hostReservedBytes) +
                                                " bytes";
                                        }
                                    } else if (claim->retainedBytes + claim->scratchBytes >
                                               audioAdmission.standaloneByteAllowance) {
                                        preview.audioDiagnostic =
                                            "Audio decode requests " +
                                            std::to_string(claim->retainedBytes +
                                                           claim->scratchBytes) +
                                            " bytes; standalone processing allowance is " +
                                            std::to_string(audioAdmission.standaloneByteAllowance);
                                    }
                                    if (preview.audioDiagnostic.empty()) {
                                        media::audio::AudioDecodeLimits limits;
                                        limits.sampleBudget = claim->samples;
                                        auto decoded = media::audio::openWindowedAudio(
                                            path, limits, media::audio::AudioStream::kWindowFrames,
                                            cancelAudio);
                                        if (!decoded) {
                                            preview.audioDiagnostic =
                                                media::audio::describeAudioError(*decoded.error());
                                        } else {
                                            auto summary = media::audio::waveformSummary(
                                                *decoded.value()->stream, 256, cancelAudio);
                                            if (!summary) {
                                                preview.audioDiagnostic =
                                                    media::audio::describeAudioError(*summary.error());
                                            } else {
                                                const auto afterSummary = verifyAudioSource(
                                                    path, asset.contentDigest,
                                                    context.cancellation());
                                                if (afterSummary == AudioSourceCheck::Cancelled)
                                                    return runtime::TaskResult<std::shared_ptr<Thumbnails>>::cancelled();
                                                if (afterSummary != AudioSourceCheck::Matches) {
                                                    preview.audioDiagnostic =
                                                        "Audio source changed during waveform preparation";
                                                } else if (auto buffer = adoptAudioBuffer(
                                                               std::move(*decoded.value()), phase)) {
                                                    results->waveforms.emplace(
                                                        asset.id,
                                                        std::make_shared<const media::audio::
                                                                                 WaveformSummary>(
                                                            std::move(*summary.value())));
                                                    keepAudio(asset.id, buffer);
                                                    preview.missing = false;
                                                } else {
                                                    preview.audioDiagnostic =
                                                        "Audio storage exceeded its admitted phase";
                                                }
                                            }
                                        }
                                    }
                                } else {
                                    preview.audioDiagnostic =
                                        "Audio frame count exceeds addressable sample storage";
                                }
                            }
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
void AssetController::emitImportFailures(const std::vector<runtime::TaskDiagnostic>& issues) {
    if (issues.empty())
        return;
    if (issues.size() == 1) {
        emit diagnostic(QString::fromStdString(issues.front().summary));
        return;
    }
    emit diagnostic(tr("%1 files could not be imported (details in Jobs). %2")
                        .arg(issues.size())
                        .arg(QString::fromStdString(issues.front().summary)));
}
void AssetController::poll() {
    for (auto task = imagePartTasks_.begin(); task != imagePartTasks_.end();) {
        auto result = task->second.tryTakeResult();
        if (!result) {
            ++task;
            continue;
        }
        if (result->state() == runtime::TaskState::Succeeded && result->value() &&
            *result->value())
            imageParts_[task->first] = std::move(**result->value());
        task = imagePartTasks_.erase(task);
        emit changed();
    }
    // A probe that finished between the bridge wake and this poll is collected on the next one.
    if (!imagePartTasks_.empty())
        QTimer::singleShot(20, &bridge_, &TaskUiBridge::wake);
    if (inputColorCatalogPending_) {
        if (auto result = inputColorCatalogTask_.tryTakeResult()) {
            inputColorCatalogPending_ = false;
            if (result->state() == runtime::TaskState::Succeeded) {
                auto catalog = result->value().value_or(nullptr);
                if (catalog && host_.colorSettings() &&
                    catalog->settings == *host_.colorSettings() &&
                    catalog->workingColorSpaceId == session_.colorIntent().workingColorSpaceId)
                    inputColorCatalog_ = std::move(catalog);
            } else if (result->state() == runtime::TaskState::Failed &&
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
                    // Successful files commit as one undoable transaction; every skipped file is
                    // reported with its own diagnostic.
                    commands::Transaction transaction("Import Assets", base_->revision());
                    if (transaction.add(std::move(**result->value())))
                        static_cast<void>(session_.executeTransaction(std::move(transaction)));
                    emitImportFailures(result->diagnostics());
                }
            }
            if (result->state() == runtime::TaskState::Failed)
                emitImportFailures(result->diagnostics());
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
                     waveforms_ != prepared.waveforms || audioBuffers_ != prepared.audioBuffers ||
                     audioDeliveryBuffers_ != prepared.audioDeliveryBuffers);
                if (previewIncludesAssets_) {
                    retryAudioAfterCapacitySample_ = prepared.retryAudioAfterCapacitySample;
                    previews_ = std::move((*result->value())->assets);
                    waveforms_ = std::move((*result->value())->waveforms);
                    audioBuffers_ = std::move((*result->value())->audioBuffers);
                    audioDeliveryBuffers_ = std::move((*result->value())->audioDeliveryBuffers);
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
