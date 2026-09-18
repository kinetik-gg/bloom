#include "native.hpp"

#include <bloom/core/sha256.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <nanobind/stl/string.h>

#include <array>
#include <fstream>
#include <stdexcept>

namespace bloom::scripting::python {
namespace {

std::string digestFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    core::Sha256Hasher hasher;
    std::array<char, 65536> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        if (!hasher.update(std::as_bytes(
                std::span(buffer.data(), static_cast<std::size_t>(input.gcount()))))) {
            throw std::runtime_error("Rendered file exceeds digest limits");
        }
    }
    if (!input.eof()) {
        throw std::runtime_error("Could not read rendered file");
    }
    const auto hex = hasher.finalize().toLowercaseHex();
    return "sha256:" + std::string(hex.data(), hex.size());
}

} // namespace

nb::dict NativeHost::render(const std::uint64_t composition, const std::uint64_t first,
                            const std::uint64_t last, const bool range, const std::string& preset,
                            const std::string& destination, const std::uint64_t cancellationTask) {
    output::OutputPresetV1 selected;
    if (preset == "PngRgba8SrgbV1") {
        selected = output::OutputPresetV1::PngRgba8SrgbV1;
    } else if (preset == "FlatExrRgba32fLinRec709SceneV1") {
        selected = output::OutputPresetV1::FlatExrRgba32fLinRec709SceneV1;
    } else {
        throw nb::value_error("Unknown render preset");
    }
    if (first > last || last - first >= 10000 || destination.empty()) {
        throw nb::value_error("Invalid frame range or destination");
    }
    const auto access = lockSession();
    RenderResult result;
    std::vector<std::pair<std::string, std::string>> files;
    {
        const nb::gil_scoped_release release;
        runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
        RenderRequest request{.composition = document::CompositionId::fromRaw(composition),
                              .frame = range ? std::nullopt : std::optional(first),
                              .range = range ? std::optional(std::pair(first, last)) : std::nullopt,
                              .preset = selected,
                              .destination = destination,
                              .cancelled = [this, cancellationTask] {
                                  if (cancellationRequested() || !session_->isValid())
                                      return true;
                                  const auto task = scheduler_.snapshot(
                                      runtime::TaskId::fromRaw(cancellationTask));
                                  return task && task->cancellationRequested;
                              }};
        result = Render::run(*session_, scheduler_, compiler, std::move(request));
        if (result.succeeded) {
            if (range) {
                for (auto index = first; index <= last; ++index) {
                    const auto path =
                        host::FrameRangeRunnerV1::sequenceFramePath(destination, index, last);
                    files.emplace_back(path.string(), digestFile(path));
                }
            } else {
                files.emplace_back(destination, digestFile(destination));
            }
        }
    }
    if (!result.succeeded) {
        throw std::runtime_error(result.diagnostic);
    }
    nb::dict row;
    row["published_frames"] = result.publishedFrames;
    row["preservation_report"] = result.preservationReport;
    nb::list paths;
    for (const auto& [path, digest] : files) {
        nb::dict file;
        file["path"] = path;
        file["digest"] = digest;
        paths.append(file);
    }
    row["files"] = paths;
    return row;
}

} // namespace bloom::scripting::python
