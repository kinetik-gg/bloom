#pragma once
#include <QImage>
#include <QObject>
#include <QStringList>
#include <bloom/commands/asset_operations.hpp>
#include <bloom/document/document.hpp>
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
    [[nodiscard]] std::filesystem::path baseDirectory() const;
    void cancel();
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
    void prepare(const QStringList& paths, document::AssetId relinkId = {});
    void refresh();
    void poll();
    CompositionSession& session_;
    ProjectHost& host_;
    runtime::TaskScheduler& scheduler_;
    TaskUiBridge& bridge_;
    runtime::TaskHandle<OperationHandle> import_;
    runtime::TaskHandle<std::shared_ptr<Previews>> preview_;
    std::optional<document::Snapshot> base_;
    Previews previews_;
    bool busy_ = false;
    bool previewPending_ = false;
};
} // namespace bloom::ui
