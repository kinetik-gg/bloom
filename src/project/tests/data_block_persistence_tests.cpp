#include "zip_container_test_support.hpp"

#include <bloom/core/rational_time.hpp>
#include <bloom/document/data_block.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/project/canonical_document.hpp>
#include <bloom/project/open_archive.hpp>
#include <bloom/project/project_io_memory.hpp>
#include <bloom/project/save_archive.hpp>

#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace bloom;

project::ProjectIoOperationMemory memory() {
    auto coordinator = project::ProjectIoMemoryCoordinator::create();
    if (!coordinator)
        throw std::runtime_error("memory coordinator");
    auto operation = coordinator->createOperation();
    if (!operation)
        throw std::runtime_error("memory operation");
    return std::move(*operation);
}

document::DataBlockRecord block(const std::uint64_t id, const document::DataBlockKind kind) {
    document::DataBlockRecord result;
    result.id = document::DataBlockRecordId::fromRaw(id);
    result.kind = kind;
    result.typeId = "bloom.test.data-block";
    result.schemaVersion = {1, 0};
    result.mediaType = "application/vnd.bloom.test-data-block";
    result.provenance.source = document::DataBlockProducer{"bloom.test", "1", {}};
    result.provenance.createdAt = "2026-09-17T00:00:00Z";
    return result;
}

int run() {
    auto initial =
        document::makeNewProject("Data Blocks", "Main", core::RationalTime::fromInteger(24));
    document::AssetRecord coverage;
    coverage.id = document::AssetId::fromRaw(100);
    coverage.locator = {"file", "project-relative", "media/mask.exr", "file:///media/mask.exr"};
    coverage.width = 1;
    coverage.height = 1;
    coverage.name = "Mask";
    if (!initial.project.addAsset(coverage)) {
        std::cerr << "coverage asset rejected\n";
        return 1;
    }

    auto curve = block(1, document::DataBlockKind::Curve);
    curve.payload = document::DataBlockCurve{0.0, 1.0, {0.0, 2.0, 4.0}};
    auto ramp = block(2, document::DataBlockKind::Ramp);
    ramp.payload =
        document::DataBlockRamp{{{0.0, {0.0, 0.0, 0.0, 1.0}}, {1.0, {1.0, 0.5, 0.0, 1.0}}}};
    auto table = block(3, document::DataBlockKind::Table);
    table.payload =
        document::DataBlockTable{{{"value", document::DataBlockValueKind::Float64, {1.0, 2.0}}}};
    auto points = block(4, document::DataBlockKind::PointSet);
    points.payload = document::DataBlockPointSet{2, {{1.0, 2.0, 0.0}, {3.0, 4.0, 0.0}}, {}};
    auto path = block(5, document::DataBlockKind::Path);
    path.payload = document::PathValue{{{{{0.0, 0.0}, std::nullopt, std::nullopt}}}, false};
    auto mask = block(6, document::DataBlockKind::Mask);
    mask.payload = document::DataBlockMask{coverage.id};
    auto analysis = block(7, document::DataBlockKind::Analysis);
    analysis.payload = document::OpaqueExtensionPayload{std::byte{0x01}, std::byte{0x02}};
    auto opaque = block(8, document::DataBlockKind::Opaque);
    opaque.payload = document::OpaqueExtensionPayload{std::byte{0x03}};

    std::size_t blockIndex = 0;
    for (auto* value : {&curve, &ramp, &table, &points, &path, &mask, &analysis, &opaque}) {
        if (!initial.project.addDataBlock(std::move(*value))) {
            std::cerr << "data block rejected " << blockIndex << '\n';
            return 1;
        }
        ++blockIndex;
    }
    const std::vector expected(initial.project.typedDataBlocks().begin(),
                               initial.project.typedDataBlocks().end());
    document::Document document(std::move(initial.project));
    const auto snapshot = document.snapshot();
    const auto settings = document::makeBloomNeutralColorSettingsV1({});
    std::array<char, 1024> payloadScratch{};
    std::array<std::size_t, 1024> sortScratch{};
    const project::CanonicalDocumentV1 documentRequest{.snapshot = &snapshot,
                                                       .colorSettings = &settings,
                                                       .payloadScratch = payloadScratch,
                                                       .sortScratch = sortScratch};
    auto archive = project::buildVerifiedSaveArchive({}, documentRequest, {}, memory());
    if (!archive) {
        std::cerr << "archive rejected at stage " << static_cast<int>(archive.failure()->stage())
                  << '\n';
        if (const auto* mismatch =
                archive.failure()->payloadAs<project::SaveArchiveVerificationMismatch>())
            std::cerr << "verification mismatch entry " << static_cast<int>(mismatch->entry)
                      << '\n';
        if (const auto* documentFailure =
                archive.failure()->payloadAs<project::SaveArchiveDocumentEncodingFailure>())
            std::cerr << "document encoding error " << static_cast<int>(documentFailure->error)
                      << '\n';
        if (const auto* requirements =
                archive.failure()->payloadAs<project::SaveArchiveRequirementsFailure>())
            for (const auto& issue : requirements->validation.issues())
                std::cerr << issue.path << ": " << issue.message << '\n';
        if (const auto* decode =
                archive.failure()->payloadAs<project::SaveArchiveDocumentDecodeFailure>())
            std::cerr << "document decode error " << static_cast<int>(decode->error) << " path "
                      << decode->path.view() << '\n';
        return 1;
    }
    auto opened = project::openProjectArchive(archive.archive()->bytes(), {}, memory());
    if (opened.outcome() != project::OpenArchiveOutcome::Opened) {
        std::cerr << "archive did not open\n";
        return 1;
    }
    auto openedArchive = std::move(opened).takeOpened();
    const auto restored = openedArchive.document->snapshot();
    if (openedArchive.schemaMinor != 20 ||
        restored.project().typedDataBlocks().size() != expected.size())
        return 1;
    for (std::size_t index = 0; index < expected.size(); ++index)
        if (restored.project().typedDataBlocks()[index] != expected[index]) {
            std::cerr << "data block mismatch " << index << '\n';
            return 1;
        }
    return 0;
}
} // namespace

int main() {
    try {
        return run();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
