#pragma once
#include <QByteArray>
#include <QImage>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/commands/asset_operations.hpp>
#include <bloom/document/document.hpp>
#include <bloom/media/audio/audio.hpp>
#include <bloom/media/image.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>

class QWidget;
namespace bloom::media::cache {
class MediaDiskCache;
} // namespace bloom::media::cache
namespace bloom::runtime {
class GpuProcessingPhasePolicy;
}
namespace bloom::ui {
class CompositionSession;
class ProjectHost;
class TaskUiBridge;
struct AudioProcessingAdmission final {
    std::shared_ptr<runtime::GpuProcessingPhasePolicy> policy;
    // Explicit allowance for standalone hosts/tests without a sampled processing root.
    std::uint64_t standaloneByteAllowance = 0;
    // Retention target. A larger admitted buffer can still cross the playback handoff.
    std::size_t cacheByteCapacity = std::size_t{2} * 1024 * 1024 * 1024;
};
class AssetController final : public QObject {
    Q_OBJECT
  public:
    // `mediaDiskCache` is the same content-addressed disk cache the runtime evaluator consults
    // (docs/architecture/media-io.md "Disk cache"): proxy/thumbnail decodes go through it too, so
    // an asset the evaluator already decoded to disk never re-decodes here, and vice versa. Null
    // (the default) disables the disk-cache stage; nothing about the existing QImage proxy cache
    // changes either way.
    AssetController(CompositionSession& session, ProjectHost& host,
                    runtime::TaskScheduler& scheduler, TaskUiBridge& bridge,
                    media::cache::MediaDiskCache* mediaDiskCache = nullptr,
                    QObject* parent = nullptr,
                    AudioProcessingAdmission audioAdmission = AudioProcessingAdmission{});
    ~AssetController() override;
    void requestImport(QWidget* parent = nullptr);
    void importFiles(const QStringList& paths);
    void relink(document::AssetId id, QWidget* parent = nullptr);
    void remove(document::AssetId id);
    [[nodiscard]] bool busy() const noexcept { return busy_; }
    [[nodiscard]] bool missing(document::AssetId id) const;
    // Present but not displayable as it is (empty when fine or missing); see Preview::problem.
    [[nodiscard]] QString problem(document::AssetId id) const;
    // A data-only image (no colour layer) -- valid, used through its channels.
    [[nodiscard]] bool dataOnly(document::AssetId id) const;
    [[nodiscard]] QString audioDiagnostic(document::AssetId id) const;
    [[nodiscard]] QImage thumbnail(document::AssetId id) const;
    [[nodiscard]] QImage nodeThumbnail(document::NodeId id) const;
    [[nodiscard]] QString inputColorSpaceDisplay(document::AssetId id) const;
    [[nodiscard]] QString inputColorSpaceWarning(document::AssetId id) const;
    void requestInputColorSpaces();
    // ADR 0026: the part and layer pickers read the image probe through this worker (identity
    // verified, header only). The asset record persists no part list; the result is session
    // state keyed by the asset's content digest. changed() is emitted when it arrives.
    void requestImageParts(document::AssetId id);
    [[nodiscard]] const std::vector<media::ImagePartInfo>* imageParts(document::AssetId id) const;
    [[nodiscard]] const std::vector<color::OcioColorSpaceInfo>* inputColorSpaces() const noexcept;
    [[nodiscard]] QString inputColorCatalogDiagnostic() const;
    [[nodiscard]] std::shared_ptr<const media::audio::WaveformSummary>
    waveform(document::AssetId id) const;
    [[nodiscard]] std::shared_ptr<const media::audio::AudioBuffer>
    audioBuffer(document::AssetId id) const;
    // Called by the playback session after it has adopted the active mix's buffers.
    void releaseAudioDelivery();
    void retryAudioAfterCapacitySample();
    [[nodiscard]] std::filesystem::path baseDirectory() const;
    // CACHEFIX-1 accounting counters (no UI): the bytes this controller's two in-memory caches
    // hold. Both are hard-capped rather than budgeted -- they are small next to the frame caches,
    // but "small" has to be a measured claim, not an assumption in a comment.
    [[nodiscard]] std::size_t proxyCacheBytes() const noexcept { return proxyCacheBytes_; }
    [[nodiscard]] std::size_t decodedAudioBytes() const noexcept { return decodedAudioBytes_; }
    [[nodiscard]] bool thumbnailsPending() const noexcept {
        return previewPending_ || previewDirty_;
    }
    void cancel();
    // Prevent thumbnail decode submissions while an application cache purge drains its old
    // generation. Import/probe work does not access the shared decoded-frame store.
    void setCachePurgeGate(bool gated);
    [[nodiscard]] bool acceptsEdits() const;
    [[nodiscard]] QByteArray dragToken() const { return dragToken_; }
  signals:
    void changed();
    void nodeThumbnailsChanged();
    void activityChanged();
    void diagnostic(QString message);

