#pragma once

// #380: the Assets selection header shows a composition as a small rendered frame. The frame is
// prepared on a worker through the composition preview pipeline (compile, evaluate, display
// transform), at a small proxy extent, and kept per composition and document revision. The
// interface thread only reads the published QImage.

#include <QImage>
#include <QObject>
#include <bloom/document/document.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/composition_preview_controller.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <thread>

namespace bloom::ui {

class CompositionSession;
class TaskUiBridge;

class CompositionThumbnailController final : public QObject {
    Q_OBJECT

  public:
    // The longest edge of a prepared thumbnail, in device pixels: twice the header's thumbnail
    // cell, so it stays sharp at a device pixel ratio of two.
    static constexpr int kMaximumEdge = 144;
    // Compositions kept; the oldest entry is dropped beyond this.
    static constexpr std::size_t kCapacity = 64;

    // `preparation` is the composition preview pipeline the Viewer analysis also uses; `byteLimit`
    // the pixel storage one preparation may hold.
    CompositionThumbnailController(CompositionSession& session, runtime::TaskScheduler& scheduler,
                                   TaskUiBridge& bridge, PreviewPreparationFunction preparation,
                                   std::size_t byteLimit, QObject* parent = nullptr);
    ~CompositionThumbnailController() override;

    // The latest frame of `composition`: the current revision's when it is ready, otherwise the
    // newest one kept (null when none) while the current revision is prepared. Never evaluates on
    // the calling thread.
    [[nodiscard]] QImage thumbnail(document::CompositionId composition);
    // Whether thumbnail() shows the current revision.
    [[nodiscard]] bool current(document::CompositionId composition) const;
    void cancel();

    // Test seams: preparations submitted so far, and the thread the last one ran on.
    [[nodiscard]] std::uint64_t submissionCount() const noexcept { return submissions_; }
    [[nodiscard]] std::optional<std::thread::id> lastWorkerThread() const noexcept {
        return lastWorkerThread_;
    }

  signals:
    void changed();

  private:
    struct Prepared final {
        QImage image;
        std::thread::id thread;
    };
    struct Entry final {
        document::Revision revision;
        QImage image;
        std::uint64_t used = 0;
    };
    struct Pending final {
        document::Revision revision;
        runtime::TaskHandle<std::shared_ptr<const Prepared>> handle;
    };
    void poll();
    void request(document::CompositionId composition);

    CompositionSession& session_;
    runtime::TaskScheduler& scheduler_;
    TaskUiBridge& bridge_;
    PreviewPreparationFunction preparation_;
    std::size_t byteLimit_;
    document::ProjectId project_{};
    std::map<document::CompositionId, Entry> entries_;
    std::map<document::CompositionId, Pending> pending_;
    std::map<document::CompositionId, document::Revision> attempted_;
    std::uint64_t submissions_ = 0;
    std::uint64_t uses_ = 0;
    std::optional<std::thread::id> lastWorkerThread_;
};

} // namespace bloom::ui
