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
#include <bloom/runtime/task_scheduler.hpp>
#include <map>

class QWidget;
namespace bloom::media::cache {
class MediaDiskCache;
} // namespace bloom::media::cache
namespace bloom::ui {
class CompositionSession;
class ProjectHost;
class TaskUiBridge;
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
                    QObject* parent = nullptr);
    ~AssetController() override;
    void requestImport(QWidget* parent = nullptr);
    void importFiles(const QStringList& paths);
    void relink(document::AssetId id, QWidget* parent = nullptr);
    void remove(document::AssetId id);
    [[nodiscard]] bool busy() const noexcept { return busy_; }
    [[nodiscard]] bool missing(document::AssetId id) const;
    [[nodiscard]] QImage thumbnail(document::AssetId id) const;
    [[nodiscard]] QImage nodeThumbnail(document::NodeId id) const;
    [[nodiscard]] QString inputColorSpaceDisplay(document::AssetId id) const;
    [[nodiscard]] QString inputColorSpaceWarning(document::AssetId id) const;
    void requestInputColorSpaces();
    [[nodiscard]] const std::vector<color::OcioColorSpaceInfo>* inputColorSpaces() const noexcept;
    [[nodiscard]] QString inputColorCatalogDiagnostic() const;
    [[nodiscard]] std::shared_ptr<const media::audio::WaveformSummary>
    waveform(document::AssetId id) const;
    [[nodiscard]] std::shared_ptr<const media::audio::AudioBuffer>
    audioBuffer(document::AssetId id) const;
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
        bool inputColorSpaceAutomatic = false;
        bool missing = false;
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
        // CACHEFIX-1 accounting: what these two maps actually hold, carried back from the worker
        // so the controller can report it without walking every entry on the interface thread.
        std::size_t proxyBytes = 0;
        std::size_t audioBytes = 0;
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
    CompositionSession& session_;
    ProjectHost& host_;
    runtime::TaskScheduler& scheduler_;
    TaskUiBridge& bridge_;
    media::cache::MediaDiskCache* mediaDiskCache_ = nullptr;
    runtime::TaskHandle<OperationHandle> import_;
    runtime::TaskHandle<std::shared_ptr<Thumbnails>> preview_;
    runtime::TaskHandle<std::shared_ptr<InputColorCatalog>> inputColorCatalogTask_;
    std::shared_ptr<InputColorCatalog> inputColorCatalog_;
    std::optional<document::ColorSettings> inputColorCatalogRequestedSettings_;
    std::string inputColorCatalogRequestedWorking_;
    bool inputColorCatalogPending_ = false;
    std::optional<document::Snapshot> base_;
    Previews previews_;
    std::map<document::NodeId, Preview> nodePreviews_;
    std::map<std::string, QImage> thumbnailCache_;
    Waveforms waveforms_;
    AudioBuffers audioBuffers_;
    std::size_t proxyCacheBytes_ = 0;
    std::size_t decodedAudioBytes_ = 0;
    QByteArray dragToken_;
    QTimer freshnessTimer_;
    bool busy_ = false;
    bool previewPending_ = false;
    bool previewDirty_ = false;
    bool cachePurgeGate_ = false;
    bool assetsDirty_ = true;
    bool previewIncludesAssets_ = false;
    bool timeDependentNodes_ = true;
    std::uint64_t contentGeneration_ = 0;
    std::uint64_t previewContentGeneration_ = 0;
};
} // namespace bloom::ui
