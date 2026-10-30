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
    expect(MemoryHeadroom{}.ramBytes == 2 * gib && MemoryHeadroom{}.vramBytes == 2 * gib,
           "both headroom defaults are 2 GiB");
    expect(kRamHeadroomKey == "memory/ramHeadroomBytes" &&
               kVramHeadroomKey == "memory/vramHeadroomBytes",
           "preference keys are stable");
    expect(parseHeadroomBytes("0") == 0, "zero headroom is valid");
    expect(parseHeadroomBytes("4294967296") == 4 * gib, "decimal bytes parse exactly");
    expect(parseHeadroomBytes("274877906944") == 256 * gib, "256 GiB is the inclusive maximum");
    expect(parseHeadroomBytes("274877906945") == 2 * gib, "above the maximum selects the default");
    expect(parseHeadroomBytes("") == 2 * gib, "missing text selects the default");
    expect(parseHeadroomBytes("-1") == 2 * gib, "negative text selects the default");
    expect(parseHeadroomBytes("12 GiB") == 2 * gib, "units are not accepted");
    expect(parseHeadroomBytes("99999999999999999999999") == 2 * gib,
           "overflowing text selects the default");
    expect(isValidHeadroomBytes(0) && !isValidHeadroomBytes(257 * gib), "validation range");
    if (failures != 0) {
        std::cerr << failures << " memory headroom expectation(s) failed\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
