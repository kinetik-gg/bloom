#include <QDir>
#include <QFileDialog>
#include <QUrl>
#include <algorithm>
#include <bloom/color/bloom_neutral_builtin.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/media/image.hpp>
#include <bloom/ui/asset_controller.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/project_host.hpp>
#include <bloom/ui/task_ui_bridge.hpp>
#include <cmath>

namespace bloom::ui {
namespace {
std::filesystem::path nativePath(const QString& text) {
#if defined(_WIN32)
    return std::filesystem::path(text.toStdWString());
#else
    return std::filesystem::path(text.toStdString());
#endif
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
                                 QObject* parent)
    : QObject(parent), session_(session), host_(host), scheduler_(scheduler), bridge_(bridge) {
    session_.setAssetController(this);
    connect(&bridge_, &TaskUiBridge::snapshotsPolled, this, &AssetController::poll);
    connect(&session_, &CompositionSession::snapshotChanged, this, &AssetController::refresh);
    connect(&host_, &ProjectHost::sessionReplaced, this, [this] {
        cancel();
        base_.reset();
        previews_.clear();
        refresh();
    });
    connect(this, &AssetController::diagnostic, &session_, &CompositionSession::commandRejected);
    refresh();
}
AssetController::~AssetController() {
    cancel();
    session_.setAssetController(nullptr);
}
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
void AssetController::requestImport(QWidget* parent) {
    const auto paths = QFileDialog::getOpenFileNames(
        parent, tr("Import Images"), {}, tr("Images (*.png *.jpg *.jpeg *.PNG *.JPG *.JPEG)"));
    if (!paths.empty())
        importFiles(paths);
}
void AssetController::importFiles(const QStringList& paths) { prepare(paths); }
void AssetController::relink(document::AssetId id, QWidget* parent) {
    const auto path = QFileDialog::getOpenFileName(parent, tr("Relink Image"), {},
                                                   tr("Images (*.png *.jpg *.jpeg)"));
    if (!path.isEmpty())
        prepare({path}, id);
}
void AssetController::remove(document::AssetId id) {
    commands::Transaction transaction("Remove Asset", session_.snapshot().revision());
    transaction.emplace<commands::RemoveAsset>(id);
    static_cast<void>(session_.executeTransaction(std::move(transaction)));
}
void AssetController::prepare(const QStringList& paths, document::AssetId relinkId) {
    if (busy_ || paths.empty())
        return;
    std::vector<std::filesystem::path> files;
    for (const auto& path : paths)
        files.push_back(nativePath(path));
    const auto directory = baseDirectory();
    base_ = session_.snapshot();
    auto submission = scheduler_.submit<OperationHandle>(
        runtime::TaskRequest(
            "Import images",
            {.kind = runtime::TaskOwnerKind::Application, .id = runtime::TaskOwnerId::fromRaw(1)},
            runtime::TaskPriority::Foreground, runtime::TaskExecutor::BlockingIo),
        [files = std::move(files), directory, relinkId](runtime::TaskContext& context) {
            auto result = std::make_shared<std::unique_ptr<commands::Operation>>();
            auto cancel = [&] { return context.isCancellationRequested(); };
            auto progress = [&](std::uint64_t done, std::uint64_t total) {
                context.reportProgress({.phase = "Importing images",
                                        .subphase = "Probing and scanning",
                                        .completed = done,
                                        .total = total});
            };
            if (relinkId.isValid())
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
        emit diagnostic(tr("Image import could not be scheduled"));
        return;
    }
    import_ = std::move(submission.handle);
    busy_ = true;
    bridge_.wake();
    emit activityChanged();
}
void AssetController::refresh() {
    if (previewPending_)
        preview_.cancel();
    const auto snapshot = session_.snapshot();
    const auto directory = baseDirectory();
    auto submission = scheduler_.submit<std::shared_ptr<Previews>>(
        runtime::TaskRequest(
            "Image thumbnails",
            {.kind = runtime::TaskOwnerKind::Application, .id = runtime::TaskOwnerId::fromRaw(1)},
            runtime::TaskPriority::Background, runtime::TaskExecutor::BlockingIo),
        [snapshot, directory, cached = previews_](runtime::TaskContext& context) {
            auto results = std::make_shared<Previews>();
            for (const auto& asset : snapshot.project().assets()) {
                if (context.isCancellationRequested())
                    return runtime::TaskResult<std::shared_ptr<Previews>>::cancelled();
                const auto& locator = asset.kind == document::AssetKind::Sequence
                                          ? asset.manifest.members.front().locator
                                          : asset.locator;
                const auto path =
                    media::resolveImagePath(locator.path, locator.relinkHint, directory);
                const auto digest = asset.kind == document::AssetKind::Sequence
                                        ? asset.manifest.members.front().contentDigest
                                        : asset.contentDigest;
                const auto hex = digest.toLowercaseHex();
                const auto configHex = color::kBloomNeutralV1ConfigDigest.toLowercaseHex();
                const auto key =
                    std::string(hex.data(), hex.size()) +
                    std::string(configHex.data(), configHex.size()) + locator.path +
                    std::to_string(static_cast<int>(asset.interpretation.colorSpace)) +
                    std::to_string(static_cast<int>(asset.interpretation.alphaAssociation));
                auto probe =
                    media::probeImage(path, [&] { return context.isCancellationRequested(); });
                Preview preview;
                preview.key = key;
                preview.missing = !probe.value || probe.value->contentDigest != digest;
                const auto found = cached.find(asset.id);
                if (!preview.missing && found != cached.end() && found->second.key == key &&
                    !found->second.image.isNull()) {
                    results->emplace(asset.id, found->second);
                    continue;
                }
                // At most 512 RGBA8 thumbnails (8 MiB); decoding remains bounded separately.
                if (preview.missing || results->size() >= 512) {
                    results->emplace(asset.id, std::move(preview));
                    continue;
                }
                const media::ImageInterpretation interpretation{
                    static_cast<media::ImageColorSpace>(asset.interpretation.colorSpace),
                    static_cast<media::ImageAlphaAssociation>(
                        asset.interpretation.alphaAssociation)};
                auto decoded = media::decodeImage(
                    path, interpretation, {}, [&] { return context.isCancellationRequested(); }, {},
                    media::kMaxImageStorageBytes, digest);
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
                results->emplace(asset.id, std::move(preview));
            }
            return runtime::TaskResult<std::shared_ptr<Previews>>::succeeded(std::move(results));
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
                if (!base_ || base_->project().id() != session_.snapshot().project().id() ||
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
            if (result->value().has_value() && *result->value()) {
                previews_ = std::move(**result->value());
                emit changed();
            }
        }
    }
}
} // namespace bloom::ui