  private:
    using OperationHandle = std::shared_ptr<std::unique_ptr<commands::Operation>>;
    struct Preview {
        QImage image;
        std::string key;
        std::string inputColorSpaceName;
        std::string inputColorSpaceWarning;
        std::string audioDiagnostic;
        bool inputColorSpaceAutomatic = false;
        // The source file is absent or no longer the imported bytes (Relink fixes it).
        bool missing = false;
        // The source is present but cannot be shown as it is (an uninterpretable colour tag, an
        // undecodable frame); the text says what to do. Never a Relink prompt.
        std::string problem;
        // The image has no colour layer: its pixels are data channels (Cryptomatte, AOVs). A
        // valid asset whose thumbnail is the Cryptomatte ID colours or its first data channel.
        bool dataOnly = false;
    };
    using Previews = std::map<document::AssetId, Preview>;
    using Waveforms =
        std::map<document::AssetId, std::shared_ptr<const media::audio::WaveformSummary>>;
    using AudioBuffers =
        std::map<document::AssetId, std::shared_ptr<const media::audio::AudioBuffer>>;
    struct Thumbnails {
        Previews assets;
        std::map<document::NodeId, Preview> nodes;
        std::map<std::string, QImage> cache;
        Waveforms waveforms;
        AudioBuffers audioBuffers;
        AudioBuffers audioDeliveryBuffers;
        // CACHEFIX-1 accounting: what these two maps actually hold, carried back from the worker
        // so the controller can report it without walking every entry on the interface thread.
        std::size_t proxyBytes = 0;
        std::size_t audioBytes = 0;
        bool retryAudioAfterCapacitySample = false;
        bool timeDependentNodes = false;
    };
    struct InputColorCatalog {
        document::ColorSettings settings;
        std::string workingColorSpaceId;
        std::vector<color::OcioColorSpaceInfo> spaces;
        QString diagnostic;
    };
    void prepare(const QStringList& paths, document::AssetId relinkId = {});
    void refresh();
    void refreshNodeThumbnails();
    void startThumbnails();
    void poll();
    // One status notice for an import job's diagnostics; the Jobs panel keeps each one.
    void emitImportFailures(const std::vector<runtime::TaskDiagnostic>& issues);
    CompositionSession& session_;
    ProjectHost& host_;
    runtime::TaskScheduler& scheduler_;
    TaskUiBridge& bridge_;
    media::cache::MediaDiskCache* mediaDiskCache_ = nullptr;
    AudioProcessingAdmission audioAdmission_;
    runtime::TaskHandle<OperationHandle> import_;
    runtime::TaskHandle<std::shared_ptr<Thumbnails>> preview_;
    runtime::TaskHandle<std::shared_ptr<InputColorCatalog>> inputColorCatalogTask_;
    std::shared_ptr<InputColorCatalog> inputColorCatalog_;
    struct ImageParts final {
        core::Sha256Digest digest;
        std::vector<media::ImagePartInfo> parts;
    };
    std::map<document::AssetId, ImageParts> imageParts_;
    std::map<document::AssetId, runtime::TaskHandle<std::shared_ptr<ImageParts>>> imagePartTasks_;
    std::optional<document::ColorSettings> inputColorCatalogRequestedSettings_;
    std::string inputColorCatalogRequestedWorking_;
    bool inputColorCatalogPending_ = false;
    std::optional<document::Snapshot> base_;
    Previews previews_;
    std::map<document::NodeId, Preview> nodePreviews_;
    std::map<std::string, QImage> thumbnailCache_;
    Waveforms waveforms_;
    AudioBuffers audioBuffers_;
    AudioBuffers audioDeliveryBuffers_;
    std::map<document::AssetId, std::weak_ptr<const media::audio::AudioBuffer>>
        adoptedAudioBuffers_;
    std::size_t proxyCacheBytes_ = 0;
    std::size_t decodedAudioBytes_ = 0;
    QByteArray dragToken_;
    QTimer freshnessTimer_;
    bool busy_ = false;
    bool previewPending_ = false;
    bool previewDirty_ = false;
    bool cachePurgeGate_ = false;
    bool retryAudioAfterCapacitySample_ = false;
    bool assetsDirty_ = true;
    bool previewIncludesAssets_ = false;
    bool timeDependentNodes_ = true;
    std::uint64_t contentGeneration_ = 0;
    std::uint64_t previewContentGeneration_ = 0;
};
} // namespace bloom::ui
