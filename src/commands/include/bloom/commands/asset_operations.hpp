#pragma once
#include <bloom/commands/operation.hpp>
#include <bloom/core/color.hpp>
#include <filesystem>
#include <functional>
#include <vector>

namespace bloom::commands {
// Construct on an import worker: preparation probes, hashes and scans. apply only copies the
// prepared records into a draft, so publishing the command never performs media I/O.
class ImportAssets final : public Operation {
  public:
    ImportAssets(const std::vector<std::filesystem::path>& paths,
                 const std::filesystem::path& projectDirectory,
                 const std::function<bool()>& cancel = {},
                 const std::function<void(std::uint64_t, std::uint64_t)>& progress = {});
    [[nodiscard]] std::string_view typeId() const noexcept override { return "bloom.asset.import"; }
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;
    [[nodiscard]] const std::vector<document::AssetRecord>& prepared() const noexcept {
        return assets_;
    }
    [[nodiscard]] const std::string& diagnostic() const noexcept { return diagnostic_; }

  private:
    std::vector<document::AssetRecord> assets_;
    std::string diagnostic_;
};
class RelinkAsset final : public Operation {
  public:
    RelinkAsset(document::AssetId id, std::filesystem::path path,
                const std::filesystem::path& projectDirectory,
                const std::function<bool()>& cancel = {});
    [[nodiscard]] std::string_view typeId() const noexcept override { return "bloom.asset.relink"; }
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::AssetId id_;
    ImportAssets prepared_;
};
class RemoveAsset final : public Operation {
  public:
    explicit RemoveAsset(document::AssetId id) : id_(id) {}
    [[nodiscard]] std::string_view typeId() const noexcept override { return "bloom.asset.remove"; }
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::AssetId id_;
};
class SetCompositionBackgroundColor final : public Operation {
  public:
    SetCompositionBackgroundColor(document::CompositionId composition, core::Color4d color)
        : composition_(composition), color_(color) {}
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.composition.set-background-color";
    }
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::CompositionId composition_;
    core::Color4d color_;
};
class AddImageLayer final : public Operation {
  public:
    AddImageLayer(document::CompositionId composition, document::AssetId asset)
        : composition_(composition), asset_(asset) {}
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.layer.add-image";
    }
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::CompositionId composition_;
    document::AssetId asset_;
};
} // namespace bloom::commands
