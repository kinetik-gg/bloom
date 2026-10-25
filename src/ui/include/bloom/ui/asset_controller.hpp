#pragma once
#include <QByteArray>
#include <QImage>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <atomic>
#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/commands/asset_operations.hpp>
#include <bloom/document/document.hpp>
#include <bloom/media/audio/audio.hpp>
#include <bloom/media/image.hpp>
#include <bloom/runtime/audio_stream_key.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

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
    // One item a batch import skipped, with its plain reason (#373).
    struct ImportSkip final {
        QString file;
        QString reason;
    };
    // How a batch import ended (#377). Imported items were committed as one undoable transaction.
    struct ImportReport final {
        std::size_t imported = 0;
        std::vector<ImportSkip> skipped;
        // Items never finished because the artist cancelled, including the one in progress.
        std::size_t notImported = 0;
        bool cancelled = false;
        // The import stopped for the application (closing the project or quitting); nothing
        // was added and no summary is due.
        bool discarded = false;
        // Why nothing at all was added when no single file is to blame; empty otherwise.
        QString failure;
        // Every asset the committed items resolved to, in import order: the ones just added
        // and the ones the project already held.
        std::vector<document::AssetId> assets;
        // What the import's follow-up did with them ("Added 2 layers"), for the status line and
        // the summary; empty when there was none.
        QString followUp;
    };
    // Runs once the import has committed, with ImportReport::assets, as its own undoable step
    // (a drop on the Timeline adds layers). Returns a plain sentence for the report, or nothing.
    using ImportFollowUp = std::function<QString(const std::vector<document::AssetId>&)>;
    // Where the running batch import stands, read from the import worker's latest report (see
    // commands::ImportProgress: items, work units, and the sequence member being read).
    struct ImportProgressState final {
        // Searching folders or listing sequence members: the work total is not known yet.
        bool finding = true;
        std::size_t completed = 0;
        std::size_t total = 0;
        std::uint64_t workDone = 0;
        std::uint64_t workTotal = 0;
        // The current file's name (or folder's, while finding).
        QString current;
        std::uint64_t found = 0;
        bool findingFrames = false;
        std::uint64_t framesRead = 0;
        std::uint64_t frames = 0;
        bool stopping = false;
        // The thread preparing the import, so tests can prove it is not the interface thread.
        std::optional<std::thread::id> workerThread;
    };
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
    // Imports into the current import folder (see setImportFolder).
    void importFiles(const QStringList& paths);
    // Imports into `folder`; nullopt is the Assets root. `followUp` runs after the commit.
    void importFiles(const QStringList& paths, std::optional<document::AssetFolderId> folder,
                     ImportFollowUp followUp = {});
    // Session state (#378): the Assets folder an import lands in when no drop target names one.
    // The Assets panel keeps it on the selected folder, or the folder of the selected item.
    void setImportFolder(std::optional<document::AssetFolderId> folder) noexcept {
        importFolder_ = folder;
    }
    [[nodiscard]] const std::optional<document::AssetFolderId>& importFolder() const noexcept {
        return importFolder_;
    }
    void relink(document::AssetId id, QWidget* parent = nullptr);
    // Reload Footage (#378): probes the asset's own file again on the import worker, as a relink to
    // the same path, so changed bytes update the record and unchanged ones leave it alone.
    void reload(document::AssetId id);
    // Where the asset's file (a sequence's first member) is, worked out from its locator and the
    // project folder without touching the filesystem. Empty when the locator names no file.
    [[nodiscard]] std::filesystem::path sourcePath(document::AssetId id) const;
    void remove(document::AssetId id);
    [[nodiscard]] bool busy() const noexcept { return busy_; }
    // True while a batch import (not a relink or reload) is being prepared.
    [[nodiscard]] bool importing() const noexcept { return busy_ && importShared_ != nullptr; }
    [[nodiscard]] ImportProgressState importProgress() const;
    // Cancel from the import dialog: the worker stops at its next safe point and the items
    // already prepared are still imported, as one undoable step. cancel() discards instead.
    void stopImport();
    // Test seam: runs on the import worker after each progress report, with that report.
    void
    setImportProgressHookForTesting(std::function<void(const commands::ImportProgress&)> hook) {
        importProgressHook_ = std::move(hook);
    }
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
    // The resolved input colour space by its plain name ("Linear Rec.709", "sRGB", or the
    // config's own name), with nothing added; empty until the worker has resolved it.
    [[nodiscard]] QString inputColorSpaceName(document::AssetId id) const;
    // True when Auto chose the space from the media tags (or assumed it).
    [[nodiscard]] bool inputColorSpaceAutomatic(document::AssetId id) const;
    // True when Auto resolved an untagged or partly tagged video by the size convention; the
    // warning is then the visible "Colour tags missing — assumed …" note.
    [[nodiscard]] bool inputColorSpaceAssumed(document::AssetId id) const;
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
    // The decoded buffer of one audio stream (0-based among the asset's audio streams).
    [[nodiscard]] std::shared_ptr<const media::audio::AudioBuffer>
    audioBuffer(document::AssetId id, std::uint32_t stream = 0) const;
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
    void importStarted();
    void importFinished(const bloom::ui::AssetController::ImportReport& report);

  private:
    using OperationHandle = std::shared_ptr<std::unique_ptr<commands::Operation>>;
    // Shared between a batch import's worker and this controller: the worker's latest progress
    // and final per-item outcome, and the artist's stop request.
    struct ImportShared final {
        mutable std::mutex mutex;
        commands::ImportProgress progress;
        std::optional<std::thread::id> workerThread;
        std::vector<commands::ImportFailure> failures;
        std::size_t notImported = 0;
        bool cancelled = false;
        std::atomic<bool> stop{false};
    };
    struct Preview {
        QImage image;
        std::string key;
        std::string inputColorSpaceName;
        std::string inputColorSpaceWarning;
        std::string audioDiagnostic;
        bool inputColorSpaceAutomatic = false;
        bool inputColorSpaceAssumed = false;
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
        std::map<runtime::AudioStreamKey, std::shared_ptr<const media::audio::AudioBuffer>>;
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
    void prepare(const QStringList& paths, document::AssetId relinkId = {},
                 std::optional<document::AssetFolderId> folder = {});
    void refresh();
    void refreshNodeThumbnails();
    void startThumbnails();
    void poll();
    // One status notice for an import job's diagnostics; the Jobs panel keeps each one.
    void emitImportFailures(const std::vector<runtime::TaskDiagnostic>& issues);
    void finishImport(runtime::TaskResult<OperationHandle>& result);
    CompositionSession& session_;
    std::optional<document::AssetFolderId> importFolder_;
    // Reload Footage requests waiting for the import worker, run one after another.
    std::deque<document::AssetId> pendingReloads_;
    std::shared_ptr<ImportShared> importShared_;
    ImportFollowUp importFollowUp_;
    std::function<void(const commands::ImportProgress&)> importProgressHook_;
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
    std::map<runtime::AudioStreamKey, std::weak_ptr<const media::audio::AudioBuffer>>
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
