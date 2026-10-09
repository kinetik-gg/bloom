#pragma once

#include <bloom/render/packed_host_retirement.hpp>

namespace bloom::runtime {
// Runtime exports the explicit composition-root lifecycle for host/scripting coordination.
// Render owns the neutral worker/ledger implementation. This alias preserves the exact owner,
// exit proof and policy injection identity; it adds no wrapper state or second lifecycle.
using PackedHostRetirementOwner = render::PackedHostRetirementOwner;
} // namespace bloom::runtime
