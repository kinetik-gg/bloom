#include <bloom/commands/data_block_operations.hpp>

#include <algorithm>
#include <ranges>

namespace bloom::commands {
namespace {
OperationResult targetMissing() {
    return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                     "Data block does not exist");
}
bool validTags(const std::vector<std::string>& tags) {
    if (tags.size() > 64 || !std::ranges::is_sorted(tags))
        return false;
    return std::ranges::adjacent_find(tags) == tags.end() &&
           std::ranges::all_of(tags,
                               [](const auto& tag) { return !tag.empty() && tag.size() <= 128; });
}
} // namespace

OperationResult CreateDataBlock::apply(document::Draft& draft) const {
    auto block = block_;
    if (!std::holds_alternative<document::DataBlockRecordId>(block.id)) {
        const auto id = draft.ids().allocateDataBlock();
        if (!id)
            return OperationResult::rejected(OperationIssueCode::Unsupported,
                                             "Data block ID space is exhausted");
        block.id = *id;
    } else if (!std::get<document::DataBlockRecordId>(block.id).isValid()) {
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Data block ID must be valid");
    }
    if (document::isMediaDataBlockKind(block.kind))
        return OperationResult::rejected(OperationIssueCode::Unsupported,
                                         "Media blocks are owned by AssetRecord");
    if (!validTags(block.tags) || !block.validate(&draft.project()).ok())
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Invalid data block payload or metadata");
    const auto id = std::get<document::DataBlockRecordId>(block.id);
    if (!draft.project().addDataBlock(std::move(block)))
        return OperationResult::rejected(OperationIssueCode::DuplicateId,
                                         "Data block ID is already present");
    return OperationResult::applied({{"dataBlock", id}});
}

OperationResult ReplaceDataBlockPayload::apply(document::Draft& draft) const {
    auto* block = draft.project().findDataBlock(id_);
    if (!block)
        return targetMissing();
    document::DataBlockRecord candidate = *block;
    candidate.payload = payload_;
    if (!candidate.validate(&draft.project()).ok())
        return OperationResult::rejected(
            OperationIssueCode::InvalidValue,
            "Payload is incompatible with the data block kind or bounds");
    block->payload = payload_;
    return OperationResult::applied({{"dataBlock", id_}});
}

OperationResult RemoveDataBlock::apply(document::Draft& draft) const {
    if (!draft.project().removeDataBlock(id_))
        return targetMissing();
    return OperationResult::applied({{"dataBlock", id_}});
}

OperationResult RelinkDataBlock::apply(document::Draft& draft) const {
    auto* block = draft.project().findDataBlock(id_);
    if (!block)
        return targetMissing();
    if (!std::holds_alternative<document::AssetLocator>(block->provenance.source))
        return OperationResult::rejected(OperationIssueCode::Unsupported,
                                         "Only locator-backed data blocks can be relinked");
    auto candidate = *block;
    candidate.provenance.source = locator_;
    if (!candidate.validate(&draft.project()).ok())
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Invalid relinked locator");
    block->provenance.source = locator_;
    return OperationResult::applied({{"dataBlock", id_}});
}

OperationResult SetDataBlockTags::apply(document::Draft& draft) const {
    auto* block = draft.project().findDataBlock(id_);
    if (!block)
        return targetMissing();
    if (!validTags(tags_))
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Tags must be sorted, unique, and bounded");
    block->tags = tags_;
    return OperationResult::applied({{"dataBlock", id_}});
}

} // namespace bloom::commands
