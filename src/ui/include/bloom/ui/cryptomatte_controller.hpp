#pragma once

// #364 Cryptomatte: the UI side of the Properties layer picker and the Viewer eyedropper. Both
// reads run on a worker (compiler + identity-verified probe/decode through injected providers);
// the controller only edits the node's matte list through the session's command path.

#include <QObject>
#include <QString>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/document.hpp>
#include <bloom/runtime/cryptomatte_inspection.hpp>
#include <bloom/runtime/evaluation.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace bloom::ui {

class CompositionSession;
class TaskUiBridge;

class CryptomatteController final : public QObject {
    Q_OBJECT

  public:
    using LayerProvider = std::function<runtime::CryptomatteLayerListing(
        const document::Snapshot&, document::CompositionId, document::NodeId, core::RationalTime,
        const runtime::EvaluationColorIntent&, const runtime::CancellationToken&)>;
    using PickProvider = std::function<runtime::CryptomattePick(
        const document::Snapshot&, document::CompositionId, document::NodeId, core::RationalTime,
        const runtime::EvaluationColorIntent&, const std::optional<runtime::ChannelInspection>&,
        std::int64_t, std::int64_t, const runtime::CancellationToken&)>;
    enum class PickAction : std::uint8_t { Add, Remove };

    CryptomatteController(CompositionSession& session, runtime::TaskScheduler& scheduler,
                          TaskUiBridge& bridge, LayerProvider layers, PickProvider pick,
                          QObject* parent = nullptr);
    ~CryptomatteController() override;

    // The Cryptomatte types in `node`'s channel set at the current snapshot; nullptr until the
    // worker listing for this revision arrives (requestLayers starts it; changed() announces it).
    void requestLayers(document::NodeId node);
    [[nodiscard]] const std::vector<std::string>* layers(document::NodeId node) const;
    [[nodiscard]] QString layerDiagnostic(document::NodeId node) const;

    // Reads the object under frame pixel (x, y) of `node`'s channel set on a worker and adds it to
    // (or removes it from) the node's matte list as one undoable edit. `inspection` is the plan
    // the Viewer shows (nullopt: the composition); the worker reads the node's source frame at the
    // playhead mapped through that plan's layer time.
    void pick(document::NodeId node, std::int64_t x, std::int64_t y, PickAction action,
              std::optional<runtime::ChannelInspection> inspection = std::nullopt);
    // Reports a pick that never reached the worker (the cursor is outside the source).
    void reportPick(QString message);
    [[nodiscard]] bool pickPending() const noexcept { return pickTask_.has_value(); }
    [[nodiscard]] const QString& lastPickMessage() const noexcept { return lastPickMessage_; }

    // The node's matte list, and one edit of it (no-op when unchanged).
    [[nodiscard]] std::vector<std::string> mattes(document::NodeId node) const;
    bool setMattes(document::NodeId node, const std::vector<std::string>& mattes,
                   const QString& label);

    void cancel();

  signals:
    void changed();

  private:
    void poll();

    struct Listing final {
        document::Revision revision;
        runtime::CryptomatteLayerListing value;
    };
    struct PendingListing final {
        document::Revision revision;
        runtime::TaskHandle<std::shared_ptr<const runtime::CryptomatteLayerListing>> handle;
    };
    struct PendingPick final {
        document::NodeId node;
        PickAction action;
        runtime::TaskHandle<std::shared_ptr<const runtime::CryptomattePick>> handle;
    };

    CompositionSession& session_;
    runtime::TaskScheduler& scheduler_;
    TaskUiBridge& bridge_;
    LayerProvider layerProvider_;
    PickProvider pickProvider_;
    std::map<document::NodeId, Listing> listings_;
    std::map<document::NodeId, PendingListing> listingTasks_;
    std::optional<PendingPick> pickTask_;
    QString lastPickMessage_;
};

} // namespace bloom::ui
