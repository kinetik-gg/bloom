#include <bloom/runtime/memory_budget_ledger.hpp>
#include <bloom/runtime/memory_headroom.hpp>

#include <cstdlib>
#include <iostream>
#include <source_location>

namespace {
int failures = 0;
void expect(const bool condition, const char* message,
            const std::source_location location = std::source_location::current()) {
    if (!condition) {
        ++failures;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }
}
} // namespace

int main() {
    using namespace bloom::runtime;
    constexpr std::uint64_t gib = 1024ULL * 1024 * 1024;
    expect(defaultHeadroomForCapacity(64 * gib) == 64 * gib / 10 &&
               defaultHeadroomForCapacity(24 * gib) == 24 * gib / 10 &&
               defaultHeadroomForCapacity(999) == 99,
           "the default headroom is exactly 10% of the protected capacity, with no floor");
    const auto physical = physicalMemoryBytes();
    expect(physical == 0 || defaultRamHeadroomBytes() <= defaultHeadroomForCapacity(physical),
           "the RAM default is 10% of physical memory or of a tighter process limit");
    expect(defaultVramHeadroomBytes() == kUnmeasuredHeadroomBytes,
           "before any GPU is measured the VRAM default is the unmeasured placeholder");
    recordDeviceLocalMemoryBytes(24 * gib);
    recordDeviceLocalMemoryBytes(8 * gib);
    expect(defaultVramHeadroomBytes() == defaultHeadroomForCapacity(24 * gib),
           "the VRAM default is 10% of the largest measured device");
    expect(MemoryHeadroom{}.ramBytes == defaultRamHeadroomBytes() &&
               MemoryHeadroom{}.vramBytes == defaultVramHeadroomBytes(),
           "MemoryHeadroom{} carries the computed defaults");
    expect(kRamHeadroomKey == "memory/ramHeadroomBytes" &&
               kVramHeadroomKey == "memory/vramHeadroomBytes",
           "preference keys are stable");
    expect(parseHeadroomBytes("0") == 0u, "zero headroom is valid");
    expect(parseHeadroomBytes("4294967296") == 4 * gib, "decimal bytes parse exactly");
    expect(parseHeadroomBytes("274877906944") == 256 * gib, "256 GiB is the inclusive maximum");
    expect(!parseHeadroomBytes("274877906945"), "above the maximum is no value");
    expect(!parseHeadroomBytes(""), "missing text is no value");
    expect(!parseHeadroomBytes("-1"), "negative text is no value");
    expect(!parseHeadroomBytes("12 GiB"), "units are not accepted");
    expect(!parseHeadroomBytes("99999999999999999999999"), "overflowing text is no value");
    expect(isValidHeadroomBytes(0) && !isValidHeadroomBytes(257 * gib), "validation range");
    if (failures != 0) {
        std::cerr << failures << " memory headroom expectation(s) failed\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
