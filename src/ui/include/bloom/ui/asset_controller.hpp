#pragma once
#include <QByteArray>
#include <QImage>
#include <QObject>
#include <QStringList>
#include <bloom/commands/asset_operations.hpp>
#include <bloom/document/document.hpp>
#include <bloom/media/audio/audio.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <map>

class QWidget;
namespace bloom::ui {
class CompositionSession;
class ProjectHost;
class TaskUiBridge;
class AssetController final : public QObject {
    Q_OBJECT
  public:
    AssetController(CompositionSession& session, ProjectHost& host,
                    runtime::TaskScheduler& scheduler, TaskUiBridge& bridge,
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
    [[nodiscard]] std::shared_ptr<const media::audio::WaveformSummary>
    waveform(document::AssetId id) const;
    [[nodiscard]] std::shared_ptr<const media::audio::AudioBuffer>
    audioBuffer(document::AssetId id) const;
    [[nodiscard]] std::filesystem::path baseDirectory() const;
    void cancel();
    [[nodiscard]] bool acceptsEdits() const;
    [[nodiscard]] QByteArray dragToken() const { return dragToken_; }
  signals:
    void changed();
    void activityChanged();
    void diagnostic(QString message);

  private:
    using OperationHandle = std::shared_ptr<std::unique_ptr<commands::Operation>>;
    struct Preview {
        QImage image;
        std::string key;
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
    };
    void prepare(const QStringList& paths, document::AssetId relinkId = {});
    void refresh();
    void poll();
    CompositionSession& session_;
    ProjectHost& host_;
    runtime::TaskScheduler& scheduler_;
    TaskUiBridge& bridge_;
    runtime::TaskHandle<OperationHandle> import_;
    runtime::TaskHandle<std::shared_ptr<Thumbnails>> preview_;
    std::optional<document::Snapshot> base_;
    Previews previews_;
    std::map<document::NodeId, Preview> nodePreviews_;
    std::map<std::string, QImage> thumbnailCache_;
    Waveforms waveforms_;
    AudioBuffers audioBuffers_;
    QByteArray dragToken_;
    bool busy_ = false;
    bool previewPending_ = false;
    bool previewDirty_ = false;
};
} // namespace bloom::ui
