#pragma once

#include <bloom/host/gpu_export_provider.hpp>

namespace bloom::host::detail {
// Private CPU-worker fixture observation; no public cache/root authority API.
struct GpuExportProviderTestAccess final {
    [[nodiscard]] static runtime::GpuSceneMediaContext mediaContext(const GpuExportProvider&);
};
} // namespace bloom::host::detail
