#pragma once

#include <bloom/commands/operation.hpp>
#include <bloom/document/data_block.hpp>

#include <string>
#include <vector>

namespace bloom::commands {

class CreateDataBlock final : public Operation {
  public:
    explicit CreateDataBlock(document::DataBlockRecord block) : block_(std::move(block)) {}
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.data-block.create";
    }
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::DataBlockRecord block_;
};

class ReplaceDataBlockPayload final : public Operation {
  public:
    ReplaceDataBlockPayload(document::DataBlockRecordId id, document::DataBlockPayload payload)
        : id_(id), payload_(std::move(payload)) {}
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.data-block.replace-payload";
    }
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::DataBlockRecordId id_;
    document::DataBlockPayload payload_;
};

class RemoveDataBlock final : public Operation {
  public:
    explicit RemoveDataBlock(document::DataBlockRecordId id) : id_(id) {}
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.data-block.remove";
    }
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::DataBlockRecordId id_;
};

class RelinkDataBlock final : public Operation {
  public:
    RelinkDataBlock(document::DataBlockRecordId id, document::AssetLocator locator)
        : id_(id), locator_(std::move(locator)) {}
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.data-block.relink";
    }
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::DataBlockRecordId id_;
    document::AssetLocator locator_;
};

class SetDataBlockTags final : public Operation {
  public:
    SetDataBlockTags(document::DataBlockRecordId id, std::vector<std::string> tags)
        : id_(id), tags_(std::move(tags)) {}
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.data-block.set-tags";
    }
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::DataBlockRecordId id_;
    std::vector<std::string> tags_;
};

} // namespace bloom::commands
