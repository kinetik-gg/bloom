// The production runner seam: ExportJobRunner and the platform destination facts.
#include <bloom/host/export_job_runner.hpp>
#include <bloom/platform/destination_facts.hpp>

#include <algorithm>
#include <array>

namespace bloom::host {
namespace {

class ProductionBackend final : public ExportRunBackend {
  public:
    std::unique_ptr<ExportRun> start(ExportJobRunnerDependencies dependencies,
                                     ExportJobRunRequest request) override {
        return std::make_unique<ExportJobRunner>(std::move(dependencies), std::move(request));
    }

    ExportDestinationSample sampleDestination(const ExportDestinationSampleRequest& key) override {
        ExportDestinationSample sample;
        if (key.forRun) {
            std::error_code created;
            std::filesystem::create_directories(key.firstPath.parent_path(), created);
            (void)platform::removeStaleStagedArtifacts(key.firstPath);
            if (key.lastPath != key.firstPath)
                (void)platform::removeStaleStagedArtifacts(key.lastPath);
        }
        sample.firstPath = key.firstPath;
        sample.lastPath = key.lastPath;
        sample.fileCount = key.fileCount;
        const auto folder = key.firstPath.parent_path();
        const auto facts = platform::destinationFolderFacts(folder);
        sample.folderExists = facts.exists;
        sample.folderWritable = facts.writable;
        sample.freeBytes = facts.availableBytes;
        if (!facts.exists)
            return sample;
        // The files this output would write that already exist: a single file, or every file in
        // the folder that sorts between the sequence's first and last names with the same length
        // and extension (sequence naming is fixed-width and ascending).
        core::Sha256Hasher hasher;
        std::error_code error;
        const auto record = [&](const std::filesystem::path& path) {
            const auto size = std::filesystem::file_size(path, error);
            const auto time = std::filesystem::last_write_time(path, error);
            const auto name = path.filename().string();
            (void)hasher.update(std::as_bytes(std::span(name.data(), name.size())));
            const std::array<std::uint64_t, 2> values{
                static_cast<std::uint64_t>(size),
                static_cast<std::uint64_t>(time.time_since_epoch().count())};
            (void)hasher.update(std::as_bytes(std::span(values)));
            ++sample.existing;
        };
        if (key.fileCount <= 1 || key.firstPath == key.lastPath) {
            if (std::filesystem::is_regular_file(key.firstPath, error))
                record(key.firstPath);
        } else {
            const auto first = key.firstPath.filename().string();
            const auto last = key.lastPath.filename().string();
            std::vector<std::filesystem::path> matches;
            for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
                const auto name = entry.path().filename().string();
                if (name.size() == first.size() && name >= first && name <= last &&
                    entry.path().extension() == key.firstPath.extension() &&
                    entry.is_regular_file(error))
                    matches.push_back(entry.path());
            }
            std::ranges::sort(matches);
            for (const auto& path : matches)
                record(path);
        }
        if (sample.existing > 0)
            sample.fingerprint = hasher.finalize();
        return sample;
    }
};

} // namespace

std::shared_ptr<ExportRunBackend> makeProductionExportRunBackend() {
    return std::make_shared<ProductionBackend>();
}

} // namespace bloom::host
