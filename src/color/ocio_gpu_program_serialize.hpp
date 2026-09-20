#pragma once

// Bounded, versioned binary codec for the portable render::OcioGpuProgramDesc. It carries the
// shader text, reflected resources, uniforms, UBO snapshot, and OCIO provenance across the
// bloom-color-worker IPC boundary. The producer-owned digests and content identity are deliberately
// NOT transported: the host recomputes and validates them, so a compromised or buggy helper can
// never smuggle an unchecked identity into the accepted program.
//
// Integers are little-endian and explicit; text is a u32 byte length followed by raw bytes. Every
// count and length is checked against the caller's limits and the remaining input before any
// allocation, and a trailing-byte or truncated stream is a typed refusal.

#include "ocio_gpu_program_transport.hpp"

#include <bloom/render/ocio_gpu_program.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace bloom::color::detail {

inline constexpr std::uint32_t kOcioGpuProgramSerializationMagic = 0x474D4C42U; // "BLMG"
inline constexpr std::uint32_t kOcioGpuProgramSerializationVersion = 1U;

// Returns nullopt when the descriptor cannot be represented (a field length exceeds u32) or when
// the encoded size would exceed `maxBytes`.
[[nodiscard]] std::optional<std::vector<std::byte>>
serializeOcioGpuProgram(const render::OcioGpuProgramDesc& program, std::uint64_t maxBytes);

// Structural decode only. On success the descriptor is fully populated with the transported fields;
// the caller must still recompute digests and run validateOcioGpuProgram before accepting it.
[[nodiscard]] OcioGpuProgramTransport
deserializeOcioGpuProgram(std::span<const std::byte> bytes,
                          const render::OcioGpuProgramLimits& limits) noexcept;

} // namespace bloom::color::detail
